/**
 * @file trading_pipeline.cpp
 * @brief Реализация торгового pipeline
 */

#include "pipeline/trading_pipeline.hpp"
#include "exchange/bitget/bitget_rest_client.hpp"
#include "exchange/bitget/bitget_order_submitter.hpp"
#include "normalizer/normalized_events.hpp"
#include "common/enums.hpp"
#include <boost/json.hpp>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cmath>
#include <numeric>

namespace {
/// Парсинг double из JSON значения (строки или числа)
double parse_json_double(const boost::json::value& v) {
    if (v.is_string()) return std::stod(std::string(v.as_string()));
    if (v.is_double()) return v.as_double();
    if (v.is_int64()) return static_cast<double>(v.as_int64());
    return 0.0;
}

/// Извлечь базовую монету из символа (BTCUSDT → BTC, IRYSUSDT → IRYS)
std::string extract_base_coin(const std::string& symbol) {
    constexpr std::string_view kQuote = "USDT";
    if (symbol.size() > kQuote.size() &&
        symbol.compare(symbol.size() - kQuote.size(), kQuote.size(), kQuote) == 0) {
        return symbol.substr(0, symbol.size() - kQuote.size());
    }
    return symbol;  // fallback: вернуть как есть
}
} // anonymous namespace

namespace tb::pipeline {

// ==================== Конструктор ====================

TradingPipeline::TradingPipeline(
    const config::AppConfig& config,
    std::shared_ptr<security::ISecretProvider> secret_provider,
    std::shared_ptr<logging::ILogger> logger,
    std::shared_ptr<clock::IClock> clock,
    std::shared_ptr<metrics::IMetricsRegistry> metrics,
    std::shared_ptr<health::IHealthService> health,
    const std::string& symbol)
    : config_(config)
    , symbol_(Symbol(symbol.empty() ? "BTCUSDT" : symbol))
    , secret_provider_(std::move(secret_provider))
    , logger_(std::move(logger))
    , clock_(std::move(clock))
    , metrics_(std::move(metrics))
    , health_(std::move(health))
{
    // 1. Индикаторы и признаки
    indicator_engine_ = std::make_shared<indicators::IndicatorEngine>(logger_);
    feature_engine_ = std::make_shared<features::FeatureEngine>(
        features::FeatureEngine::Config{}, indicator_engine_, clock_, logger_, metrics_);

    // 2. Стакан ордеров
    order_book_ = std::make_shared<order_book::LocalOrderBook>(symbol_, logger_, metrics_);

    // 3. Аналитический слой
    world_model_ = std::make_shared<world_model::RuleBasedWorldModelEngine>(logger_, clock_);
    regime_engine_ = std::make_shared<regime::RuleBasedRegimeEngine>(logger_, clock_, metrics_);
    uncertainty_engine_ = std::make_shared<uncertainty::RuleBasedUncertaintyEngine>(logger_, clock_);

    // 4. Стратегии
    strategy_registry_ = std::make_shared<strategy::StrategyRegistry>();
    strategy_registry_->register_strategy(
        std::make_shared<strategy::MomentumStrategy>(logger_, clock_));
    strategy_registry_->register_strategy(
        std::make_shared<strategy::MeanReversionStrategy>(logger_, clock_));
    strategy_registry_->register_strategy(
        std::make_shared<strategy::BreakoutStrategy>(logger_, clock_));
    strategy_registry_->register_strategy(
        std::make_shared<strategy::VolExpansionStrategy>(logger_, clock_));
    strategy_registry_->register_strategy(
        std::make_shared<strategy::MicrostructureScalpStrategy>(logger_, clock_));

    // 5. Аллокация стратегий и решения
    strategy_allocator_ = std::make_shared<strategy_allocator::RegimeAwareAllocator>(logger_, clock_);
    decision_engine_ = std::make_shared<decision::CommitteeDecisionEngine>(logger_, clock_);

    // 5.5. Монитор деградации альфы
    alpha_decay_monitor_ = std::make_shared<alpha_decay::AlphaDecayMonitor>();

    // 5.6. Продвинутые features (CUSUM, VPIN, Volume Profile, Time-of-Day)
    advanced_features_ = std::make_shared<features::AdvancedFeatureEngine>(
        features::CusumConfig{}, features::VpinConfig{},
        features::VolumeProfileConfig{}, features::TimeOfDayConfig{},
        logger_);

    // 5.7. ML-модули: байесовская адаптация, фильтр энтропии, fingerprinting
    bayesian_adapter_ = std::make_shared<ml::BayesianAdapter>(
        ml::BayesianConfig{}, logger_);
    entropy_filter_ = std::make_shared<ml::EntropyFilter>(
        ml::EntropyConfig{}, logger_);
    fingerprinter_ = std::make_shared<ml::MicrostructureFingerprinter>(
        ml::FingerprintConfig{}, logger_);

    // 5.8. ML-модули: каскады, корреляции, Thompson Sampling
    cascade_detector_ = std::make_shared<ml::LiquidationCascadeDetector>(
        ml::CascadeConfig{}, logger_);
    correlation_monitor_ = std::make_shared<ml::CorrelationMonitor>(
        ml::CorrelationConfig{}, logger_);
    thompson_sampler_ = std::make_shared<ml::ThompsonSampler>(
        ml::ThompsonConfig{}, logger_);

    // Регистрация ключевых параметров для байесовской адаптации
    {
        ml::BayesianParameter conviction_param;
        conviction_param.name = "conviction_threshold";
        conviction_param.prior_mean = 0.3;
        conviction_param.prior_variance = 0.05;
        conviction_param.min_value = 0.15;
        conviction_param.max_value = 0.6;
        bayesian_adapter_->register_parameter("global", conviction_param);

        ml::BayesianParameter atr_stop_param;
        atr_stop_param.name = "atr_stop_multiplier";
        atr_stop_param.prior_mean = 2.0;
        atr_stop_param.prior_variance = 0.5;
        atr_stop_param.min_value = 1.0;
        atr_stop_param.max_value = 4.0;
        bayesian_adapter_->register_parameter("global", atr_stop_param);
    }

    // 6. Портфель (капитал из конфигурации)
    portfolio_ = std::make_shared<portfolio::InMemoryPortfolioEngine>(
        config_.trading.initial_capital, logger_, clock_, metrics_);

    // 7. Размер позиции (адаптация под размер капитала)
    portfolio_allocator::HierarchicalAllocator::Config alloc_cfg;
    alloc_cfg.budget.global_budget = config_.trading.initial_capital;
    // Для маленьких аккаунтов (< $100) — ослабляем лимиты концентрации
    // и повышаем target_vol, чтобы не блокировать минимальные ордера биржи ($1 USDT)
    if (config_.trading.initial_capital < 100.0) {
        alloc_cfg.max_concentration_pct = 0.80;
        alloc_cfg.max_strategy_allocation_pct = 0.80;
        alloc_cfg.budget.symbol_budget_pct = 0.80;
        alloc_cfg.target_annual_vol = 0.50;       // 50% — адекватнее для крипто
        alloc_cfg.min_size_multiplier = 0.25;     // Не ниже 25% от базового размера
    }
    portfolio_allocator_ = std::make_shared<portfolio_allocator::HierarchicalAllocator>(
        alloc_cfg, logger_);

    // 8. Execution alpha
    execution_alpha_ = std::make_shared<execution_alpha::RuleBasedExecutionAlpha>(
        execution_alpha::RuleBasedExecutionAlpha::Config{}, logger_, clock_, metrics_);

    // 9. Риск-движок
    risk::ExtendedRiskConfig risk_cfg;
    risk_cfg.max_position_notional = config_.risk.max_position_notional;
    risk_cfg.max_daily_loss_pct = config_.risk.max_daily_loss_pct;
    risk_cfg.max_drawdown_pct = config_.risk.max_drawdown_pct;
    risk_cfg.kill_switch_enabled = config_.risk.kill_switch_enabled;
    // Для маленьких аккаунтов — снижаем порог минимальной ликвидности
    if (config_.trading.initial_capital < 100.0) {
        risk_cfg.min_liquidity_depth = 1.0;
    }
    risk_engine_ = std::make_shared<risk::ProductionRiskEngine>(
        risk_cfg, logger_, clock_, metrics_);

    // 10. Исполнение — выбор submitter в зависимости от режима
    std::shared_ptr<execution::IOrderSubmitter> submitter;

    if (config_.trading.mode == TradingMode::Production ||
        config_.trading.mode == TradingMode::Testnet) {
        // Реальная торговля — загружаем API ключи из SecretProvider
        auto key_result = secret_provider_->get_secret(
            security::SecretRef{config_.exchange.api_key_ref});
        auto secret_result = secret_provider_->get_secret(
            security::SecretRef{config_.exchange.api_secret_ref});
        auto pass_result = secret_provider_->get_secret(
            security::SecretRef{config_.exchange.passphrase_ref});

        if (!key_result || !secret_result || !pass_result) {
            logger_->critical("pipeline",
                "Не удалось загрузить API ключи для реальной торговли");
            throw std::runtime_error("API ключи не найдены");
        }

        auto rest_client = std::make_shared<exchange::bitget::BitgetRestClient>(
            config_.exchange.endpoint_rest,
            *key_result, *secret_result, *pass_result,
            logger_, config_.exchange.timeout_ms);

        rest_client_ = rest_client;  // Сохраняем для запроса баланса

        auto bitget_sub = std::make_shared<exchange::bitget::BitgetOrderSubmitter>(
            rest_client, logger_);
        bitget_submitter_ = bitget_sub;  // Сохраняем для set_symbol_precision
        submitter = bitget_sub;

        logger_->info("pipeline",
            "Режим исполнения: РЕАЛЬНАЯ БИРЖА (Bitget REST API)");
    } else {
        // Paper/Shadow — локальная симуляция
        submitter = std::make_shared<execution::PaperOrderSubmitter>();
        logger_->info("pipeline",
            "Режим исполнения: Paper (локальная симуляция)");
    }

    execution_engine_ = std::make_shared<execution::ExecutionEngine>(
        submitter, portfolio_, logger_, clock_, metrics_);

    // 10a. Smart TWAP — адаптивное нарезание крупных ордеров
    twap_executor_ = std::make_shared<execution::SmartTwapExecutor>(
        execution::TwapConfig{}, logger_);

    // 11. Шлюз рыночных данных
    market_data::GatewayConfig gw_cfg;
    gw_cfg.ws_config.url = config_.exchange.endpoint_ws;
    gw_cfg.symbols = {symbol_};

    gateway_ = std::make_shared<market_data::MarketDataGateway>(
        gw_cfg, feature_engine_, order_book_, health_, logger_, metrics_, clock_,
        [this](features::FeatureSnapshot snap) { on_feature_snapshot(std::move(snap)); });

    logger_->info("pipeline", "Торговый pipeline создан",
        {{"symbol", symbol_.get()},
         {"strategies", std::to_string(strategy_registry_->active().size())}});
}

TradingPipeline::~TradingPipeline() {
    stop();
}

void TradingPipeline::set_symbol_precision(int quantity_precision, int price_precision) {
    if (bitget_submitter_) {
        bitget_submitter_->set_symbol_precision(quantity_precision, price_precision);
        logger_->info("pipeline", "Установлена точность ордеров",
            {{"symbol", symbol_.get()},
             {"qty_precision", std::to_string(quantity_precision)},
             {"price_precision", std::to_string(price_precision)}});
    }
}

// ==================== Загрузка точности ордеров ====================

void TradingPipeline::fetch_symbol_precision() {
    if (!rest_client_ || !bitget_submitter_) return;

    try {
        auto resp = rest_client_->get("/api/v2/spot/public/symbols",
                                       "symbol=" + symbol_.get());
        if (!resp.success) {
            logger_->warn("pipeline", "Не удалось запросить precision для " + symbol_.get());
            return;
        }

        auto doc = boost::json::parse(resp.body);
        auto& obj = doc.as_object();
        if (obj.at("code").as_string() != "00000") return;

        auto& data = obj.at("data").as_array();
        for (const auto& item : data) {
            auto& o = item.as_object();
            std::string sym(o.at("symbol").as_string());
            if (sym != symbol_.get()) continue;

            int qty_prec = 6, price_prec = 2;
            if (o.contains("quantityPrecision")) {
                qty_prec = static_cast<int>(parse_json_double(o.at("quantityPrecision")));
            }
            if (o.contains("pricePrecision")) {
                price_prec = static_cast<int>(parse_json_double(o.at("pricePrecision")));
            }

            bitget_submitter_->set_symbol_precision(qty_prec, price_prec);
            logger_->info("pipeline", "Precision загружена с биржи",
                {{"symbol", sym},
                 {"qty_precision", std::to_string(qty_prec)},
                 {"price_precision", std::to_string(price_prec)}});
            return;
        }
    } catch (const std::exception& e) {
        logger_->warn("pipeline", "Ошибка загрузки precision: " + std::string(e.what()));
    }
}

// ==================== Синхронизация баланса ====================

void TradingPipeline::sync_balance_from_exchange() {
    logger_->info("pipeline", "Запрос балансов с биржи...");

    // Запрашиваем ВСЕ ассеты (без фильтра по монете)
    auto resp = rest_client_->get("/api/v2/spot/account/assets");
    if (!resp.success) {
        logger_->warn("pipeline", "Не удалось запросить баланс",
            {{"error", resp.error_message}});
        return;
    }

    try {
        auto doc = boost::json::parse(resp.body);
        auto& obj = doc.as_object();

        if (obj.at("code").as_string() != "00000") {
            logger_->warn("pipeline", "Ошибка API при запросе баланса",
                {{"code", std::string(obj.at("code").as_string())},
                 {"msg", std::string(obj.at("msg").as_string())}});
            return;
        }

        double usdt_available = 0.0;
        double base_available = 0.0;
        std::string base_coin = extract_base_coin(symbol_.get());

        auto& data = obj.at("data").as_array();
        for (const auto& item : data) {
            auto& asset = item.as_object();
            std::string coin(asset.at("coin").as_string());
            double available = std::stod(
                std::string(asset.at("available").as_string()));

            if (coin == "USDT") {
                usdt_available = available;
            } else if (!base_coin.empty() && coin == base_coin) {
                base_available = available;
            }
        }

        logger_->info("pipeline", "Балансы получены с биржи",
            {{"USDT", std::to_string(usdt_available)},
             {base_coin.empty() ? "BASE" : base_coin, std::to_string(base_available)}});

        // Обновить капитал USDT — делим поровну между параллельными pipeline
        double capital_per_pipeline = usdt_available / std::max(1, num_pipelines_);
        portfolio_->set_capital(capital_per_pipeline);

        logger_->info("pipeline", "Капитал для этого pipeline",
            {{"total_usdt", std::to_string(usdt_available)},
             {"per_pipeline", std::to_string(capital_per_pipeline)},
             {"num_pipelines", std::to_string(num_pipelines_)}});

        // Если есть base coin — зарегистрировать как позицию,
        // но ТОЛЬКО если notional стоимость > $0.50 (не пыль).
        // usdtValue из API НЕНАДЁЖЕН (часто возвращает "0"),
        // поэтому используем fallback: запрос тикера для расчёта notional.
        if (base_available > 0.0) {
            double estimated_notional = 0.0;
            // Попробуем получить usdtValue из API
            for (const auto& item : data) {
                auto& asset2 = item.as_object();
                std::string coin2(asset2.at("coin").as_string());
                if (coin2 == base_coin && asset2.contains("usdtValue")) {
                    try {
                        estimated_notional = std::stod(
                            std::string(asset2.at("usdtValue").as_string()));
                    } catch (...) {}
                    break;
                }
            }

            // Fallback: если usdtValue ненадёжен (0), запрашиваем рыночную цену
            if (estimated_notional < 0.01 && base_available > 0.0) {
                auto ticker_resp = rest_client_->get(
                    "/api/v2/spot/market/tickers?symbol=" + symbol_.get());
                if (ticker_resp.success) {
                    try {
                        auto tdoc = boost::json::parse(ticker_resp.body);
                        auto& tobj = tdoc.as_object();
                        if (tobj.at("code").as_string() == "00000") {
                            auto& tdata = tobj.at("data").as_array();
                            if (!tdata.empty()) {
                                double last_price = std::stod(std::string(
                                    tdata[0].as_object().at("lastPr").as_string()));
                                estimated_notional = base_available * last_price;
                                logger_->info("pipeline", "Notional рассчитан по тикеру (usdtValue=0)",
                                    {{"coin", base_coin},
                                     {"size", std::to_string(base_available)},
                                     {"price", std::to_string(last_price)},
                                     {"notional", std::to_string(estimated_notional)}});
                            }
                        }
                    } catch (...) {
                        logger_->warn("pipeline", "Не удалось получить тикер для dust-фильтра",
                            {{"symbol", symbol_.get()}});
                    }
                }
            }

            constexpr double kDustThreshold = 0.50;  // $0.50 — минимальная реальная позиция
            if (estimated_notional >= kDustThreshold) {
                // Для entry price используем текущую рыночную цену (лучше чем 0.0):
                // — позволяет корректно работать trailing stop, time exit, PnL
                // — стоп-лосс и breakeven считают от "entry", при 0.0 всё отключается
                double entry_price_estimate = (estimated_notional > 0.01 && base_available > 0.0)
                    ? estimated_notional / base_available
                    : 0.0;

                portfolio::Position base_pos;
                base_pos.symbol = symbol_;
                base_pos.side = Side::Buy;
                base_pos.size = Quantity(base_available);
                base_pos.avg_entry_price = Price(entry_price_estimate);
                base_pos.opened_at = clock_->now();
                base_pos.strategy_id = StrategyId("sync_from_exchange");
                portfolio_->open_position(base_pos);

                logger_->info("pipeline", "Восстановлена позиция с биржи",
                    {{"coin", base_coin},
                     {"size", std::to_string(base_available)},
                     {"entry_price", std::to_string(entry_price_estimate)},
                     {"notional", std::to_string(estimated_notional)}});
            } else {
                logger_->info("pipeline", "Пылевой баланс проигнорирован",
                    {{"coin", base_coin},
                     {"size", std::to_string(base_available)},
                     {"notional", std::to_string(estimated_notional)}});
            }
        }

    } catch (const std::exception& e) {
        logger_->warn("pipeline", "Ошибка парсинга баланса",
            {{"error", std::string(e.what())}});
    }
}

double TradingPipeline::query_asset_balance(const std::string& coin) {
    // Запрашиваем актуальный баланс ассета с биржи.
    // Критично для SELL — использовать реальный доступный баланс,
    // а не сохранённый при старте (может отличаться из-за комиссий/пыли).
    if (!rest_client_) return 0.0;

    auto resp = rest_client_->get("/api/v2/spot/account/assets");
    if (!resp.success) return 0.0;

    try {
        auto doc = boost::json::parse(resp.body);
        auto& obj = doc.as_object();
        if (obj.at("code").as_string() != "00000") return 0.0;

        auto& data = obj.at("data").as_array();
        for (const auto& item : data) {
            auto& asset = item.as_object();
            std::string asset_coin(asset.at("coin").as_string());
            if (asset_coin == coin) {
                return std::stod(std::string(asset.at("available").as_string()));
            }
        }
    } catch (...) {}

    return 0.0;
}

// ==================== Загрузка исторических свечей ====================

void TradingPipeline::bootstrap_historical_candles() {
    // Профессиональная загрузка истории для краткосрочной торговли:
    // 1. 200 минутных свечей (~3.3 часа) → прогрев всех индикаторов (ADX, EMA50, RSI, MACD)
    // 2. Затем вызываем bootstrap_htf_candles() для 7-дневной истории часовых свечей
    //
    // Bitget API: GET /api/v2/spot/market/candles
    // Формат: [[ts, open, high, low, close, volume, baseVolume], ...]
    // Лимит: 200 свечей за запрос

    logger_->info("pipeline", "=== ЗАГРУЗКА ИСТОРИИ ДЛЯ ПРОГРЕВА ===");

    // Получаем REST-клиент (публичный, без авторизации — работает в любом режиме)
    auto get_client = [&]() -> std::shared_ptr<exchange::bitget::BitgetRestClient> {
        if (rest_client_) return rest_client_;
        return std::make_shared<exchange::bitget::BitgetRestClient>(
            "https://api.bitget.com", "", "", "", logger_, 5000);
    };

    auto client = get_client();
    std::string path = "/api/v2/spot/market/candles";

    // --- Шаг 1: 200 минутных свечей для прогрева индикаторов ---
    logger_->info("pipeline", "Загрузка 200 минутных свечей...");
    std::string query_1m = "symbol=" + symbol_.get() + "&granularity=1min&limit=200";
    auto resp_1m = client->get(path, query_1m);

    if (resp_1m.success) {
        try {
            auto doc = boost::json::parse(resp_1m.body);
            auto& obj = doc.as_object();
            if (obj.at("code").as_string() == "00000") {
                auto& data = obj.at("data").as_array();
                std::vector<normalizer::NormalizedCandle> candles;
                candles.reserve(data.size());

                for (const auto& item : data) {
                    const auto& arr = item.as_array();
                    if (arr.size() < 6) continue;

                    normalizer::NormalizedCandle candle;
                    candle.envelope.symbol = symbol_;
                    candle.envelope.source = "bitget_rest_bootstrap";

                    int64_t ts_ms = 0;
                    if (arr[0].is_string())
                        ts_ms = std::stoll(std::string(arr[0].as_string()));
                    else
                        ts_ms = arr[0].as_int64();
                    candle.envelope.exchange_ts = Timestamp(ts_ms * 1'000'000LL);
                    candle.envelope.received_ts = clock_->now();
                    candle.envelope.processed_ts = clock_->now();

                    candle.interval = "1m";
                    candle.open = Price(parse_json_double(arr[1]));
                    candle.high = Price(parse_json_double(arr[2]));
                    candle.low = Price(parse_json_double(arr[3]));
                    candle.close = Price(parse_json_double(arr[4]));
                    candle.volume = Quantity(parse_json_double(arr[5]));
                    if (arr.size() >= 7) {
                        candle.base_volume = Quantity(parse_json_double(arr[6]));
                    }
                    candle.is_closed = true;
                    candles.push_back(std::move(candle));
                }

                // Разворачиваем: Bitget отдаёт новые→старые
                std::reverse(candles.begin(), candles.end());

                for (const auto& candle : candles) {
                    feature_engine_->on_candle(candle);
                }

                indicators_warmed_up_ = true;
                logger_->info("pipeline", "1m свечи загружены",
                    {{"count", std::to_string(candles.size())},
                     {"is_ready", feature_engine_->is_ready(symbol_) ? "true" : "false"}});
            }
        } catch (const std::exception& e) {
            logger_->warn("pipeline", "Ошибка парсинга 1m свечей: " + std::string(e.what()));
        }
    } else {
        logger_->warn("pipeline", "Не удалось загрузить 1m свечи: " + resp_1m.error_message);
    }

    // --- Шаг 2: Загрузка HTF (часовых) свечей для определения тренда ---
    bootstrap_htf_candles();
    last_htf_update_ns_ = clock_->now().get();
}

// ==================== HTF (High-TimeFrame) анализ ====================

void TradingPipeline::bootstrap_htf_candles() {
    // Загружаем 168 часовых свечей (7 дней) для определения глобального тренда.
    // Это КРИТИЧНО для профессиональной торговли:
    // - EMA20/EMA50 на часовом таймфрейме показывают среднесрочный тренд
    // - RSI на часовом показывает глобальную перекупленность/перепроданность
    // - ADX на часовом определяет, есть ли вообще тренд
    // - MACD histogram на часовом подтверждает/опровергает разворот
    //
    // БЕЗ этих данных бот не может отличить краткосрочный откат от глобального краха.

    logger_->info("pipeline", "Загрузка 168 часовых свечей (7 дней) для HTF-анализа...");

    auto get_client = [&]() -> std::shared_ptr<exchange::bitget::BitgetRestClient> {
        if (rest_client_) return rest_client_;
        return std::make_shared<exchange::bitget::BitgetRestClient>(
            "https://api.bitget.com", "", "", "", logger_, 5000);
    };

    auto client = get_client();
    std::string path = "/api/v2/spot/market/candles";
    std::string query = "symbol=" + symbol_.get() + "&granularity=1h&limit=200";

    auto resp = client->get(path, query);
    if (!resp.success) {
        logger_->warn("pipeline", "Не удалось загрузить HTF свечи: " + resp.error_message);
        return;
    }

    try {
        auto doc = boost::json::parse(resp.body);
        auto& obj = doc.as_object();
        if (obj.at("code").as_string() != "00000") {
            logger_->warn("pipeline", "API ошибка при загрузке HTF свечей");
            return;
        }

        auto& data = obj.at("data").as_array();
        if (data.size() < 50) {
            logger_->warn("pipeline", "Недостаточно HTF свечей: " + std::to_string(data.size()));
            return;
        }

        // Собираем цены закрытия (от старых к новым)
        std::vector<double> closes;
        std::vector<double> highs;
        std::vector<double> lows;
        closes.reserve(data.size());
        highs.reserve(data.size());
        lows.reserve(data.size());

        for (auto it = data.end(); it != data.begin(); ) {
            --it;
            const auto& arr = it->as_array();
            if (arr.size() < 6) continue;
            closes.push_back(parse_json_double(arr[4]));
            highs.push_back(parse_json_double(arr[2]));
            lows.push_back(parse_json_double(arr[3]));
        }

        if (closes.size() >= 50) {
            // Примечание: pipeline_mutex_ НЕ берём здесь, т.к.:
            // - При загрузке (start): однопоточный, конкуренция невозможна
            // - При обновлении (on_feature_snapshot): вызывающий уже держит mutex

            htf_last_close_ = closes.back();

            // Сохраняем буферы для инкрементального обновления
            htf_closes_buffer_ = closes;
            htf_highs_buffer_ = highs;
            htf_lows_buffer_ = lows;

            compute_htf_trend(closes);

            logger_->info("pipeline", "HTF-анализ завершён",
                {{"htf_candles", std::to_string(closes.size())},
                 {"htf_ema20", std::to_string(htf_ema_20_)},
                 {"htf_ema50", std::to_string(htf_ema_50_)},
                 {"htf_rsi", std::to_string(htf_rsi_14_)},
                 {"htf_adx", std::to_string(htf_adx_)},
                 {"htf_macd_hist", std::to_string(htf_macd_histogram_)},
                 {"htf_trend", std::to_string(htf_trend_direction_)},
                 {"htf_strength", std::to_string(htf_trend_strength_)}});
        }

    } catch (const std::exception& e) {
        logger_->warn("pipeline", "Ошибка парсинга HTF свечей: " + std::string(e.what()));
    }
}

// ==================== HTF Real-Time Update ====================

void TradingPipeline::maybe_update_htf(const features::FeatureSnapshot& snapshot) {
    int64_t now_ns = clock_->now().get();

    // Проверка экстренного обновления: цена ушла > 3×ATR от последнего HTF close
    if (htf_valid_ && snapshot.technical.atr_valid && htf_last_close_ > 0.0) {
        double price_move = std::abs(snapshot.mid_price.get() - htf_last_close_);
        if (price_move > 3.0 * snapshot.technical.atr_14) {
            htf_urgent_update_needed_ = true;
        }
    }

    // Регулярное обновление каждый час ИЛИ экстренное
    bool time_to_update = (last_htf_update_ns_ == 0) ||
        ((now_ns - last_htf_update_ns_) >= kHtfUpdateIntervalNs);

    if (!time_to_update && !htf_urgent_update_needed_) return;

    logger_->info("pipeline", "Обновление HTF данных",
        {{"reason", htf_urgent_update_needed_ ? "URGENT: цена сдвинулась > 3×ATR" : "periodic_hourly"},
         {"symbol", symbol_.get()}});

    try {
        // Загружаем свежие свечи через REST (тот же метод что при bootstrap)
        bootstrap_htf_candles();
        last_htf_update_ns_ = now_ns;

        // КРИТИЧНО: обновляем htf_last_close_ текущей ценой, чтобы
        // urgent-проверка (price_move > 3×ATR) не срабатывала повторно.
        // bootstrap_htf_candles() ставит htf_last_close_ = последняя часовая свеча,
        // которая может отличаться от real-time цены на > 3×ATR(1m).
        htf_last_close_ = snapshot.mid_price.get();
        htf_urgent_update_needed_ = false;

        logger_->info("pipeline", "HTF данные обновлены",
            {{"htf_trend", std::to_string(htf_trend_direction_)},
             {"htf_strength", std::to_string(htf_trend_strength_)},
             {"htf_ema20", std::to_string(htf_ema_20_)},
             {"htf_ema50", std::to_string(htf_ema_50_)},
             {"htf_rsi", std::to_string(htf_rsi_14_)}});
    } catch (const std::exception& e) {
        // Устанавливаем cooldown даже при ошибке, чтобы не спамить API
        last_htf_update_ns_ = now_ns - kHtfUpdateIntervalNs + 300'000'000'000LL; // Retry через 5 мин
        htf_urgent_update_needed_ = false;
        logger_->warn("pipeline", "Ошибка обновления HTF данных (retry через 5 мин)",
            {{"error", e.what()}});
    }
}

void TradingPipeline::compute_htf_trend(const std::vector<double>& closes) {
    // Вычисляем индикаторы на часовом таймфрейме.
    // Это "старший фильтр" — определяет общее направление рынка.

    const size_t n = closes.size();
    if (n < 51) return;

    // --- EMA20 и EMA50 ---
    {
        double k20 = 2.0 / (20.0 + 1.0);
        double k50 = 2.0 / (50.0 + 1.0);

        // Seed: SMA за первые N баров
        double sma20 = 0.0, sma50 = 0.0;
        for (size_t i = 0; i < 20; ++i) sma20 += closes[i];
        sma20 /= 20.0;
        for (size_t i = 0; i < 50; ++i) sma50 += closes[i];
        sma50 /= 50.0;

        double ema20 = sma20, ema50 = sma50;
        for (size_t i = 20; i < n; ++i)
            ema20 = closes[i] * k20 + ema20 * (1.0 - k20);
        for (size_t i = 50; i < n; ++i)
            ema50 = closes[i] * k50 + ema50 * (1.0 - k50);

        htf_ema_20_ = ema20;
        htf_ema_50_ = ema50;
    }

    // --- RSI (14 периодов, Wilder's smoothing) ---
    {
        const size_t period = 14;
        double avg_gain = 0.0, avg_loss = 0.0;

        for (size_t i = 1; i <= period && i < n; ++i) {
            double diff = closes[i] - closes[i - 1];
            if (diff > 0) avg_gain += diff;
            else avg_loss -= diff;
        }
        avg_gain /= period;
        avg_loss /= period;

        for (size_t i = period + 1; i < n; ++i) {
            double diff = closes[i] - closes[i - 1];
            double gain = (diff > 0) ? diff : 0.0;
            double loss = (diff < 0) ? -diff : 0.0;
            avg_gain = (avg_gain * (period - 1) + gain) / period;
            avg_loss = (avg_loss * (period - 1) + loss) / period;
        }

        if (avg_loss > 0.0) {
            double rs = avg_gain / avg_loss;
            htf_rsi_14_ = 100.0 - 100.0 / (1.0 + rs);
        } else {
            htf_rsi_14_ = 100.0;
        }
    }

    // --- MACD (12/26/9) ---
    {
        double k12 = 2.0 / 13.0, k26 = 2.0 / 27.0, k9 = 2.0 / 10.0;
        double ema12 = closes[0], ema26 = closes[0];
        for (size_t i = 1; i < n; ++i) {
            ema12 = closes[i] * k12 + ema12 * (1.0 - k12);
            ema26 = closes[i] * k26 + ema26 * (1.0 - k26);
        }
        double macd_line = ema12 - ema26;

        // Signal line (EMA9 of MACD): пересчитываем из полной серии
        std::vector<double> macd_series;
        double e12 = closes[0], e26 = closes[0];
        for (size_t i = 1; i < n; ++i) {
            e12 = closes[i] * k12 + e12 * (1.0 - k12);
            e26 = closes[i] * k26 + e26 * (1.0 - k26);
            macd_series.push_back(e12 - e26);
        }

        if (macd_series.size() >= 9) {
            double signal = macd_series[0];
            for (size_t i = 1; i < macd_series.size(); ++i) {
                signal = macd_series[i] * k9 + signal * (1.0 - k9);
            }
            htf_macd_histogram_ = macd_line - signal;
        }
    }

    // --- ADX (14 периодов) с True Range из highs/lows ---
    // Используем реальные highs/lows если доступны, иначе fallback на closes.
    {
        const size_t period = 14;
        bool have_hlc = (htf_highs_buffer_.size() == n && htf_lows_buffer_.size() == n);
        std::vector<double> dx_vals;
        double sum_plus_dm = 0.0, sum_minus_dm = 0.0, sum_tr = 0.0;

        for (size_t i = 1; i < n; ++i) {
            double plus_dm = 0.0, minus_dm = 0.0, tr = 0.0;

            if (have_hlc) {
                // Proper Directional Movement с highs/lows
                double up_move = htf_highs_buffer_[i] - htf_highs_buffer_[i - 1];
                double down_move = htf_lows_buffer_[i - 1] - htf_lows_buffer_[i];
                plus_dm = (up_move > down_move && up_move > 0.0) ? up_move : 0.0;
                minus_dm = (down_move > up_move && down_move > 0.0) ? down_move : 0.0;
                // True Range
                double hl = htf_highs_buffer_[i] - htf_lows_buffer_[i];
                double hc = std::abs(htf_highs_buffer_[i] - closes[i - 1]);
                double lc = std::abs(htf_lows_buffer_[i] - closes[i - 1]);
                tr = std::max({hl, hc, lc});
            } else {
                // Fallback: closes-only прокси
                double diff = closes[i] - closes[i - 1];
                plus_dm = std::max(0.0, diff);
                minus_dm = std::max(0.0, -diff);
                tr = std::abs(diff);
            }

            if (i <= period) {
                sum_plus_dm += plus_dm;
                sum_minus_dm += minus_dm;
                sum_tr += tr;
            } else {
                sum_plus_dm = sum_plus_dm - sum_plus_dm / period + plus_dm;
                sum_minus_dm = sum_minus_dm - sum_minus_dm / period + minus_dm;
                sum_tr = sum_tr - sum_tr / period + tr;
            }

            if (i >= period && sum_tr > 0.0) {
                double plus_di = sum_plus_dm / sum_tr * 100.0;
                double minus_di = sum_minus_dm / sum_tr * 100.0;
                double di_sum = plus_di + minus_di;
                if (di_sum > 0.0) {
                    dx_vals.push_back(std::abs(plus_di - minus_di) / di_sum * 100.0);
                }
            }
        }

        if (dx_vals.size() >= period) {
            double adx = 0.0;
            for (size_t i = 0; i < period; ++i) adx += dx_vals[i];
            adx /= period;
            for (size_t i = period; i < dx_vals.size(); ++i) {
                adx = (adx * (period - 1) + dx_vals[i]) / period;
            }
            htf_adx_ = adx;
        }
    }

    // --- Определение направления и силы тренда ---
    // Правила профессионального трейдинга:
    // - EMA20 > EMA50 + ADX > 20 → аптренд
    // - EMA20 < EMA50 + ADX > 20 → даунтренд
    // - ADX < 20 → боковик (нет тренда)
    // - MACD histogram подтверждает направление

    bool ema_bullish = htf_ema_20_ > htf_ema_50_;
    bool ema_bearish = htf_ema_20_ < htf_ema_50_;
    bool strong_trend = htf_adx_ > 25.0;
    bool has_trend = htf_adx_ > 20.0;

    if (ema_bullish && has_trend) {
        htf_trend_direction_ = 1;
    } else if (ema_bearish && has_trend) {
        htf_trend_direction_ = -1;
    } else {
        htf_trend_direction_ = 0;
    }

    // Сила тренда: комбинация ADX + расстояния EMA + RSI
    double ema_gap_pct = (htf_ema_50_ > 0.0)
        ? std::abs(htf_ema_20_ - htf_ema_50_) / htf_ema_50_ * 100.0
        : 0.0;
    double adx_factor = std::clamp((htf_adx_ - 15.0) / 35.0, 0.0, 1.0);
    double ema_factor = std::clamp(ema_gap_pct / 3.0, 0.0, 1.0);

    // RSI: чем дальше от 50, тем сильнее тренд
    double rsi_factor = std::clamp(std::abs(htf_rsi_14_ - 50.0) / 30.0, 0.0, 1.0);

    htf_trend_strength_ = (adx_factor * 0.5 + ema_factor * 0.3 + rsi_factor * 0.2);
    htf_trend_strength_ = std::clamp(htf_trend_strength_, 0.0, 1.0);

    htf_valid_ = true;

    logger_->info("pipeline", "HTF тренд определён",
        {{"direction", htf_trend_direction_ == 1 ? "UP" :
                       htf_trend_direction_ == -1 ? "DOWN" : "SIDEWAYS"},
         {"strength", std::to_string(htf_trend_strength_)},
         {"strong_trend", strong_trend ? "true" : "false"},
         {"ema_gap_pct", std::to_string(ema_gap_pct)}});
}

// ==================== Market Readiness Gate ====================

bool TradingPipeline::check_market_readiness(const features::FeatureSnapshot& snapshot) {
    // Профессиональная система не торгует "вслепую" при запуске.
    // Проверяет несколько условий перед первой сделкой:
    //
    // 1. HTF-данные загружены и валидны
    // 2. Индикаторы на рабочем таймфрейме стабилизированы
    // 3. Нет экстремальных условий (RSI < 15 или > 85 на HTF)
    // 4. Если HTF в сильном даунтренде — не покупаем (waiting for reversal)
    //
    // После прохождения всех проверок market_ready_ = true на весь цикл,
    // пока не сбросится (ротация пар, перезапуск).

    if (market_ready_) return true;

    // 1. HTF должен быть загружен
    if (!htf_valid_) {
        if (tick_count_ % 500 == 0) {
            logger_->info("pipeline", "Ожидание HTF-данных перед началом торговли...");
        }
        return false;
    }

    // 2. Индикаторы рабочего таймфрейма должны быть готовы
    if (!snapshot.technical.sma_valid || !snapshot.technical.rsi_valid
        || !snapshot.technical.adx_valid || !snapshot.technical.macd_valid) {
        if (tick_count_ % 500 == 0) {
            logger_->info("pipeline", "Ожидание прогрева индикаторов...",
                {{"sma", snapshot.technical.sma_valid ? "ok" : "wait"},
                 {"rsi", snapshot.technical.rsi_valid ? "ok" : "wait"},
                 {"adx", snapshot.technical.adx_valid ? "ok" : "wait"},
                 {"macd", snapshot.technical.macd_valid ? "ok" : "wait"}});
        }
        return false;
    }

    // 3. Не входить в рынок при экстремальных HTF условиях
    if (htf_rsi_14_ < 15.0 || htf_rsi_14_ > 85.0) {
        if (tick_count_ % 1000 == 0) {
            logger_->warn("pipeline",
                "HTF RSI в экстремальной зоне — ожидание нормализации",
                {{"htf_rsi", std::to_string(htf_rsi_14_)}});
        }
        return false;
    }

    // 4. Не входить при сильном даунтренде на HTF (сила > 0.6)
    // Это защита от "ловли падающего ножа"
    if (htf_trend_direction_ == -1 && htf_trend_strength_ > 0.6) {
        // Разрешаем вход если MACD histogram начал расти (потенциальный разворот)
        bool macd_reversal = htf_macd_histogram_ > 0.0;
        // Или RSI вышел из перепроданности и растёт (> 35)
        bool rsi_recovery = htf_rsi_14_ > 35.0;

        if (!macd_reversal && !rsi_recovery) {
            if (tick_count_ % 1000 == 0) {
                logger_->warn("pipeline",
                    "Сильный даунтренд на HTF — ожидание сигнала разворота",
                    {{"htf_trend", "DOWN"},
                     {"htf_strength", std::to_string(htf_trend_strength_)},
                     {"htf_macd_hist", std::to_string(htf_macd_histogram_)},
                     {"htf_rsi", std::to_string(htf_rsi_14_)}});
            }
            return false;
        }
    }

    // Все проверки пройдены
    market_ready_ = true;
    market_ready_since_tick_ = tick_count_;
    logger_->info("pipeline", "=== РЫНОК ГОТОВ К ТОРГОВЛЕ ===",
        {{"tick", std::to_string(tick_count_)},
         {"htf_trend", htf_trend_direction_ == 1 ? "UP" :
                       htf_trend_direction_ == -1 ? "DOWN" : "SIDEWAYS"},
         {"htf_strength", std::to_string(htf_trend_strength_)},
         {"htf_rsi", std::to_string(htf_rsi_14_)}});
    return true;
}

// ==================== Адаптивный стоп-лосс (Chandelier Exit) ====================

void TradingPipeline::reset_trailing_state() {
    highest_price_since_entry_ = 0.0;
    lowest_price_since_entry_ = 1e18;
    current_stop_level_ = 0.0;
    breakeven_activated_ = false;
    partial_tp_taken_ = false;
    initial_position_size_ = 0.0;
    current_trail_mult_ = 2.0;
    position_entry_time_ns_ = 0;
}

void TradingPipeline::update_trailing_stop(const features::FeatureSnapshot& snapshot) {
    auto port_snap = portfolio_->snapshot();
    if (port_snap.positions.empty()) {
        reset_trailing_state();
        return;
    }

    for (const auto& pos : port_snap.positions) {
        if (pos.symbol.get() != symbol_.get()) continue;
        double price = pos.current_price.get();
        double entry = pos.avg_entry_price.get();
        if (entry <= 0.0 || price <= 0.0) continue;

        // Запоминаем начальный размер и время входа при первом обновлении
        if (initial_position_size_ <= 0.0) {
            initial_position_size_ = pos.size.get();
            position_entry_time_ns_ = clock_->now().get();
        }

        // ATR должен быть валидным
        if (!snapshot.technical.atr_valid || snapshot.technical.atr_14 <= 0.0) continue;
        double atr = snapshot.technical.atr_14;

        // Адаптивный ATR-множитель по режиму рынка (ADX)
        // Для 1-мин скальпинга используем плотные стопы (1.0–2.0×ATR)
        if (snapshot.technical.adx_valid) {
            if (snapshot.technical.adx > 30.0) {
                current_trail_mult_ = 2.0;   // Сильный тренд → средний trail
            } else if (snapshot.technical.adx > 20.0) {
                current_trail_mult_ = 1.5;   // Умеренный тренд → плотный trail
            } else {
                current_trail_mult_ = 1.0;   // Боковик → очень узкий trail
            }
        }

        if (pos.side == Side::Buy) {
            // Обновляем максимальную цену
            highest_price_since_entry_ = std::max(highest_price_since_entry_, price);

            // Chandelier Exit: стоп = максимум - N×ATR
            double new_stop = highest_price_since_entry_ - current_trail_mult_ * atr;

            // Breakeven: если профит >= 1.5×ATR, подтягиваем стоп к входу + 0.1×ATR
            double profit_in_atr = (price - entry) / atr;
            if (!breakeven_activated_ && profit_in_atr >= 1.5) {
                double breakeven_level = entry + 0.1 * atr;
                new_stop = std::max(new_stop, breakeven_level);
                breakeven_activated_ = true;
                logger_->info("pipeline", "Breakeven Stop активирован",
                    {{"entry", std::to_string(entry)},
                     {"new_stop", std::to_string(breakeven_level)},
                     {"profit_atr", std::to_string(profit_in_atr)}});
            }

            // Стоп только поднимается, никогда не опускается
            current_stop_level_ = std::max(current_stop_level_, new_stop);
        } else {
            // SELL позиция (шорт-эквивалент)
            lowest_price_since_entry_ = std::min(lowest_price_since_entry_, price);
            double new_stop = lowest_price_since_entry_ + current_trail_mult_ * atr;

            double profit_in_atr = (entry - price) / atr;
            if (!breakeven_activated_ && profit_in_atr >= 1.5) {
                double breakeven_level = entry - 0.1 * atr;
                new_stop = std::min(new_stop, breakeven_level);
                breakeven_activated_ = true;
                logger_->info("pipeline", "Breakeven Stop активирован (SELL)",
                    {{"entry", std::to_string(entry)},
                     {"new_stop", std::to_string(breakeven_level)}});
            }

            // Стоп только опускается для шорта
            // Для SELL: стоп находится выше цены, инициализируем корректно
            if (current_stop_level_ <= 0.0 || current_stop_level_ > new_stop * 2.0) {
                current_stop_level_ = new_stop;
            } else {
                current_stop_level_ = std::min(current_stop_level_, new_stop);
            }
        }
    }
}

// ==================== Стоп-лосс позиций ====================

bool TradingPipeline::check_position_stop_loss(const features::FeatureSnapshot& snapshot) {
    // Система управления стопами (Chandelier Exit + Breakeven + Partial TP):
    //
    // 1. Trailing Stop (Chandelier Exit): стоп подтягивается за ценой, никогда не откатывается.
    //    Адаптивный множитель ATR зависит от режима рынка (ADX).
    // 2. Partial Take-Profit: при профите >= 2×ATR закрываем 50% позиции.
    // 3. Фиксированный стоп (safety net): убыток > kMaxLossPerTradePct% капитала → экстренное закрытие.
    //
    // Trailing stop — основной механизм выхода, фиксированный — страховка на случай сбоя.

    // Обновляем trailing stop каждый тик
    update_trailing_stop(snapshot);

    auto port_snap = portfolio_->snapshot();
    if (port_snap.positions.empty()) return false;

    for (const auto& pos : port_snap.positions) {
        if (pos.symbol.get() != symbol_.get()) continue;
        if (pos.avg_entry_price.get() <= 0.0) continue;  // Нет цены входа (sync)
        if (pos.current_price.get() <= 0.0) continue;

        double price = pos.current_price.get();
        double entry = pos.avg_entry_price.get();

        // === 1. Trailing Stop (Chandelier Exit) ===
        bool trailing_stop_triggered = false;
        if (current_stop_level_ > 0.0) {
            if (pos.side == Side::Buy) {
                trailing_stop_triggered = price <= current_stop_level_;
            } else {
                trailing_stop_triggered = price >= current_stop_level_;
            }
        }

        // === 2. Partial Take-Profit: профит >= 2×ATR → закрыть 50% ===
        bool partial_tp_triggered = false;
        if (!partial_tp_taken_ && snapshot.technical.atr_valid && snapshot.technical.atr_14 > 0.0) {
            double atr = snapshot.technical.atr_14;
            double profit_in_atr = (pos.side == Side::Buy)
                ? (price - entry) / atr
                : (entry - price) / atr;
            if (profit_in_atr >= 2.0) {
                partial_tp_triggered = true;
            }
        }

        // === 3. Фиксированный стоп (safety net): убыток > X% капитала ===
        double loss_pct_of_capital = 0.0;
        if (port_snap.total_capital > 0.0) {
            loss_pct_of_capital = std::abs(std::min(pos.unrealized_pnl, 0.0))
                                / port_snap.total_capital * 100.0;
        }
        bool fixed_stop_triggered = loss_pct_of_capital >= kMaxLossPerTradePct;

        // === 4. Time-based exit: закрытие по таймауту ===
        bool time_exit_triggered = false;
        if (position_entry_time_ns_ > 0) {
            int64_t now_check = clock_->now().get();
            int64_t hold_duration = now_check - position_entry_time_ns_;
            // Убыточная позиция > 15 мин → закрыть
            if (pos.unrealized_pnl < 0.0 && hold_duration >= kMaxHoldLossNs) {
                time_exit_triggered = true;
            }
            // Любая позиция > 60 мин → закрыть
            if (hold_duration >= kMaxHoldAbsoluteNs) {
                time_exit_triggered = true;
            }
        }

        if (!trailing_stop_triggered && !partial_tp_triggered && !fixed_stop_triggered && !time_exit_triggered) continue;

        // === Стоп/TP сработал: проверяем возможность закрытия ===

        // Cooldown стоп-лосса (отдельный от обычных ордеров — стоп экстренный)
        int64_t now_ns = clock_->now().get();
        if (last_stop_loss_time_ns_ > 0 &&
            (now_ns - last_stop_loss_time_ns_) < kStopLossCooldownNs) {
            // Уже пытались недавно — не спамим биржу, но сигнализируем pipeline
            // что стоп-лосс активен (блокировать стратегии)
            return true;
        }

        // Для SELL всегда используем РЕАЛЬНЫЙ баланс с биржи,
        // а не портфель — портфель может расходиться из-за комиссий/частичных ордеров
        double actual_qty = query_asset_balance(extract_base_coin(symbol_.get()));
        if (actual_qty <= 0.0) {
            // Фолбэк на портфель если API недоступен
            actual_qty = pos.size.get();
        }
        if (actual_qty < 0.00001) {
            // Токен уже продан (стоп-лосс сработал ранее), но позиция
            // осталась в портфеле. Очищаем её принудительно.
            logger_->info("pipeline", "Стоп-лосс: актив уже продан, очищаем позицию из портфеля",
                {{"qty", std::to_string(actual_qty)},
                 {"symbol", symbol_.get()}});
            record_trade_for_decay(current_position_strategy_, pos.unrealized_pnl, current_position_conviction_);
            portfolio_->close_position(symbol_, pos.current_price, pos.unrealized_pnl);
            reset_trailing_state();
            return true;
        }
        // Пылевая позиция (< $0.50) — невозможно продать, пропускаем
        double actual_notional = actual_qty * price;
        if (actual_notional < 0.50) {
            logger_->debug("pipeline", "Пылевая позиция, пропускаем стоп-лосс",
                {{"notional", std::to_string(actual_notional)},
                 {"symbol", symbol_.get()}});
            // Очищаем позицию из портфеля чтобы не блокировать новые сделки
            portfolio_->close_position(symbol_, pos.current_price, pos.unrealized_pnl);
            reset_trailing_state();
            return false;
        }

        // Определяем количество и причину закрытия
        double close_qty = actual_qty;
        bool is_full_close = true;
        std::string reason;

        if (partial_tp_triggered && !trailing_stop_triggered && !fixed_stop_triggered) {
            // Partial TP: закрываем 50% позиции
            close_qty = actual_qty * 0.5;
            // Минимальный ордер Bitget
            if (close_qty < 0.00001) close_qty = actual_qty;
            is_full_close = false;
            partial_tp_taken_ = true;
            reason = "PARTIAL_TP: профит >= 2×ATR, закрытие 50%";

            logger_->info("pipeline", "PARTIAL TAKE-PROFIT — закрытие 50% позиции",
                {{"symbol", symbol_.get()},
                 {"entry", std::to_string(entry)},
                 {"current", std::to_string(price)},
                 {"close_qty", std::to_string(close_qty)},
                 {"remaining", std::to_string(actual_qty - close_qty)}});
        } else if (trailing_stop_triggered) {
            reason = "TRAILING_STOP: цена " + std::to_string(price)
                   + " прошла стоп " + std::to_string(current_stop_level_);
        } else if (time_exit_triggered) {
            int64_t hold_dur = clock_->now().get() - position_entry_time_ns_;
            reason = "TIME_EXIT: hold=" + std::to_string(hold_dur / 60'000'000'000LL)
                   + "min, PnL=" + std::to_string(pos.unrealized_pnl);
        } else {
            reason = "FIXED_STOP: убыток " + std::to_string(loss_pct_of_capital) + "% капитала";
        }

        if (is_full_close) {
            logger_->warn("pipeline", "СТОП-ЛОСС СРАБОТАЛ — принудительное закрытие",
                {{"symbol", symbol_.get()},
                 {"reason", reason},
                 {"entry", std::to_string(entry)},
                 {"current", std::to_string(price)},
                 {"trail_stop", std::to_string(current_stop_level_)},
                 {"trail_mult", std::to_string(current_trail_mult_)},
                 {"unrealized_pnl", std::to_string(pos.unrealized_pnl)},
                 {"loss_pct", std::to_string(loss_pct_of_capital)}});
        }

        // Формируем SELL intent для закрытия
        strategy::TradeIntent close_intent;
        close_intent.symbol = symbol_;
        close_intent.side = Side::Sell;
        close_intent.conviction = 1.0;  // Максимальная уверенность — это стоп/TP
        close_intent.urgency = 1.0;     // Максимальная срочность → market order
        close_intent.strategy_id = StrategyId(
            partial_tp_triggered && !trailing_stop_triggered ? "partial_tp"
            : time_exit_triggered ? "time_exit"
            : "stop_loss");
        close_intent.limit_price = snapshot.mid_price;
        close_intent.suggested_quantity = Quantity(close_qty);
        // Уникальный correlation_id для предотвращения дублирования
        close_intent.correlation_id = CorrelationId(
            "SL-" + std::to_string(now_ns));

        risk::RiskDecision risk_decision;
        risk_decision.decided_at = clock_->now();
        risk_decision.approved_quantity = Quantity(close_qty);
        risk_decision.verdict = risk::RiskVerdict::Approved;
        risk_decision.summary = "STOP-LOSS: " + reason;

        // Stop-loss bypass: не используем execution_alpha для экстренных ордеров.
        // ExecutionAlpha может блокировать ордер ("NoExecution: условия неблагоприятны"),
        // но стоп-лосс ОБЯЗАН исполниться для ограничения убытков.
        execution_alpha::ExecutionAlphaResult exec_alpha;
        exec_alpha.should_execute = true;
        exec_alpha.recommended_style = execution_alpha::ExecutionStyle::Aggressive;
        exec_alpha.urgency_score = 1.0;
        exec_alpha.rationale = "STOP-LOSS: bypass execution alpha";

        // Обновляем cooldown стоп-лосса ПЕРЕД отправкой (даже при ошибке — не спамить)
        last_stop_loss_time_ns_ = now_ns;
        // Также обновляем общий cooldown — стоп-лосс это тоже ордер
        last_order_time_ns_ = now_ns;
        last_activity_ns_.store(now_ns, std::memory_order_relaxed);

        auto order_result = execution_engine_->execute(close_intent, risk_decision, exec_alpha);
        if (order_result) {
            risk_engine_->record_order_sent();
            logger_->warn("pipeline", is_full_close
                    ? "СТОП-ЛОСС ОРДЕР ОТПРАВЛЕН"
                    : "PARTIAL TP ОРДЕР ОТПРАВЛЕН",
                {{"order_id", order_result->get()},
                 {"qty", std::to_string(close_qty)},
                 {"symbol", symbol_.get()},
                 {"reason", reason}});

            if (is_full_close) {
                // Execution engine уже вызовет close_position при fill.
                // Записываем данные для decay/fingerprint/thompson, но НЕ вызываем
                // portfolio_->close_position() повторно — иначе PnL будет двойным.
                record_trade_for_decay(current_position_strategy_, pos.unrealized_pnl, current_position_conviction_);

                // Записываем результат fingerprint (стоп-лосс = негативный исход)
                if (fingerprinter_ && last_entry_fingerprint_) {
                    double norm_pnl = (pos.unrealized_pnl > 0) ? 1.0 : -1.0;
                    fingerprinter_->record_outcome(*last_entry_fingerprint_, norm_pnl);
                    last_entry_fingerprint_.reset();
                }

                // Thompson Sampling: стоп-лосс — используем действие входа
                if (thompson_sampler_) {
                    double ts_reward = (pos.unrealized_pnl > 0) ? 1.0 : -1.0;
                    thompson_sampler_->record_reward(current_entry_thompson_action_, ts_reward);
                }

                // Записываем результат в risk engine
                risk_engine_->record_trade_result(pos.unrealized_pnl < 0.0);

                reset_trailing_state();
            }
        } else {
            logger_->error("pipeline", "Стоп-лосс ордер не исполнен — позиция остаётся открытой",
                {{"symbol", symbol_.get()}});
        }

        return true;  // Стоп-лосс/TP активен — блокируем дальнейшую торговлю на этом тике
    }

    return false;
}

bool TradingPipeline::start() {
    logger_->info("pipeline", "Запуск торгового pipeline...");
    logger_->info("pipeline", "Символ: " + symbol_.get());
    logger_->info("pipeline", "Режим: " + std::string(to_string(config_.trading.mode)));
    logger_->info("pipeline", "Стратегий активно: " +
        std::to_string(strategy_registry_->active().size()));

    // Синхронизация баланса с биржей (только production)
    if (rest_client_) {
        sync_balance_from_exchange();
        fetch_symbol_precision();
    }

    // Загрузка исторических свечей для прогрева индикаторов.
    // КРИТИЧНО: без этого SMA/RSI/ADX/EMA не имеют данных,
    // и microstructure_scalp торгует вслепую.
    bootstrap_historical_candles();

    gateway_->start();
    running_ = true;
    last_activity_ns_.store(clock_->now().get(), std::memory_order_relaxed);

    logger_->info("pipeline", "Торговый pipeline запущен. Подключение к бирже...");
    return true;
}

void TradingPipeline::stop() {
    // Защита от повторного вызова
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false)) {
        return;
    }
    if (gateway_) gateway_->stop();
    logger_->info("pipeline", "Торговый pipeline остановлен");
}

