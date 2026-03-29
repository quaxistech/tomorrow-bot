#include "pair_scanner.hpp"
#include "data_validator.hpp"
#include "config/config_types.hpp"

#include <boost/json.hpp>
#include <algorithm>
#include <chrono>
#include <random>
#include <set>
#include <sstream>
#include <iomanip>

namespace tb::pair_scanner {

static constexpr char kComp[] = "PairScanner";

// Bitget API эндпоинты
static constexpr char kSymbolsPath[]  = "/api/v2/spot/public/symbols";
static constexpr char kTickersPath[]  = "/api/v2/spot/market/tickers";
static constexpr char kCandlesPath[]  = "/api/v2/spot/market/candles";

/// Утилита для парсинга числовых полей Bitget API (приходят как строки)
static double parse_double(const boost::json::value& v) {
    if (v.is_string()) {
        auto s = std::string(v.as_string());
        return s.empty() ? 0.0 : std::stod(s);
    }
    if (v.is_double()) return v.as_double();
    if (v.is_int64()) return static_cast<double>(v.as_int64());
    return 0.0;
}

/// Безопасно извлечь строковое поле
static std::string parse_string(const boost::json::value& v) {
    if (v.is_string()) return std::string(v.as_string());
    return "";
}

// ========== Конструктор / Деструктор ==========

PairScanner::PairScanner(config::PairSelectionConfig config,
                         std::shared_ptr<exchange::bitget::BitgetRestClient> rest_client,
                         std::shared_ptr<logging::ILogger> logger,
                         std::shared_ptr<metrics::IMetricsRegistry> metrics_registry,
                         std::shared_ptr<health::IHealthService> health,
                         std::shared_ptr<persistence::IStorageAdapter> storage)
    : config_(std::move(config))
    , rest_client_(std::move(rest_client))
    , logger_(std::move(logger))
    , scorer_(config_.scorer)
    , metrics_(metrics_registry)
    , diversification_(DiversificationConfig{
          config_.max_correlation_in_basket,
          config_.max_pairs_per_sector,
          config_.min_liquidity_depth_usdt,
          config_.enable_diversification})
    , retry_policy_(RetryPolicy{config_.api_retry_max, config_.api_retry_base_delay_ms})
    , circuit_breaker_(config_.circuit_breaker_threshold, config_.circuit_breaker_reset_ms)
    , health_(std::move(health))
    , storage_(std::move(storage))
{
    if (health_) {
        health_->register_subsystem("pair_scanner");
        health_->update_subsystem("pair_scanner", health::SubsystemState::Starting,
                                  "Инициализация сканера пар");
    }
}

PairScanner::~PairScanner() {
    stop_rotation();
}

// ========== UUID ==========

std::string PairScanner::generate_scan_id() {
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();
    thread_local std::mt19937 gen(std::random_device{}());
    std::uniform_int_distribution<> dist(0x1000, 0xFFFF);
    std::ostringstream oss;
    oss << "scan-" << ms << "-" << std::hex << dist(gen);
    return oss.str();
}

// ========== Основной API ==========

ScanResult PairScanner::scan() {
    ScanContext ctx;
    ctx.scan_id = generate_scan_id();
    auto scan_start = std::chrono::system_clock::now();
    ctx.started_at_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        scan_start.time_since_epoch()).count();

    logger_->info(kComp, "Начало сканирования торговых пар...",
        {{"mode", config_.mode == config::PairSelectionMode::Auto ? "auto" : "manual"},
         {"top_n", std::to_string(config_.top_n)},
         {"scan_id", ctx.scan_id}});

    // Capture previous selection for turnover calculation
    std::vector<std::string> prev_selected;
    {
        std::lock_guard lock(mutex_);
        prev_selected = last_result_.selected;
    }

    ScanResult result;
    result.scanned_at_ms = ctx.started_at_ms;

    // Ручной режим — берём символы из конфига, не сканируем
    if (config_.mode == config::PairSelectionMode::Manual) {
        result.selected = config_.manual_symbols;
        result.context = ctx;
        logger_->info(kComp, "Ручной режим: " + std::to_string(result.selected.size()) + " пар");
        std::lock_guard lock(mutex_);
        last_result_ = result;
        return result;
    }

