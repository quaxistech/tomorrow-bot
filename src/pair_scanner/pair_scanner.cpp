#include "pair_scanner.hpp"
#include "config/config_types.hpp"

#include <boost/json.hpp>
#include <algorithm>
#include <chrono>
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
                         std::shared_ptr<logging::ILogger> logger)
    : config_(std::move(config))
    , rest_client_(std::move(rest_client))
    , logger_(std::move(logger))
{
}

PairScanner::~PairScanner() {
    stop_rotation();
}

// ========== Основной API ==========

ScanResult PairScanner::scan() {
    logger_->info(kComp, "Начало сканирования торговых пар...",
        {{"mode", config_.mode == config::PairSelectionMode::Auto ? "auto" : "manual"},
         {"top_n", std::to_string(config_.top_n)}});

    ScanResult result;
    auto now = std::chrono::system_clock::now();
    result.scanned_at_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();

    // Ручной режим — берём символы из конфига, не сканируем
    if (config_.mode == config::PairSelectionMode::Manual) {
        result.selected = config_.manual_symbols;
        logger_->info(kComp, "Ручной режим: " + std::to_string(result.selected.size()) + " пар");
        std::lock_guard lock(mutex_);
        last_result_ = result;
        return result;
    }

    // === Автоматический режим ===

    // Шаг 1: Получить список всех торговых пар
    auto pairs = fetch_symbols();
    result.total_pairs_found = static_cast<int>(pairs.size());
    logger_->info(kComp, "Загружено пар с биржи: " + std::to_string(pairs.size()));

    if (pairs.empty()) {
        logger_->error(kComp, "Не удалось загрузить список пар");
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
    auto tickers = fetch_tickers();
    logger_->info(kComp, "Загружено тикеров: " + std::to_string(tickers.size()));

    if (tickers.empty()) {
        logger_->error(kComp, "Не удалось загрузить тикеры");
        return result;
    }

    // Шаг 3: Фильтрация кандидатов
    auto candidates = filter_candidates(pairs, tickers);
    result.pairs_after_filter = static_cast<int>(candidates.size());
    logger_->info(kComp, "Кандидатов после фильтрации: " + std::to_string(candidates.size()),
        {{"min_volume", std::to_string(config_.min_volume_usdt)},
         {"max_spread", std::to_string(config_.max_spread_bps)}});

    if (candidates.empty()) {
        logger_->warn(kComp, "Нет подходящих пар после фильтрации");
        return result;
    }

    // Шаг 4: Для каждого кандидата загружаем свечи и считаем скоринг
    // КРИТИЧЕСКИ ВАЖНО: сортируем по 24h change (убывание), чтобы
    // РАСТУЩИЕ монеты получили приоритет на загрузку свечей.
    // Раньше сортировали по volume — из-за этого BTC всегда был в топ-30.
    std::sort(candidates.begin(), candidates.end(),
        [](const TickerData& a, const TickerData& b) {
            return a.change_24h_pct > b.change_24h_pct;
        });

    const size_t kMaxCandidatesForCandles = 30;
    if (candidates.size() > kMaxCandidatesForCandles) {
        candidates.resize(kMaxCandidatesForCandles);
    }

    logger_->info(kComp,
        "Загрузка свечей для " + std::to_string(candidates.size()) + " кандидатов...");

    for (const auto& ticker : candidates) {
        auto candles = fetch_candles(ticker.symbol, 48);  // 48 часовых свечей

        auto score = scorer_.score(ticker, candles);

        // Пропускаем отфильтрованные монеты (24h change < -1%)
        if (score.filtered_out) continue;

        // Заполняем precision из кеша symbol_info_
        {
            std::lock_guard lock(mutex_);
            auto it = symbol_info_.find(ticker.symbol);
            if (it != symbol_info_.end()) {
                score.quantity_precision = static_cast<int>(it->second.quantity_precision);
                score.price_precision = static_cast<int>(it->second.price_precision);
            }
        }

        result.ranked_pairs.push_back(std::move(score));
    }

    // Шаг 5: Сортировка по total_score (убывание)
    std::sort(result.ranked_pairs.begin(), result.ranked_pairs.end(),
        [](const PairScore& a, const PairScore& b) {
            return a.total_score > b.total_score;
        });

    // Шаг 6: Выбор топ-N
    int top_n = std::min(config_.top_n, static_cast<int>(result.ranked_pairs.size()));
    for (int i = 0; i < top_n; ++i) {
        result.selected.push_back(result.ranked_pairs[i].symbol);
    }

    // Лог результатов (формат v3: Mom/Trend/Trade/Qual)
    logger_->info(kComp, "=== РЕЗУЛЬТАТЫ СКАНИРОВАНИЯ v3 (ТОЛЬКО РАСТУЩИЕ) ===");
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
        bool selected = (i < top_n);
        logger_->info(kComp, oss.str(),
            {{"rank", std::to_string(i + 1)},
             {"selected", selected ? "YES" : "no"}});
    }

    logger_->info(kComp, "Выбрано " + std::to_string(top_n) + " пар для торговли");

    std::lock_guard lock(mutex_);
    last_result_ = result;
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

// ========== Загрузка данных с биржи ==========

std::vector<PairInfo> PairScanner::fetch_symbols() {
    std::vector<PairInfo> result;

    try {
        auto resp = rest_client_->get(kSymbolsPath, "");
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
    } catch (const std::exception& e) {
        logger_->error(kComp, "Исключение при загрузке символов: " + std::string(e.what()));
    }

    return result;
}

std::vector<TickerData> PairScanner::fetch_tickers() {
    std::vector<TickerData> result;

    try {
        auto resp = rest_client_->get(kTickersPath, "");
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
    } catch (const std::exception& e) {
        logger_->error(kComp, "Исключение при загрузке тикеров: " + std::string(e.what()));
    }

    return result;
}

std::vector<CandleData> PairScanner::fetch_candles(const std::string& symbol, int limit) {
    std::vector<CandleData> result;

    try {
        std::string query = "symbol=" + symbol + "&granularity=1h&limit=" + std::to_string(limit);
        auto resp = rest_client_->get(kCandlesPath, query);
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
    } catch (const std::exception& e) {
        logger_->debug(kComp, "Исключение при загрузке свечей " + symbol + ": " + e.what());
    }

    return result;
}

std::vector<TickerData> PairScanner::filter_candidates(
    const std::vector<PairInfo>& pairs,
    const std::vector<TickerData>& tickers) const
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
        // Пара должна быть в списке валидных
        if (!valid_symbols.contains(t.symbol)) continue;

        // Не в blacklist
        if (blacklisted.contains(t.symbol)) continue;

        // Минимальный объём
        if (t.quote_volume_24h < config_.min_volume_usdt) continue;

        // Максимальный спред
        if (t.spread_bps > config_.max_spread_bps && config_.max_spread_bps > 0.0) continue;

        // Цена должна быть валидной
        if (t.last_price <= 0.0) continue;

        result.push_back(t);
    }

    return result;
}

} // namespace tb::pair_scanner