bool TradingPipeline::is_connected() const {
    return gateway_ && gateway_->is_connected();
}

bool TradingPipeline::has_open_position() const {
    if (!portfolio_) return false;
    auto pos = portfolio_->get_position(symbol_);
    if (!pos.has_value()) return false;
    return pos->size.get() > 0.0;
}

bool TradingPipeline::is_idle(int64_t threshold_ns) const {
    int64_t last = last_activity_ns_.load(std::memory_order_relaxed);
    if (last == 0) return false; // ещё не было ни одного тика
    int64_t now = clock_->now().get();
    return (now - last) > threshold_ns;
}

// ==================== Alpha Decay Feedback ====================

void TradingPipeline::check_alpha_decay_feedback() {
    // Не проверять чаще чем раз в 60 секунд
    int64_t now_ns = clock_->now().get();
    if (last_alpha_decay_check_ns_ > 0 &&
        (now_ns - last_alpha_decay_check_ns_) < kAlphaDecayCheckIntervalNs) {
        return;
    }
    last_alpha_decay_check_ns_ = now_ns;

    auto reports = alpha_decay_monitor_->get_all_reports();

    // Сбрасываем множители перед пересчётом
    alpha_decay_size_mult_ = 1.0;
    alpha_decay_threshold_adj_ = 0.0;

    for (const auto& report : reports) {
        auto rec = report.overall_recommendation;
        double health = report.overall_health;

        switch (rec) {
            case alpha_decay::DecayRecommendation::NoAction:
                break;

            case alpha_decay::DecayRecommendation::ReduceWeight:
                // Небольшое снижение размера
                alpha_decay_size_mult_ = std::min(alpha_decay_size_mult_, 0.7);
                logger_->info("AlphaDecay", "Рекомендация: уменьшить вес стратегии",
                    {{"strategy", report.strategy_id.get()},
                     {"health", std::to_string(health)}});
                break;

            case alpha_decay::DecayRecommendation::ReduceSize:
                // Существенное снижение — z-score > 2σ
                alpha_decay_size_mult_ = std::min(alpha_decay_size_mult_, 0.5);
                alpha_decay_threshold_adj_ = std::max(alpha_decay_threshold_adj_, 0.05);
                logger_->warn("AlphaDecay", "Рекомендация: уменьшить размер позиций",
                    {{"strategy", report.strategy_id.get()},
                     {"health", std::to_string(health)}});
                break;

            case alpha_decay::DecayRecommendation::RaiseThresholds:
                // Поднять порог conviction
                alpha_decay_threshold_adj_ = std::max(alpha_decay_threshold_adj_, 0.10);
                alpha_decay_size_mult_ = std::min(alpha_decay_size_mult_, 0.5);
                logger_->warn("AlphaDecay", "Рекомендация: поднять пороги входа",
                    {{"strategy", report.strategy_id.get()},
                     {"health", std::to_string(health)}});
                break;

            case alpha_decay::DecayRecommendation::MoveToShadow:
            case alpha_decay::DecayRecommendation::Disable:
                // Критическая деградация — блокировать торговлю
                alpha_decay_size_mult_ = 0.0;
                alpha_decay_threshold_adj_ = 1.0;  // Недостижимый порог
                logger_->error("AlphaDecay", "КРИТИЧЕСКАЯ ДЕГРАДАЦИЯ — торговля заблокирована",
                    {{"strategy", report.strategy_id.get()},
                     {"health", std::to_string(health)},
                     {"recommendation", "DISABLE"}});
                break;

            case alpha_decay::DecayRecommendation::AlertOperator:
                logger_->error("AlphaDecay", "ТРЕБУЕТСЯ ВНИМАНИЕ ОПЕРАТОРА",
                    {{"strategy", report.strategy_id.get()},
                     {"health", std::to_string(health)}});
                break;
        }
    }
}