    // === Автоматический режим ===

    // Шаг 1: Получить список всех торговых пар
    auto pairs = fetch_symbols(ctx);
    result.total_pairs_found = static_cast<int>(pairs.size());
    logger_->info(kComp, "Загружено пар с биржи: " + std::to_string(pairs.size()));

    if (pairs.empty()) {
        logger_->error(kComp, "Не удалось загрузить список пар");
        ctx.degraded_mode = true;
        result.context = ctx;
        result.api_errors = ctx.api_failures;
        metrics_.record_scan_failure();
        update_health(result);
        return result;
    }

    // Кешируем symbol → PairInfo для precision данных
    {
        std::lock_guard lock(mutex_);
        symbol_info_.clear();
        for (const auto& p : pairs) {
            symbol_info_[p.symbol] = p;
        }
    }

    // Шаг 2: Получить 24ч тикеры
    auto tickers = fetch_tickers(ctx);
    logger_->info(kComp, "Загружено тикеров: " + std::to_string(tickers.size()));

    if (tickers.empty()) {
        logger_->error(kComp, "Не удалось загрузить тикеры");
        ctx.degraded_mode = true;
        result.context = ctx;
        result.api_errors = ctx.api_failures;
        metrics_.record_scan_failure();
        update_health(result);
        return result;
    }

    // Шаг 3: Фильтрация кандидатов с отслеживанием отклонённых
    std::vector<PairScore> rejected;
    auto candidates = filter_candidates(pairs, tickers, rejected);
    result.pairs_after_filter = static_cast<int>(candidates.size());
    result.rejected_pairs = std::move(rejected);

    logger_->info(kComp, "Кандидатов после фильтрации: " + std::to_string(candidates.size()),
        {{"min_volume", std::to_string(config_.min_volume_usdt)},
         {"max_spread", std::to_string(config_.max_spread_bps)},
         {"rejected", std::to_string(result.rejected_pairs.size())}});

    if (candidates.empty()) {
        logger_->warn(kComp, "Нет подходящих пар после фильтрации");
        ctx.degraded_mode = true;
        result.context = ctx;
        result.api_errors = ctx.api_failures;
        metrics_.record_empty_scan();
        update_health(result);
        return result;
    }

    // Шаг 4: Сортируем по 24h VOLUME (убывание): берём самые ликвидные пары
    // для загрузки свечей. Не сортируем по 24h change — это отбирает
    // монеты, которые УЖЕ pumped (selection bias / buying at peaks).
    // Научный отбор (ускорение, fresh start) делается в scorer.
    std::sort(candidates.begin(), candidates.end(),
        [](const TickerData& a, const TickerData& b) {
            return a.quote_volume_24h > b.quote_volume_24h;
        });

    const int max_for_candles = config_.max_candidates_for_candles;
    if (static_cast<int>(candidates.size()) > max_for_candles) {
        candidates.resize(static_cast<size_t>(max_for_candles));
    }

    logger_->info(kComp,
        "Загрузка свечей для " + std::to_string(candidates.size()) + " кандидатов...");

    // Шаг 5: Параллельная загрузка свечей
    auto candles_map = fetch_candles_parallel(candidates, ctx);

    // Шаг 6: Скоринг каждого кандидата с валидацией качества данных
    for (const auto& ticker : candidates) {
        auto it = candles_map.find(ticker.symbol);
        const auto& candles = (it != candles_map.end()) ? it->second : std::vector<CandleData>{};

        auto score = scorer_.score(ticker, candles);

        // Валидация качества данных
        score.data_quality = DataValidator::validate(ticker, candles, config_.candle_history_hours);

        if (score.filtered_out) {
            score.filter_verdict = FilterVerdict{FilterReason::NegativeChange,
                "Отфильтрована scorer-ом (24h change / exhausted pump)"};
            result.rejected_pairs.push_back(std::move(score));
            continue;
        }

        if (!score.data_quality.is_acceptable()) {
            score.filter_verdict = FilterVerdict{FilterReason::InsufficientData,
                "Недостаточное качество данных (completeness=" +
                std::to_string(score.data_quality.completeness_ratio) + ")"};
            ctx.degraded_mode = true;
        }

        // Заполняем precision из кеша symbol_info_
        {
            std::lock_guard lock(mutex_);
            auto info_it = symbol_info_.find(ticker.symbol);
            if (info_it != symbol_info_.end()) {
                score.quantity_precision = static_cast<int>(info_it->second.quantity_precision);
                score.price_precision = static_cast<int>(info_it->second.price_precision);
                score.min_trade_usdt = info_it->second.min_trade_amount > 0.0
                    ? info_it->second.min_trade_amount : 1.0;
            }
        }

        result.ranked_pairs.push_back(std::move(score));
    }

    // Шаг 7: Сортировка по total_score (убывание)
    std::sort(result.ranked_pairs.begin(), result.ranked_pairs.end(),
        [](const PairScore& a, const PairScore& b) {
            return a.total_score > b.total_score;
        });

    // Шаг 8: Применение диверсификации вместо наивного top-N
    result.selected = diversification_.apply(result.ranked_pairs, candles_map, config_.top_n);

    int top_n = static_cast<int>(result.selected.size());

    // Лог результатов (формат v4: Mom/Trend/Trade/Qual — acceleration-based)
    logger_->info(kComp, "=== РЕЗУЛЬТАТЫ СКАНИРОВАНИЯ v4 (ACCELERATION-BASED) ===");
    for (int i = 0; i < static_cast<int>(result.ranked_pairs.size()) && i < 10; ++i) {
        const auto& s = result.ranked_pairs[i];
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(1);
        oss << "#" << (i + 1) << " " << s.symbol
            << " | Score: " << s.total_score
            << " (Mom:" << s.momentum_score
            << " Trend:" << s.trend_score
            << " Trade:" << s.tradability_score
            << " Qual:" << s.quality_score << ")"
            << " | 24h:" << std::showpos << std::setprecision(2) << s.change_24h_pct << "%"
            << std::noshowpos
            << " | Vol24h: $" << std::setprecision(0) << s.quote_volume_24h;

        // Check if this symbol is in the selected set
        bool is_selected = std::find(result.selected.begin(), result.selected.end(),
                                     s.symbol) != result.selected.end();
        logger_->info(kComp, oss.str(),
            {{"rank", std::to_string(i + 1)},
             {"selected", is_selected ? "YES" : "no"}});
    }

    logger_->info(kComp, "Выбрано " + std::to_string(top_n) + " пар для торговли",
        {{"scan_id", ctx.scan_id},
         {"diversification", config_.enable_diversification ? "on" : "off"}});

    // Шаг 9: Завершение контекста сканирования
    auto scan_end = std::chrono::system_clock::now();
    ctx.finished_at_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        scan_end.time_since_epoch()).count();
    ctx.duration_ms = ctx.finished_at_ms - ctx.started_at_ms;
    if (ctx.api_failures > 0) {
        ctx.degraded_mode = true;
    }
    result.context = ctx;
    result.api_errors = ctx.api_failures;

    // Шаг 10: Метрики
    metrics_.observe_scan_duration(static_cast<double>(ctx.duration_ms));
    metrics_.update_counts(static_cast<int>(result.ranked_pairs.size()), top_n);
    metrics_.observe_scores(result.ranked_pairs);
    double turnover = ScanMetrics::compute_turnover(prev_selected, result.selected);
    metrics_.update_turnover(turnover);
    metrics_.update_circuit_breaker(circuit_breaker_.current_state() == CircuitState::Open);

    if (result.selected.empty()) {
        metrics_.record_empty_scan();
    }

    // Шаг 11: Health
    update_health(result);

    // Шаг 12: Persistence
    persist_result(result);

    // Шаг 13: Сохранить последний результат
    {
        std::lock_guard lock(mutex_);
        last_result_ = result;
    }

    logger_->info(kComp, "Сканирование завершено",
        {{"scan_id", ctx.scan_id},
         {"duration_ms", std::to_string(ctx.duration_ms)},
         {"selected", std::to_string(top_n)},
         {"degraded", ctx.degraded_mode ? "true" : "false"},
         {"turnover", std::to_string(turnover)}});

    return result;
}