void TradingPipeline::record_trade_for_decay(
    const StrategyId& strategy_id, double pnl, double conviction)
{
    if (!alpha_decay_monitor_) return;

    alpha_decay::TradeOutcome outcome;
    // Конвертируем абсолютную P&L ($) в basis points относительно капитала.
    // pnl_bps = (pnl / capital) * 10000
    double capital = portfolio_->snapshot().total_capital;
    outcome.pnl_bps = (capital > 0.0) ? (pnl / capital * 10000.0) : 0.0;
    outcome.slippage_bps = 0.0;
    outcome.conviction = conviction;
    outcome.timestamp = clock_->now();

    // Режим рынка на момент закрытия (Unclear если нет данных)
    outcome.regime = RegimeLabel::Unclear;

    alpha_decay_monitor_->record_trade_outcome(strategy_id, outcome);

    logger_->debug("AlphaDecay", "Результат сделки записан",
        {{"strategy", strategy_id.get()},
         {"pnl_bps", std::to_string(outcome.pnl_bps)},
         {"conviction", std::to_string(conviction)}});
}

// ==================== Основной торговый цикл ====================

void TradingPipeline::on_feature_snapshot(features::FeatureSnapshot snapshot) {
    if (!running_) return;
    std::lock_guard<std::mutex> lock(pipeline_mutex_);

    ++tick_count_;

    // Логируем первый тик — означает завершение прогрева
    if (tick_count_ == 1) {
        logger_->info("pipeline", "Первый тик от WebSocket",
            {{"sma20", std::to_string(snapshot.technical.sma_20)},
             {"rsi14", std::to_string(snapshot.technical.rsi_14)},
             {"sma_valid", snapshot.technical.sma_valid ? "true" : "false"},
             {"rsi_valid", snapshot.technical.rsi_valid ? "true" : "false"},
             {"atr_valid", snapshot.technical.atr_valid ? "true" : "false"},
             {"adx_valid", snapshot.technical.adx_valid ? "true" : "false"},
             {"indicators_warmed", indicators_warmed_up_ ? "true" : "false"}});
    }

    // 0. Обновить текущую цену в портфеле (для P&L и стоп-лоссов)
    if (snapshot.mid_price.get() > 0.0) {
        portfolio_->update_price(symbol_, snapshot.mid_price);

        // Обновляем продвинутые features по текущему тику
        if (advanced_features_) {
            advanced_features_->on_tick(snapshot.mid_price.get());
        }
    }

    // 0.adv. Заполняем продвинутые features (CUSUM, VPIN, Volume Profile, Time-of-Day)
    if (advanced_features_) {
        advanced_features_->fill_snapshot(snapshot);
    }

    // 0.ml1. Фильтр энтропии: обновляем данные каждый тик
    if (entropy_filter_ && snapshot.mid_price.get() > 0.0) {
        entropy_filter_->on_tick(
            snapshot.mid_price.get(),
            snapshot.microstructure.bid_depth_5_notional + snapshot.microstructure.ask_depth_5_notional,
            snapshot.microstructure.spread_bps,
            snapshot.microstructure.aggressive_flow);
    }

    // 0.ml2. Детектор ликвидационных каскадов: обновляем по каждому тику
    if (cascade_detector_ && snapshot.mid_price.get() > 0.0) {
        double tick_volume = snapshot.microstructure.bid_depth_5_notional
                           + snapshot.microstructure.ask_depth_5_notional;
        cascade_detector_->on_tick(
            snapshot.mid_price.get(),
            tick_volume,
            snapshot.microstructure.bid_depth_5_notional,
            snapshot.microstructure.ask_depth_5_notional);
    }

    // 0.ml3. Монитор корреляций: обновляем цену основного актива
    if (correlation_monitor_ && snapshot.mid_price.get() > 0.0) {
        correlation_monitor_->on_primary_tick(snapshot.mid_price.get());
    }

    // 0a. СТОП-ЛОСС: проверяем КАЖДЫЙ тик, независимо от стратегий.
    // Стоп-лосс имеет приоритет над всеми остальными решениями.
    // Если стоп-лосс активен — прекращаем обработку тика полностью.
    if (check_position_stop_loss(snapshot)) {
        return;
    }

    // 0b. Защита от торговли без прогрева.
    // 200 тиков ≈ 3-5 минут live данных — индикаторы стабилизируются.
    if (tick_count_ < kMinWarmupTicks) {
        if (tick_count_ == 1) {
            logger_->info("pipeline",
                "Прогрев: ожидание " + std::to_string(kMinWarmupTicks) + " тиков...");
        }
        return;
    }

    // 0c. Market Readiness Gate — не торгуем, пока не загружены HTF данные
    // и рынок не в безопасных условиях для входа.
    if (!check_market_readiness(snapshot)) {
        return;
    }

    // 0d. Alpha Decay: периодическая проверка деградации стратегий
    check_alpha_decay_feedback();

    // 0e. Периодическое обновление HTF индикаторов (каждый час или экстренно)
    maybe_update_htf(snapshot);

    // 0g. Фильтр энтропии: блокируем торговлю при зашумлённом рынке
    if (entropy_filter_ && entropy_filter_->is_noisy()) {
        if (tick_count_ % 200 == 0) {
            auto ent = entropy_filter_->compute();
            logger_->debug("pipeline",
                "Энтропийный фильтр: рынок зашумлён, торговля приостановлена",
                {{"composite_entropy", std::to_string(ent.composite_entropy)},
                 {"signal_quality", std::to_string(ent.signal_quality)}});
        }
        return;
    }

    // 0h. Детектор каскадов: блокируем торговлю при вероятном каскаде ликвидаций
    if (cascade_detector_ && cascade_detector_->is_cascade_likely()) {
        if (tick_count_ % 200 == 0) {
            auto sig = cascade_detector_->evaluate();
            logger_->warn("pipeline",
                "Каскад ликвидаций вероятен — торговля приостановлена",
                {{"probability", std::to_string(sig.probability)},
                 {"velocity", std::to_string(sig.price_velocity)},
                 {"volume_ratio", std::to_string(sig.volume_ratio)},
                 {"depth_ratio", std::to_string(sig.depth_ratio)},
                 {"direction", std::to_string(sig.direction)}});
        }
        return;
    }

    // 0i. Thompson Sampling: проверяем отложенный вход
    if (pending_entry_.has_value()) {
        auto& pe = *pending_entry_;
        --pe.wait_periods_remaining;
        if (pe.wait_periods_remaining <= 0) {
            // Период ожидания истёк — переходим к исполнению отложенного входа
            // (intent уже прошёл все проверки при первоначальном одобрении)
            logger_->info("pipeline", "Thompson: отложенный вход активирован",
                {{"action", ml::to_string(pe.action)},
                 {"strategy", pe.intent.strategy_id.get()}});
            // Обновляем цену intent'а до текущей
            if (snapshot.mid_price.get() > 0.0) {
                pe.intent.limit_price = snapshot.mid_price;
            }
            // Здесь не исполняем напрямую — позволяем пройти через основной pipeline ниже,
            // сбрасываем pending и продолжаем обработку тика как обычно
            pending_entry_.reset();
        } else {
            // Ещё ждём — пропускаем тик
            return;
        }
    }

    // 0f. TWAP: проверяем есть ли активный TWAP ордер для исполнения следующего слайса
    if (twap_executor_ && twap_executor_->active_twap().has_value()) {
        auto& twap = *twap_executor_->active_twap();
        if (!twap_executor_->is_complete(twap)) {
            auto now_ms = clock_->now().get() / 1'000'000;  // нс → мс
            auto slice_intent = twap_executor_->get_next_slice(twap, snapshot, now_ms);
            if (slice_intent) {
                // Исполняем слайс через обычный pipeline (размер уже рассчитан)
                auto exec_alpha = execution_alpha_->evaluate(*slice_intent, snapshot);
                risk::RiskDecision slice_risk;
                slice_risk.decided_at = clock_->now();
                slice_risk.approved_quantity = slice_intent->suggested_quantity;
                slice_risk.verdict = risk::RiskVerdict::Approved;
                slice_risk.summary = "TWAP slice";

                auto result = execution_engine_->execute(*slice_intent, slice_risk, exec_alpha);
                if (result) {
                    risk_engine_->record_order_sent();
                    twap_executor_->record_slice_fill(twap, twap.next_slice - 1,
                        slice_intent->suggested_quantity,
                        slice_intent->limit_price ? *slice_intent->limit_price : snapshot.mid_price);
                }
            }
            return;  // Не обрабатываем новые сигналы, пока TWAP активен
        } else {
            // TWAP завершён — сбрасываем
            twap_executor_->active_twap().reset();
        }
    }

    // 1. Мировая модель
    auto world = world_model_->update(snapshot);

    // 2. Режим рынка
    auto regime = regime_engine_->classify(snapshot);

    // 3. Неопределённость
    auto uncertainty = uncertainty_engine_->assess(snapshot, regime, world);

    // 4. Статус рынка каждые 100 тиков (~30 секунд)
    if (tick_count_ % 100 == 0) {
        print_status(snapshot, world, regime);
    }

    // 5. Высокая неопределённость — торговля приостановлена
    if (uncertainty.recommended_action == uncertainty::UncertaintyAction::NoTrade) {
        if (tick_count_ % 200 == 0) {
            logger_->info("pipeline", "Торговля приостановлена: Extreme неопределённость",
                {{"aggregate", std::to_string(uncertainty.aggregate_score)},
                 {"size_mult", std::to_string(uncertainty.size_multiplier)}});
        }
        return;
    }

    // 6. Аллокация стратегий по текущему режиму
    auto allocation = strategy_allocator_->allocate(
        strategy_registry_->active(), regime, world, uncertainty);

    // 7. Оценка каждой активной стратегии
    std::vector<strategy::TradeIntent> intents;

    // Проверяем текущую позицию ДО оценки стратегий.
    // На споте: без позиции нужны только BUY сигналы, с позицией — только SELL.
    // Фильтрация ДО стратегий экономит CPU и избавляет от лог-спама.
    bool has_long_position = false;
    {
        auto port_snap = portfolio_->snapshot();
        for (const auto& pos : port_snap.positions) {
            if (pos.symbol.get() == symbol_.get()
                && pos.side == Side::Buy
                && pos.size.get() >= 0.00001) {
                has_long_position = true;
                break;
            }
        }
    }

    // Диагностика: сколько стратегий сгенерировали сигналы, сколько отфильтровано
    int total_intents = 0;
    int filtered_intents = 0;

    for (const auto& strat : strategy_registry_->active()) {
        strategy::StrategyContext ctx;
        ctx.features = snapshot;
        ctx.regime = regime.label;
        ctx.world_state = world_model::WorldModelSnapshot::to_label(world.state);
        ctx.uncertainty = uncertainty.level;
        ctx.uncertainty_size_multiplier = uncertainty.size_multiplier;
        ctx.uncertainty_threshold_multiplier = uncertainty.threshold_multiplier;

        auto intent = strat->evaluate(ctx);
        if (intent.has_value()) {
            ++total_intents;
            // Фильтруем неисполнимые сигналы на споте:
            // - SELL без открытой long-позиции бесполезен
            // - BUY при уже открытой long-позиции — нарастить нельзя
            bool is_buy = (intent->side == Side::Buy);
            if (is_buy && has_long_position) { ++filtered_intents; continue; }
            if (!is_buy && !has_long_position) { ++filtered_intents; continue; }
            intents.push_back(std::move(*intent));
        }
    }

    // Логируем состояние сигналов каждые 200 тиков для диагностики
    if (tick_count_ % 200 == 0) {
        logger_->info("pipeline", "Итоги оценки стратегий",
            {{"total_intents", std::to_string(total_intents)},
             {"filtered", std::to_string(filtered_intents)},
             {"actionable", std::to_string(intents.size())},
             {"has_position", has_long_position ? "true" : "false"},
             {"tick", std::to_string(tick_count_)}});
    }

    if (intents.empty()) return;

    // 8. Агрегация решений комитетом
    auto decision = decision_engine_->aggregate(
        symbol_, intents, allocation, regime, world, uncertainty);

    if (!decision.trade_approved || !decision.final_intent.has_value()) {
        if (tick_count_ % 200 == 0) {
            logger_->info("pipeline", "Комитет не одобрил сигнал",
                {{"tick", std::to_string(tick_count_)},
                 {"trade_approved", decision.trade_approved ? "true" : "false"},
                 {"has_intent", decision.final_intent.has_value() ? "true" : "false"},
                 {"rationale", decision.rationale}});
        }
        return;
    }

    auto& intent = *decision.final_intent;

    // 8a.ml. Microstructure fingerprint: проверяем edge перед входом
    if (fingerprinter_) {
        auto fp = fingerprinter_->create_fingerprint(snapshot);
        double fp_edge = fingerprinter_->predict_edge(fp);
        if (fp_edge < -0.1) {
            if (tick_count_ % 200 == 0) {
                logger_->debug("pipeline",
                    "Fingerprint неблагоприятный — сигнал отклонён",
                    {{"edge", std::to_string(fp_edge)},
                     {"hash", std::to_string(fp.hash())}});
            }
            return;
        }
    }

    // 8a.bayes. Байесовская адаптация порога conviction
    double bayesian_conviction_adj = 0.0;
    if (bayesian_adapter_ && bayesian_adapter_->total_observations() >= 20) {
        double adapted_threshold = bayesian_adapter_->get_adapted_value(
            "global", "conviction_threshold", regime.detailed);
        // Разница между адаптированным и дефолтным порогом
        bayesian_conviction_adj = adapted_threshold - kDefaultConvictionThreshold;
    }

    // 8b. Alpha Decay: проверка порога conviction с учётом деградации
    {
        double effective_threshold = kDefaultConvictionThreshold
                                   + alpha_decay_threshold_adj_
                                   + bayesian_conviction_adj;

        // Time-of-Day: корректировка порога в неблагоприятные часы
        if (snapshot.technical.tod_valid && snapshot.technical.tod_alpha_score < -0.2) {
            effective_threshold += 0.05;  // Поднимаем порог в тихие часы
        }

        if (intent.conviction < effective_threshold) {
            if (tick_count_ % 200 == 0) {
                logger_->info("pipeline", "Conviction ниже порога",
                    {{"conviction", std::to_string(intent.conviction)},
                     {"threshold", std::to_string(effective_threshold)},
                     {"decay_adj", std::to_string(alpha_decay_threshold_adj_)},
                     {"bayesian_adj", std::to_string(bayesian_conviction_adj)},
                     {"strategy", intent.strategy_id.get()}});
            }
            return;
        }
    }

    // 8c. Корреляционный монитор: снижаем размер позиции при разрыве корреляции
    double correlation_risk_mult = 1.0;
    if (correlation_monitor_) {
        auto corr_result = correlation_monitor_->evaluate();
        correlation_risk_mult = corr_result.risk_multiplier;
        if (corr_result.any_break && tick_count_ % 200 == 0) {
            logger_->warn("pipeline",
                "Разрыв корреляции обнаружен — размер позиции снижен",
                {{"risk_mult", std::to_string(correlation_risk_mult)},
                 {"avg_corr", std::to_string(corr_result.avg_correlation)}});
        }
    }

    // 8d. Thompson Sampling: выбираем момент входа
    ml::EntryAction thompson_action = ml::EntryAction::EnterNow;
    if (thompson_sampler_) {
        thompson_action = thompson_sampler_->select_action();
        int wait = ml::wait_periods(thompson_action);

        if (thompson_action == ml::EntryAction::Skip) {
            // Пропускаем сигнал полностью
            if (tick_count_ % 200 == 0) {
                logger_->debug("pipeline",
                    "Thompson Sampling: сигнал пропущен",
                    {{"strategy", intent.strategy_id.get()}});
            }
            return;
        }

        if (wait > 0) {
            // Откладываем вход на N тиков
            PendingEntry pe;
            pe.intent = intent;
            pe.wait_periods_remaining = wait;
            pe.action = thompson_action;
            pending_entry_ = std::move(pe);
            logger_->debug("pipeline",
                "Thompson Sampling: вход отложен",
                {{"action", ml::to_string(thompson_action)},
                 {"wait_ticks", std::to_string(wait)},
                 {"strategy", intent.strategy_id.get()}});
            return;
        }
        // EnterNow — продолжаем немедленно
    }

    // 9_htf. HTF Trend Filter — ФИНАЛЬНЫЙ барьер перед отправкой ордера.
    // Даже если стратегия одобрила сигнал, мы блокируем его если он
    // идёт ПРОТИВ сильного тренда на старшем таймфрейме.
    // ВАЖНО: SELL для закрытия существующей позиции (take-profit) НЕ блокируется —
    // иначе бот не сможет зафиксировать прибыль в аптренде.
    if (htf_valid_) {
        // Определяем, есть ли открытая позиция для этого символа (для bypass SELL filter)
        bool has_open_long = portfolio_->has_position(symbol_);

        bool blocked = false;
        // Блокируем BUY в сильном даунтренде HTF (EMA20 < EMA50, сила > 0.4)
        if (intent.side == Side::Buy && htf_trend_direction_ == -1 && htf_trend_strength_ > 0.4) {
            // Разрешаем только если есть явный разворот: MACD > 0 И RSI > 40
            bool reversal_confirmed = (htf_macd_histogram_ > 0.0 && htf_rsi_14_ > 40.0);
            if (!reversal_confirmed) {
                blocked = true;
                if (tick_count_ % 200 == 0) {
                    logger_->warn("pipeline",
                        "HTF Trend Filter: BUY заблокирован — сильный даунтренд",
                        {{"htf_trend", std::to_string(htf_trend_direction_)},
                         {"htf_strength", std::to_string(htf_trend_strength_)},
                         {"htf_macd", std::to_string(htf_macd_histogram_)},
                         {"htf_rsi", std::to_string(htf_rsi_14_)},
                         {"strategy", intent.strategy_id.get()}});
                }
            }
        }
        // Блокируем SELL в сильном аптренде HTF, но ТОЛЬКО для новых коротких позиций.
        // SELL при наличии открытой длинной позиции = take-profit, его не блокируем.
        if (intent.side == Side::Sell && !has_open_long && htf_trend_direction_ == 1 && htf_trend_strength_ > 0.4) {
            bool reversal_confirmed = (htf_macd_histogram_ < 0.0 && htf_rsi_14_ < 60.0);
            if (!reversal_confirmed) {
                blocked = true;
                if (tick_count_ % 200 == 0) {
                    logger_->warn("pipeline",
                        "HTF Trend Filter: SELL заблокирован — сильный аптренд",
                        {{"htf_trend", std::to_string(htf_trend_direction_)},
                         {"htf_strength", std::to_string(htf_trend_strength_)},
                         {"strategy", intent.strategy_id.get()}});
                }
            }
        }
        if (blocked) {
            return;
        }
    }

    // 9a. Cooldown — не торгуем чаще чем раз в 30 секунд
    int64_t now_ns = clock_->now().get();
    if (last_order_time_ns_ > 0 && (now_ns - last_order_time_ns_) < kOrderCooldownNs) {
        return;
    }

    // 9b. Определяем тип операции: открытие или закрытие позиции.
    bool is_closing_position = false;
    Quantity closing_qty{0.0};
    double position_size_for_log = 0.0;
    double closing_pnl = 0.0; // P&L позиции для записи в decay/fingerprint/thompson
    {
        auto port_snap = portfolio_->snapshot();
        bool has_position = false;
        bool position_is_long = false;
        Quantity position_size{0.0};
        for (const auto& pos : port_snap.positions) {
            if (pos.symbol.get() == symbol_.get()) {
                has_position = true;
                position_is_long = (pos.side == Side::Buy);
                position_size = pos.size;
                closing_pnl = pos.unrealized_pnl;
                break;
            }
        }

        bool intent_is_buy = (intent.side == Side::Buy);

        // Минимальный торгуемый нотионал на Bitget ($1 USDT).
        // Пылевые остатки ниже этого порога не считаются реальной позицией.
        constexpr double kMinTradeableNotional = 0.50;  // $0.50 — ниже невозможно продать
        double position_notional = position_size.get() * snapshot.mid_price.get();

        // Нельзя SELL на спот без реальной длинной позиции (>= min notional)
        bool has_real_position = has_position && position_notional >= kMinTradeableNotional;
        if (!intent_is_buy && (!has_real_position || !position_is_long)) {
            logger_->debug("pipeline",
                "Пропуск SELL: нет длинной позиции для продажи",
                {{"symbol", symbol_.get()}});
            return;
        }

        // Не наращиваем позицию в том же направлении.
        // Пылевые позиции (< min notional) игнорируем — они не могут быть проданы
        // и не должны блокировать новые ордера.
        if (has_position && (position_is_long == intent_is_buy)
            && position_notional >= kMinTradeableNotional) {
            logger_->debug("pipeline",
                "Пропуск: уже есть позиция в том же направлении",
                {{"symbol", symbol_.get()},
                 {"side", intent_is_buy ? "BUY" : "SELL"},
                 {"size", std::to_string(position_size.get())}});
            return;
        }

        // SELL при открытой BUY = закрытие позиции (только если нотионал достаточный)
        if (!intent_is_buy && has_position && position_is_long
            && position_notional >= kMinTradeableNotional) {
            is_closing_position = true;
            closing_qty = position_size;
            position_size_for_log = position_size.get();
        }
    }

    // 9c. Установить цену для расчёта размера позиции (если стратегия не задала)
    if (!intent.limit_price.has_value() && snapshot.mid_price.get() > 0.0) {
        intent.limit_price = snapshot.mid_price;
    }

    risk::RiskDecision risk_decision;
    risk_decision.decided_at = clock_->now();

    // Для маленьких аккаунтов (< $100) гарантируем быстрое исполнение:
    // urgency >= 0.8 → Aggressive → market order.
    // Стоимость проскальзывания на $15 ордере ~ 0.01%, стоимость
    // незаполненного лимитного ордера (потерянный вход) гораздо выше.
    if (intent.side == Side::Buy && portfolio_->snapshot().total_capital < 100.0) {
        intent.urgency = std::max(intent.urgency, 0.8);
    }

    // Execution alpha — вычисляем один раз для всех ветвей
    auto exec_alpha = execution_alpha_->evaluate(intent, snapshot);

    if (is_closing_position) {
        // Закрытие позиции — обходим allocator и risk проверки экспозиции,
        // т.к. закрытие УМЕНЬШАЕТ риск, а не увеличивает.
        // Запрашиваем актуальный баланс base coin, чтобы избежать "Insufficient balance"
        // (портфельный размер может не учитывать комиссии/пыль)
        std::string base_coin = extract_base_coin(symbol_.get());
        double actual_base = query_asset_balance(base_coin);
        if (actual_base > 1e-8) {
            closing_qty = Quantity(actual_base);
            logger_->info("pipeline", "Актуальный баланс " + base_coin + " для закрытия",
                {{"portfolio_qty", std::to_string(position_size_for_log)},
                 {"exchange_qty", std::to_string(actual_base)}});
        }

        // Минимальный ордер Bitget: 0.00001 BTC (или ~$5)
        if (closing_qty.get() < 0.00001) {
            logger_->warn("pipeline", "BTC баланс слишком мал для закрытия",
                {{"qty", std::to_string(closing_qty.get())}});
            return;
        }

        risk_decision.approved_quantity = closing_qty;
        risk_decision.verdict = risk::RiskVerdict::Approved;
        risk_decision.summary = "Закрытие позиции — размер из реального баланса биржи";
        intent.suggested_quantity = closing_qty;

        logger_->info("pipeline", "Закрытие позиции",
            {{"symbol", symbol_.get()},
             {"qty", std::to_string(closing_qty.get())}});
    } else {
        // 10. Расчёт размера для нового ордера

        // Volatility Targeting: передаём рыночный контекст в аллокатор
        {
            double realized_vol_annual = 0.0;
            if (snapshot.technical.volatility_valid && snapshot.technical.volatility_20 > 0.0) {
                // volatility_20 — реализованная 20-периодная волатильность (std dev доходностей).
                // Данные приходят из 1-минутных свечей, поэтому аннуализация:
                // × sqrt(минут_в_году) = sqrt(365 * 24 * 60) = sqrt(525600) ≈ 725.3
                realized_vol_annual = snapshot.technical.volatility_20 * std::sqrt(525600.0);
            }

            // Реальные win_rate/win_loss_ratio из alpha_decay_monitor
            double win_rate = 0.5;
            double win_loss_ratio = 1.5;
            {
                auto reports = alpha_decay_monitor_->get_all_reports();
                if (!reports.empty()) {
                    // Средний health по всем стратегиям как прокси win_rate
                    double total_health = 0.0;
                    for (const auto& r : reports) {
                        total_health += r.overall_health;
                    }
                    double avg_health = total_health / static_cast<double>(reports.size());
                    // Корректируем win_rate: health 1.0 → 0.55, health 0.0 → 0.30
                    win_rate = 0.30 + avg_health * 0.25;
                    // win_loss_ratio снижается при деградации
                    win_loss_ratio = 1.0 + avg_health * 0.5;
                }
            }

            portfolio_allocator_->set_market_context(
                realized_vol_annual, regime.detailed, win_rate, win_loss_ratio);
        }

        auto sizing = portfolio_allocator_->compute_size(
            intent, portfolio_->snapshot(),
            uncertainty.size_multiplier * alpha_decay_size_mult_ * correlation_risk_mult);

        if (!sizing.approved || sizing.approved_quantity.get() <= 0.0) {
            logger_->debug("pipeline", "Размер позиции не одобрен аллокатором");
            return;
        }

        // 11. Проверка риск-движком — ОБЯЗАТЕЛЬНА для новых позиций
        risk_decision = risk_engine_->evaluate(
            intent, sizing, portfolio_->snapshot(), snapshot, exec_alpha);

        if (risk_decision.verdict == risk::RiskVerdict::Denied ||
            risk_decision.verdict == risk::RiskVerdict::Throttled) {
            logger_->warn("pipeline", "Сделка отклонена риск-движком",
                {{"verdict", risk::to_string(risk_decision.verdict)},
                 {"reasons", std::to_string(risk_decision.reasons.size())}});
            return;
        }
    }

    // 11a. Smart TWAP: разбиваем крупные ордера на слайсы
    if (twap_executor_ && twap_executor_->should_use_twap(intent, snapshot)) {
        auto twap_plan = twap_executor_->create_twap_plan(
            intent, snapshot, risk_decision.approved_quantity);
        twap_executor_->active_twap() = twap_plan;
        logger_->info("pipeline", "Активирован Smart TWAP",
            {{"symbol", symbol_.get()},
             {"total_qty", std::to_string(twap_plan.total_qty.get())},
             {"slices", std::to_string(twap_plan.slices.size())}});
        return;  // Первый слайс отправится на следующем тике
    }

    // 12. Исполнение ордера
    // Устанавливаем cooldown ПЕРЕД отправкой — чтобы даже при ошибке
    // не спамить биржу повторными запросами.
    last_order_time_ns_ = clock_->now().get();
    last_activity_ns_.store(last_order_time_ns_, std::memory_order_relaxed);

    auto order_result = execution_engine_->execute(intent, risk_decision, exec_alpha);
    if (order_result) {
        consecutive_rejections_ = 0;  // Успешный ордер — сброс backoff
        risk_engine_->record_order_sent();
        logger_->info("pipeline", "ОРДЕР ОТПРАВЛЕН",
            {{"order_id", order_result->get()},
             {"side", intent.side == Side::Buy ? "BUY" : "SELL"},
             {"qty", std::to_string(risk_decision.approved_quantity.get())},
             {"symbol", symbol_.get()},
             {"conviction", std::to_string(intent.conviction)}});

        // Инициализация trailing stop при открытии новой позиции
        if (intent.side == Side::Buy) {
            current_position_strategy_ = intent.strategy_id;
            current_position_conviction_ = intent.conviction;
            current_entry_thompson_action_ = thompson_action;
            reset_trailing_state();
            double entry_price = snapshot.mid_price.get();
            highest_price_since_entry_ = entry_price;
            lowest_price_since_entry_ = entry_price;
            initial_position_size_ = risk_decision.approved_quantity.get();

            // Сохраняем fingerprint на входе для записи результата при закрытии
            if (fingerprinter_) {
                last_entry_fingerprint_ = fingerprinter_->create_fingerprint(snapshot);
            }
        }

        // Сброс trailing state при полном закрытии позиции (SELL всей позиции)
        // Execution engine уже вызовет portfolio_->close_position() при fill.
        // Здесь только записываем данные для аналитики.
        if (intent.side == Side::Sell && is_closing_position) {
            record_trade_for_decay(current_position_strategy_,
                closing_pnl,
                current_position_conviction_);

            // Записываем результат fingerprint и байесовское наблюдение
            if (fingerprinter_ && last_entry_fingerprint_) {
                double norm_pnl = (closing_pnl > 0) ? 1.0 : -1.0;
                fingerprinter_->record_outcome(*last_entry_fingerprint_, norm_pnl);
                last_entry_fingerprint_.reset();
            }
            if (bayesian_adapter_) {
                ml::ParameterObservation obs;
                obs.reward = closing_pnl;
                obs.regime = regime.detailed;
                bayesian_adapter_->record_observation("global", obs);
            }

            // Thompson Sampling: бинарная награда для бандита
            // Используем действие, выбранное при ВХОДЕ в позицию, а не текущее
            if (thompson_sampler_) {
                double ts_reward = (closing_pnl > 0) ? 1.0 : -1.0;
                thompson_sampler_->record_reward(current_entry_thompson_action_, ts_reward);
            }

            // Записываем результат в risk engine
            risk_engine_->record_trade_result(closing_pnl < 0.0);

            reset_trailing_state();
        }
    } else {
        ++consecutive_rejections_;
        // Экспоненциальный backoff: 30с × 2^(rejections-1), макс 10 минут
        int64_t backoff = kOrderCooldownNs * (1LL << std::min(consecutive_rejections_, 8));
        backoff = std::min(backoff, kMaxRejectionBackoffNs);
        last_order_time_ns_ = clock_->now().get() + backoff - kOrderCooldownNs;
        logger_->warn("pipeline", "Ордер не исполнен",
            {{"side", intent.side == Side::Buy ? "BUY" : "SELL"},
             {"symbol", symbol_.get()},
             {"consecutive_rejections", std::to_string(consecutive_rejections_)},
             {"backoff_sec", std::to_string(backoff / 1'000'000'000LL)}});
    }
}

// ==================== Статус ====================

void TradingPipeline::print_status(
    const features::FeatureSnapshot& snap,
    const world_model::WorldModelSnapshot& world,
    const regime::RegimeSnapshot& regime)
{
    // Адаптивная точность: для дешёвых токенов ($0.02) нужно больше знаков
    auto adaptive_prec = [](double v) -> int {
        if (v <= 0.0) return 2;
        if (v < 0.001) return 8;
        if (v < 0.01) return 6;
        if (v < 0.1) return 5;
        if (v < 1.0) return 4;
        if (v < 100.0) return 3;
        return 2;
    };

    auto fmt = [](double v, int prec = 2) {
        std::ostringstream s;
        s << std::fixed << std::setprecision(prec) << v;
        return s.str();
    };

    // Основные индикаторы для диагностики стратегий
    std::string ema_signal = (snap.technical.ema_20 > snap.technical.ema_50)
                             ? "BULL" : "BEAR";
    double bb_pos = 0.0;  // Позиция цены в BB каналe: <0=ниже, >1=выше
    if (snap.technical.bb_upper > snap.technical.bb_lower) {
        bb_pos = (snap.mid_price.get() - snap.technical.bb_lower) /
                 (snap.technical.bb_upper - snap.technical.bb_lower);
    }

    int price_prec = adaptive_prec(snap.mid_price.get());
    int atr_prec = adaptive_prec(snap.technical.atr_14);

    logger_->info("pipeline", "Статус рынка",
        {{"tick", std::to_string(tick_count_)},
         {"price", fmt(snap.mid_price.get(), price_prec)},
         {"rsi14", fmt(snap.technical.rsi_14, 1)},
         {"ema", ema_signal},
         {"bb_pos", fmt(bb_pos, 2)},
         {"atr", fmt(snap.technical.atr_14, atr_prec)},
         {"regime", std::string(to_string(regime.label))},
         {"world", std::string(world_model::to_string(world.state))},
         {"positions", std::to_string(portfolio_->snapshot().exposure.open_positions_count)}});
}

} // namespace tb::pipeline