std::vector<std::string> PairScanner::selected_symbols() const {
    std::lock_guard lock(mutex_);
    return last_result_.selected;
}

ScanResult PairScanner::last_result() const {
    std::lock_guard lock(mutex_);
    return last_result_;
}

std::pair<int, int> PairScanner::symbol_precision(const std::string& symbol) const {
    std::lock_guard lock(mutex_);
    auto it = symbol_info_.find(symbol);
    if (it != symbol_info_.end()) {
        return {static_cast<int>(it->second.quantity_precision),
                static_cast<int>(it->second.price_precision)};
    }
    return {6, 2};  // defaults
}

// ========== Ротация ==========

void PairScanner::start_rotation(RotationCallback on_rotation) {
    if (rotation_running_.exchange(true)) return;  // Уже запущена
    rotation_callback_ = std::move(on_rotation);
    rotation_thread_ = std::thread([this] { rotation_loop(); });
    logger_->info(kComp, "Ротация запущена",
        {{"interval_hours", std::to_string(config_.rotation_interval_hours)}});
}

void PairScanner::stop_rotation() {
    if (!rotation_running_.exchange(false)) return;
    if (rotation_thread_.joinable()) {
        rotation_thread_.join();
    }
    logger_->info(kComp, "Ротация остановлена");
}

void PairScanner::rotation_loop() {
    while (rotation_running_.load()) {
        // Ждём интервал ротации (проверяем running каждые 10 секунд)
        int total_seconds = config_.rotation_interval_hours * 3600;
        for (int elapsed = 0; elapsed < total_seconds && rotation_running_.load(); elapsed += 10) {
            std::this_thread::sleep_for(std::chrono::seconds(10));
        }

        if (!rotation_running_.load()) break;

        logger_->info(kComp, "Запуск плановой ротации пар...");
        try {
            auto result = scan();
            if (!result.selected.empty() && rotation_callback_) {
                rotation_callback_(result.selected);
            }
        } catch (const std::exception& e) {
            logger_->error(kComp, "Ошибка при ротации: " + std::string(e.what()));
        }
    }
}

// ========== Retry / Circuit Breaker ==========

exchange::bitget::RestResponse PairScanner::fetch_with_retry(
    const std::string& path, const std::string& query,
    ScanContext& ctx, const std::string& endpoint_name)
{
    if (!circuit_breaker_.allow_request()) {
        ctx.api_failures++;
        ctx.failure_details.push_back("Circuit breaker open for " + endpoint_name);
        metrics_.record_api_failure(endpoint_name);
        return {0, "", false, "Circuit breaker open"};
    }

    for (int attempt = 0; attempt <= retry_policy_.max_retries; ++attempt) {
        if (attempt > 0) {
            int delay = retry_policy_.delay_for_attempt(attempt - 1);
            std::this_thread::sleep_for(std::chrono::milliseconds(delay));
            ctx.api_retries++;
            metrics_.record_api_retry();
        }

        auto resp = rest_client_->get(path, query);
        if (resp.success) {
            circuit_breaker_.record_success();
            return resp;
        }

        auto err_class = classify_http_error(resp.status_code);
        if (err_class == ErrorClass::Permanent) {
            circuit_breaker_.record_failure();
            ctx.api_failures++;
            ctx.failure_details.push_back(endpoint_name + " permanent error: " +
                std::to_string(resp.status_code) + " " +
                resp.error_message.substr(0, 200));
            metrics_.record_api_failure(endpoint_name);
            return resp;
        }

        logger_->debug(kComp, "Retry " + std::to_string(attempt + 1) + "/" +
            std::to_string(retry_policy_.max_retries) + " для " + endpoint_name,
            {{"status", std::to_string(resp.status_code)}});
    }

    // All retries exhausted
    circuit_breaker_.record_failure();
    ctx.api_failures++;
    ctx.failure_details.push_back(endpoint_name + " failed after " +
        std::to_string(retry_policy_.max_retries) + " retries");
    metrics_.record_api_failure(endpoint_name);
    metrics_.update_circuit_breaker(circuit_breaker_.current_state() == CircuitState::Open);
    return {0, "", false, "All retries exhausted"};
}

// ========== Загрузка данных с биржи ==========

std::vector<PairInfo> PairScanner::fetch_symbols(ScanContext& ctx) {
    std::vector<PairInfo> result;

    try {
        auto resp = fetch_with_retry(kSymbolsPath, "", ctx, "symbols");
        if (!resp.success) {
            logger_->error(kComp, "Ошибка загрузки символов: " + resp.error_message);
            return result;
        }

        auto doc = boost::json::parse(resp.body);
        auto& obj = doc.as_object();

        if (obj.at("code").as_string() != "00000") {
            logger_->error(kComp, "API ошибка (symbols)",
                {{"code", parse_string(obj.at("code"))}});
            return result;
        }

        auto& data = obj.at("data").as_array();
        for (const auto& item : data) {
            auto& o = item.as_object();
            PairInfo info;
            info.symbol = parse_string(o.at("symbol"));
            info.base_coin = parse_string(o.at("baseCoin"));
            info.quote_coin = parse_string(o.at("quoteCoin"));
            info.status = parse_string(o.at("status"));

            // Фильтруем: только USDT пары со статусом online
            if (info.quote_coin != "USDT") continue;
            if (info.status != "online") continue;

            if (o.contains("minTradeUSDT")) {
                info.min_trade_amount = parse_double(o.at("minTradeUSDT"));
            }
            if (o.contains("pricePrecision")) {
                info.price_precision = parse_double(o.at("pricePrecision"));
            }
            if (o.contains("quantityPrecision")) {
                info.quantity_precision = parse_double(o.at("quantityPrecision"));
            }

            result.push_back(std::move(info));
        }

        ctx.symbols_fetched = static_cast<int>(result.size());
    } catch (const std::exception& e) {
        logger_->error(kComp, "Исключение при загрузке символов: " + std::string(e.what()));
    }

    return result;
}

std::vector<TickerData> PairScanner::fetch_tickers(ScanContext& ctx) {
    std::vector<TickerData> result;

    try {
        auto resp = fetch_with_retry(kTickersPath, "", ctx, "tickers");
        if (!resp.success) {
            logger_->error(kComp, "Ошибка загрузки тикеров: " + resp.error_message);
            return result;
        }

        auto doc = boost::json::parse(resp.body);
        auto& obj = doc.as_object();

        if (obj.at("code").as_string() != "00000") {
            logger_->error(kComp, "API ошибка (tickers)",
                {{"code", parse_string(obj.at("code"))}});
            return result;
        }

        auto& data = obj.at("data").as_array();
        for (const auto& item : data) {
            auto& o = item.as_object();
            TickerData td;
            td.symbol = parse_string(o.at("symbol"));

            // Основные поля тикера
            if (o.contains("lastPr"))      td.last_price       = parse_double(o.at("lastPr"));
            if (o.contains("high24h"))     td.high_24h         = parse_double(o.at("high24h"));
            if (o.contains("low24h"))      td.low_24h          = parse_double(o.at("low24h"));
            if (o.contains("open"))        td.open_24h         = parse_double(o.at("open"));
            if (o.contains("change24h"))   td.change_24h_pct   = parse_double(o.at("change24h")) * 100.0;
            if (o.contains("baseVolume"))  td.volume_24h       = parse_double(o.at("baseVolume"));
            if (o.contains("quoteVolume")) td.quote_volume_24h = parse_double(o.at("quoteVolume"));
            if (o.contains("bidPr"))       td.best_bid         = parse_double(o.at("bidPr"));
            if (o.contains("askPr"))       td.best_ask         = parse_double(o.at("askPr"));

            // Рассчитать спред в базисных пунктах
            if (td.best_bid > 0.0 && td.best_ask > 0.0) {
                double mid = (td.best_bid + td.best_ask) / 2.0;
                td.spread_bps = (td.best_ask - td.best_bid) / mid * 10000.0;
            }

            result.push_back(std::move(td));
        }

        ctx.tickers_fetched = static_cast<int>(result.size());
    } catch (const std::exception& e) {
        logger_->error(kComp, "Исключение при загрузке тикеров: " + std::string(e.what()));
    }

    return result;
}

std::vector<CandleData> PairScanner::fetch_candles(const std::string& symbol, int limit,
                                                    ScanContext& ctx) {
    std::vector<CandleData> result;

    auto fetch_start = std::chrono::steady_clock::now();

    try {
        std::string query = "symbol=" + symbol + "&granularity=1h&limit=" + std::to_string(limit);
        auto resp = fetch_with_retry(kCandlesPath, query, ctx, "candles/" + symbol);
        if (!resp.success) {
            logger_->debug(kComp, "Ошибка загрузки свечей для " + symbol);
            return result;
        }

        auto doc = boost::json::parse(resp.body);
        auto& obj = doc.as_object();

        if (obj.at("code").as_string() != "00000") {
            return result;
        }

        auto& data = obj.at("data").as_array();
        for (const auto& item : data) {
            auto& arr = item.as_array();
            if (arr.size() < 6) continue;

            CandleData cd;
            cd.timestamp_ms = static_cast<int64_t>(parse_double(arr[0]));
            cd.open   = parse_double(arr[1]);
            cd.high   = parse_double(arr[2]);
            cd.low    = parse_double(arr[3]);
            cd.close  = parse_double(arr[4]);
            cd.volume = parse_double(arr[5]);
            result.push_back(cd);
        }

        // Bitget возвращает от новых к старым — переворачиваем
        std::reverse(result.begin(), result.end());

        ctx.candles_fetched += static_cast<int>(result.size());
    } catch (const std::exception& e) {
        logger_->debug(kComp, "Исключение при загрузке свечей " + symbol + ": " + e.what());
    }

    auto fetch_end = std::chrono::steady_clock::now();
    double latency_ms = std::chrono::duration<double, std::milli>(fetch_end - fetch_start).count();
    metrics_.observe_candle_fetch(latency_ms);

    return result;
}

// ========== Параллельная загрузка свечей ==========

std::unordered_map<std::string, std::vector<CandleData>>
PairScanner::fetch_candles_parallel(const std::vector<TickerData>& candidates, ScanContext& ctx) {
    std::unordered_map<std::string, std::vector<CandleData>> result;
    int concurrency = std::max(1, config_.candle_fetch_concurrency);
    int limit = config_.candle_history_hours;

    // Mutex for thread-safe ScanContext updates from parallel fetches
    std::mutex ctx_mutex;

    size_t i = 0;
    while (i < candidates.size()) {
        std::vector<std::future<std::pair<std::string, std::vector<CandleData>>>> futures;
        for (int j = 0; j < concurrency && i < candidates.size(); ++j, ++i) {
            const auto& sym = candidates[i].symbol;
            futures.push_back(std::async(std::launch::async,
                [this, sym, limit, &ctx, &ctx_mutex]()
                    -> std::pair<std::string, std::vector<CandleData>> {
                    // Use a local context to avoid data races on shared ctx
                    ScanContext local_ctx;
                    auto candles = fetch_candles(sym, limit, local_ctx);

                    // Merge local stats back into shared ctx under lock
                    {
                        std::lock_guard lock(ctx_mutex);
                        ctx.candles_fetched += local_ctx.candles_fetched;
                        ctx.api_failures += local_ctx.api_failures;
                        ctx.api_retries += local_ctx.api_retries;
                        ctx.failure_details.insert(ctx.failure_details.end(),
                            local_ctx.failure_details.begin(),
                            local_ctx.failure_details.end());
                    }

                    return {sym, std::move(candles)};
                }));
        }
        for (auto& f : futures) {
            auto [sym, candles] = f.get();
            if (!candles.empty()) {
                result[sym] = std::move(candles);
            }
        }
    }

    return result;
}

// ========== Фильтрация ==========

std::vector<TickerData> PairScanner::filter_candidates(
    const std::vector<PairInfo>& pairs,
    const std::vector<TickerData>& tickers,
    std::vector<PairScore>& rejected) const
{
    // Множество допустимых символов (online USDT-пары)
    std::set<std::string> valid_symbols;
    for (const auto& p : pairs) {
        valid_symbols.insert(p.symbol);
    }

    // Множество запрещённых символов (blacklist)
    std::set<std::string> blacklisted(config_.blacklist.begin(), config_.blacklist.end());

    std::vector<TickerData> result;
    for (const auto& t : tickers) {
        auto make_rejected = [&](FilterReason reason, const std::string& details) {
            PairScore ps;
            ps.symbol = t.symbol;
            ps.filtered_out = true;
            ps.quote_volume_24h = t.quote_volume_24h;
            ps.change_24h_pct = t.change_24h_pct;
            ps.filter_verdict = FilterVerdict{reason, details};
            rejected.push_back(std::move(ps));
        };

        // Пара должна быть в списке валидных
        if (!valid_symbols.contains(t.symbol)) {
            make_rejected(FilterReason::NotOnline, "Символ не найден среди online USDT-пар");
            continue;
        }

        // Не в blacklist
        if (blacklisted.contains(t.symbol)) {
            make_rejected(FilterReason::Blacklisted, "В чёрном списке");
            continue;
        }

        // Минимальный объём
        if (t.quote_volume_24h < config_.min_volume_usdt) {
            make_rejected(FilterReason::BelowMinVolume,
                "Объём " + std::to_string(t.quote_volume_24h) +
                " < " + std::to_string(config_.min_volume_usdt));
            continue;
        }

        // Максимальный спред
        if (t.spread_bps > config_.max_spread_bps && config_.max_spread_bps > 0.0) {
            make_rejected(FilterReason::AboveMaxSpread,
                "Спред " + std::to_string(t.spread_bps) +
                " bps > " + std::to_string(config_.max_spread_bps));
            continue;
        }

        // Цена должна быть валидной
        if (t.last_price <= 0.0) {
            make_rejected(FilterReason::InvalidPrice, "Невалидная цена: " +
                std::to_string(t.last_price));
            continue;
        }

        result.push_back(t);
    }

    return result;
}

// ========== Persistence ==========

void PairScanner::persist_result(const ScanResult& result) {
    if (!storage_ || !config_.persist_scan_results) return;
    try {
        // Journal entry for audit trail
        persistence::JournalEntry entry;
        entry.type = persistence::JournalEntryType::SystemEvent;
        entry.timestamp = Timestamp{result.context.finished_at_ms * 1'000'000LL};
        entry.correlation_id = CorrelationId{result.context.scan_id};

        // Build payload JSON
        boost::json::object payload;
        payload["scan_id"] = result.context.scan_id;
        payload["duration_ms"] = result.context.duration_ms;
        payload["selected_count"] = static_cast<int>(result.selected.size());
        payload["total_scored"] = static_cast<int>(result.ranked_pairs.size());
        payload["api_errors"] = result.api_errors;
        payload["degraded"] = result.context.degraded_mode;
        boost::json::array sel;
        for (const auto& s : result.selected) sel.push_back(boost::json::value(s));
        payload["selected"] = std::move(sel);
        entry.payload_json = boost::json::serialize(payload);

        storage_->append_journal(entry);

        // Snapshot for recovery
        persistence::SnapshotEntry snap;
        snap.type = persistence::SnapshotType::FullSystem;
        snap.created_at = Timestamp{result.context.finished_at_ms * 1'000'000LL};
        snap.payload_json = entry.payload_json;
        storage_->store_snapshot(snap);
    } catch (const std::exception& e) {
        logger_->warn(kComp, "Ошибка сохранения результата: " + std::string(e.what()));
    }
}

// ========== Health ==========

void PairScanner::update_health(const ScanResult& result) {
    if (!health_) return;

    if (result.selected.empty() && config_.mode == config::PairSelectionMode::Auto) {
        health_->update_subsystem("pair_scanner", health::SubsystemState::Failed,
            "Сканирование не выбрало ни одной пары (api_errors=" +
            std::to_string(result.api_errors) + ")");
    } else if (result.context.degraded_mode) {
        health_->update_subsystem("pair_scanner", health::SubsystemState::Degraded,
            "Сканирование завершено с ошибками (api_errors=" +
            std::to_string(result.api_errors) +
            ", selected=" + std::to_string(result.selected.size()) + ")");
    } else {
        health_->update_subsystem("pair_scanner", health::SubsystemState::Healthy,
            "Выбрано " + std::to_string(result.selected.size()) + " пар за " +
            std::to_string(result.context.duration_ms) + " мс");
    }
}

} // namespace tb::pair_scanner
