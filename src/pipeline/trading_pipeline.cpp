/**
 * @file trading_pipeline.cpp
 * @brief Реализация торгового pipeline
 */

#include "pipeline/trading_pipeline.hpp"
#include "exchange/bitget/bitget_rest_client.hpp"
#include "exchange/bitget/bitget_order_submitter.hpp"
#include "common/enums.hpp"
#include "common/constants.hpp"
#include "persistence/postgres_storage_adapter.hpp"
#include <boost/json.hpp>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cmath>
#include <numeric>
#include <cstdlib>

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
    const std::string& symbol)
    : config_(config)
    , symbol_(Symbol(symbol.empty() ? "BTCUSDT" : symbol))
    , secret_provider_(std::move(secret_provider))
    , logger_(std::move(logger))
    , clock_(std::move(clock))
    , metrics_(std::move(metrics))
{
    // 1. Индикаторы и признаки
    indicator_engine_ = std::make_shared<indicators::IndicatorEngine>(logger_);
    feature_engine_ = std::make_shared<features::FeatureEngine>(
        features::FeatureEngine::Config{}, indicator_engine_, clock_, logger_, metrics_);

    // 2. Стакан ордеров
    order_book_ = std::make_shared<order_book::LocalOrderBook>(symbol_, logger_, metrics_);

    // 3. Аналитический слой
    world_model_ = std::make_shared<world_model::RuleBasedWorldModelEngine>(logger_, clock_);
    regime_engine_ = std::make_shared<regime::RuleBasedRegimeEngine>(logger_, clock_, metrics_, config_.regime);
    uncertainty_engine_ = std::make_shared<uncertainty::RuleBasedUncertaintyEngine>(
        uncertainty::UncertaintyConfig{}, logger_, clock_);

    // 4. Стратегии
    strategy_registry_ = std::make_shared<strategy::StrategyRegistry>();
    strategy_registry_->register_strategy(
        std::make_shared<strategy::StrategyEngine>(logger_, clock_));

    // 5. Аллокация стратегий и решения
    strategy_allocator_ = std::make_shared<strategy_allocator::RegimeAwareAllocator>(logger_, clock_);

    // Advanced decision config — маппинг из AppConfig
    decision::AdvancedDecisionConfig adv_decision;
    adv_decision.enable_regime_threshold_scaling = config_.decision.enable_regime_threshold_scaling;
    adv_decision.enable_regime_dominance_scaling = config_.decision.enable_regime_dominance_scaling;
    adv_decision.enable_time_decay = config_.decision.enable_time_decay;
    adv_decision.time_decay_halflife_ms = config_.decision.time_decay_halflife_ms;
    adv_decision.enable_ensemble_conviction = config_.decision.enable_ensemble_conviction;
    adv_decision.ensemble_agreement_bonus = config_.decision.ensemble_agreement_bonus;
    adv_decision.ensemble_max_bonus = config_.decision.ensemble_max_bonus;
    adv_decision.enable_portfolio_awareness = config_.decision.enable_portfolio_awareness;
    adv_decision.drawdown_boost_scale = config_.decision.drawdown_boost_scale;
    adv_decision.drawdown_max_boost = config_.decision.drawdown_max_boost;
    adv_decision.consecutive_loss_boost = config_.decision.consecutive_loss_boost;
    adv_decision.enable_execution_cost_modeling = config_.decision.enable_execution_cost_modeling;
    adv_decision.max_acceptable_cost_bps = config_.decision.max_acceptable_cost_bps;
    adv_decision.enable_time_skew_detection = config_.decision.enable_time_skew_detection;

    decision_engine_ = std::make_shared<decision::CommitteeDecisionEngine>(
        logger_, clock_,
        config_.decision.min_conviction_threshold,
        config_.decision.conflict_dominance_threshold,
        adv_decision);

    // Общий PostgreSQL storage — создаём один раз, передаём всем компонентам.
    // Если POSTGRES_URL не задан или подключение не удалось — nullptr (in-memory fallback).
    std::shared_ptr<persistence::IStorageAdapter> shared_pg_storage;
    if (const char* pg_url = std::getenv("POSTGRES_URL"); pg_url && *pg_url) {
        try {
            shared_pg_storage = persistence::make_postgres_adapter(pg_url);
            logger_->info("pipeline", "PostgreSQL storage подключён",
                {{"url_prefix", std::string(pg_url).substr(0, 20) + "..."}});
        } catch (const std::exception& ex) {
            logger_->warn("pipeline",
                "Не удалось подключиться к PostgreSQL, работаем in-memory",
                {{"error", ex.what()}});
        }
    }

    // 5.5. Продвинутые features (CUSUM, VPIN, Volume Profile, Time-of-Day)
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
    if (config_.futures.enabled) {
        portfolio_->set_leverage(static_cast<double>(config_.futures.default_leverage));
    }

    // 7. Размер позиции (адаптация под размер капитала)
    portfolio_allocator::HierarchicalAllocator::Config alloc_cfg;
    alloc_cfg.budget.global_budget = config_.trading.initial_capital;
    // Для фьючерсов — масштабировать бюджет на leverage
    if (config_.futures.enabled) {
        alloc_cfg.max_leverage = static_cast<double>(config_.futures.default_leverage);
        alloc_cfg.budget.global_budget *= alloc_cfg.max_leverage;
    }
    // Для маленьких аккаунтов (< $100) — ослабляем лимиты концентрации
    // и повышаем target_vol, чтобы не блокировать минимальные ордера биржи ($1 USDT)
    if (config_.trading.initial_capital < 100.0) {
        alloc_cfg.max_concentration_pct = 0.35;
        alloc_cfg.max_strategy_allocation_pct = 0.50;
        alloc_cfg.budget.symbol_budget_pct = 0.50;
        alloc_cfg.target_annual_vol = 1.50;       // 150% — адекватнее для крипто-фьючерсов
        alloc_cfg.min_size_multiplier = 0.60;     // Не ниже 60% — малый аккаунт не может дробить
        alloc_cfg.kelly_fraction = 0.50;           // Half-Kelly — не ждать статистику
    }
    portfolio_allocator_ = std::make_shared<portfolio_allocator::HierarchicalAllocator>(
        alloc_cfg, logger_);

    // 8. Execution alpha — инициализируется из конфигурации (не хардкод)
    {
        execution_alpha::RuleBasedExecutionAlpha::Config ea_cfg;
        ea_cfg.max_spread_bps_passive          = config_.execution_alpha.max_spread_bps_passive;
        ea_cfg.max_spread_bps_any              = config_.execution_alpha.max_spread_bps_any;
        ea_cfg.adverse_selection_threshold     = config_.execution_alpha.adverse_selection_threshold;
        ea_cfg.urgency_passive_threshold       = config_.execution_alpha.urgency_passive_threshold;
        ea_cfg.urgency_aggressive_threshold    = config_.execution_alpha.urgency_aggressive_threshold;
        ea_cfg.large_order_slice_threshold     = config_.execution_alpha.large_order_slice_threshold;
        ea_cfg.vpin_toxic_threshold            = config_.execution_alpha.vpin_toxic_threshold;
        ea_cfg.vpin_weight                     = config_.execution_alpha.vpin_weight;
        ea_cfg.imbalance_favorable_threshold   = config_.execution_alpha.imbalance_favorable_threshold;
        ea_cfg.imbalance_unfavorable_threshold = config_.execution_alpha.imbalance_unfavorable_threshold;
        ea_cfg.use_weighted_mid_price          = config_.execution_alpha.use_weighted_mid_price;
        ea_cfg.limit_price_passive_bps         = config_.execution_alpha.limit_price_passive_bps;
        ea_cfg.urgency_cusum_boost             = config_.execution_alpha.urgency_cusum_boost;
        ea_cfg.urgency_tod_weight              = config_.execution_alpha.urgency_tod_weight;
        ea_cfg.min_fill_probability_passive    = config_.execution_alpha.min_fill_probability_passive;
        ea_cfg.postonly_spread_threshold_bps   = config_.execution_alpha.postonly_spread_threshold_bps;
        ea_cfg.postonly_urgency_max            = config_.execution_alpha.postonly_urgency_max;
        ea_cfg.postonly_adverse_max            = config_.execution_alpha.postonly_adverse_max;
        execution_alpha_ = std::make_shared<execution_alpha::RuleBasedExecutionAlpha>(
            std::move(ea_cfg), logger_, clock_, metrics_);
    }

    // 8a. Opportunity cost — модуль ранжирования / gating возможностей
    {
        opportunity_cost::OpportunityCostConfig oc_cfg;
        oc_cfg.min_net_expected_bps       = config_.opportunity_cost.min_net_expected_bps;
        oc_cfg.execute_min_net_bps        = config_.opportunity_cost.execute_min_net_bps;
        oc_cfg.high_exposure_threshold    = config_.opportunity_cost.high_exposure_threshold;
        oc_cfg.high_exposure_min_conviction = config_.opportunity_cost.high_exposure_min_conviction;
        oc_cfg.max_symbol_concentration   = config_.opportunity_cost.max_symbol_concentration;
        oc_cfg.max_strategy_concentration = config_.opportunity_cost.max_strategy_concentration;
        oc_cfg.capital_exhaustion_threshold = config_.opportunity_cost.capital_exhaustion_threshold;
        oc_cfg.weight_conviction          = config_.opportunity_cost.weight_conviction;
        oc_cfg.weight_net_edge            = config_.opportunity_cost.weight_net_edge;
        oc_cfg.weight_capital_efficiency  = config_.opportunity_cost.weight_capital_efficiency;
        oc_cfg.weight_urgency             = config_.opportunity_cost.weight_urgency;
        oc_cfg.conviction_to_bps_scale    = config_.opportunity_cost.conviction_to_bps_scale;
        oc_cfg.upgrade_min_edge_advantage_bps = config_.opportunity_cost.upgrade_min_edge_advantage_bps;
        oc_cfg.drawdown_penalty_scale     = config_.opportunity_cost.drawdown_penalty_scale;

        opportunity_cost_engine_ = std::make_shared<opportunity_cost::RuleBasedOpportunityCost>(
            std::move(oc_cfg), logger_, clock_, metrics_);
    }

    // 9. Риск-движок
    risk::ExtendedRiskConfig risk_cfg;
    risk_cfg.max_position_notional = config_.risk.max_position_notional;
    risk_cfg.max_daily_loss_pct = config_.risk.max_daily_loss_pct;
    risk_cfg.max_drawdown_pct = config_.risk.max_drawdown_pct;
    risk_cfg.kill_switch_enabled = config_.risk.kill_switch_enabled;
    risk_cfg.max_loss_per_trade_pct = config_.trading_params.max_loss_per_trade_pct;
    risk_cfg.max_position_hold_ns = static_cast<int64_t>(config_.trading_params.max_hold_absolute_minutes) * 60'000'000'000LL;
    risk_cfg.post_loss_cooldown_ns = static_cast<int64_t>(config_.trading_params.stop_loss_cooldown_seconds) * 1'000'000'000LL;
    // Расширенные параметры из конфига
    risk_cfg.max_strategy_daily_loss_pct = config_.risk.max_strategy_daily_loss_pct;
    risk_cfg.max_strategy_exposure_pct = config_.risk.max_strategy_exposure_pct;
    risk_cfg.max_symbol_concentration_pct = config_.risk.max_symbol_concentration_pct;
    risk_cfg.max_same_direction_positions = config_.risk.max_same_direction_positions;
    risk_cfg.regime_aware_limits_enabled = config_.risk.regime_aware_limits_enabled;
    risk_cfg.stress_regime_scale = config_.risk.stress_regime_scale;
    risk_cfg.trending_regime_scale = config_.risk.trending_regime_scale;
    risk_cfg.chop_regime_scale = config_.risk.chop_regime_scale;
    risk_cfg.max_trades_per_hour = config_.risk.max_trades_per_hour;
    risk_cfg.min_trade_interval_ns = static_cast<int64_t>(config_.risk.min_trade_interval_sec * 1'000'000'000LL);
    risk_cfg.max_adverse_excursion_pct = config_.risk.max_adverse_excursion_pct;
    risk_cfg.max_realized_daily_loss_pct = config_.risk.max_realized_daily_loss_pct;
    risk_cfg.utc_cutoff_hour = config_.risk.utc_cutoff_hour;
    // Для маленьких аккаунтов — снижаем порог минимальной ликвидности
    if (config_.trading.initial_capital < 100.0) {
        risk_cfg.min_liquidity_depth = 1.0;
    }
    // v11: Фьючерсы с плечом — экспозиция и leverage пропорциональны default_leverage
    // С плечом 20x notional = 20× capital → exposure = 2000%
    if (config_.futures.enabled) {
        const double lev = static_cast<double>(config_.futures.default_leverage);
        risk_cfg.max_gross_exposure_pct = lev * 150.0;  // 20x → 3000% headroom
        risk_cfg.max_leverage = lev * 1.5;               // 20x → 30x headroom
        risk_cfg.max_symbol_concentration_pct = lev * 150.0; // Same for single-pair
    }
    risk_engine_ = std::make_shared<risk::ProductionRiskEngine>(
        risk_cfg, logger_, clock_, metrics_);

    // 9.5. Адаптивное управление кредитным плечом (USDT-M фьючерсы)
    // Инстанцируется ТОЛЬКО если указано futures.enabled в конфиге
    if (config_.futures.enabled) {
        leverage_engine_ = std::make_shared<leverage::LeverageEngine>(config_.futures);
        logger_->info("pipeline", "LeverageEngine инициализирован",
            {{"max_leverage", std::to_string(config_.futures.max_leverage)},
             {"margin_mode", config_.futures.margin_mode},
             {"liquidation_buffer_pct", std::to_string(config_.futures.liquidation_buffer_pct)}});
    } else {
        logger_->info("pipeline", "Фьючерсный режим отключен (futures.enabled=false)");
    }

    // 10. Исполнение — выбор submitter в зависимости от режима и типа торговли (spot vs futures)
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

        // Выбор в зависимости от типа торговли
        if (config_.futures.enabled) {
            // Фьючерсный режим
            auto futures_sub = std::make_shared<exchange::bitget::BitgetFuturesOrderSubmitter>(
                rest_client, logger_, config_.futures);
            futures_submitter_ = futures_sub;
            submitter = futures_sub;
            logger_->info("pipeline",
                "Режим исполнения: ФЬЮЧЕРСЫ USDT-M (Bitget Mix API v2)");
        } else {
            // Спотовый режим
            auto bitget_sub = std::make_shared<exchange::bitget::BitgetOrderSubmitter>(
                rest_client, logger_);
            bitget_submitter_ = bitget_sub;  // Сохраняем для set_symbol_precision
            submitter = bitget_sub;
            logger_->info("pipeline",
                "Режим исполнения: СПОТ (Bitget REST API v2)");
        }
    } else {
        // Paper/Shadow — локальная симуляция
        submitter = std::make_shared<execution::PaperOrderSubmitter>();
        logger_->info("pipeline",
            "Режим исполнения: Paper (локальная симуляция)");
    }

    execution_engine_ = std::make_shared<execution::ExecutionEngine>(
        submitter, portfolio_, logger_, clock_, metrics_);
    if (config_.futures.enabled) {
        execution_engine_->set_leverage(static_cast<double>(config_.futures.default_leverage));
    }

    // 10a. Smart TWAP — адаптивное нарезание крупных ордеров
    twap_executor_ = std::make_shared<execution::SmartTwapExecutor>(
        execution::TwapConfig{}, logger_);

    // 11. Шлюз рыночных данных
    market_data::GatewayConfig gw_cfg;
    gw_cfg.ws_config.url = config_.exchange.endpoint_ws;
    gw_cfg.symbols = {symbol_};
    if (config_.futures.enabled) {
        gw_cfg.inst_type = "USDT-FUTURES";
    }

    gateway_ = std::make_shared<market_data::MarketDataGateway>(
        gw_cfg, feature_engine_, order_book_, logger_, metrics_, clock_,
        [this](features::FeatureSnapshot snap) { on_feature_snapshot(std::move(snap)); });

    logger_->info("pipeline", "Торговый pipeline создан",
        {{"symbol", symbol_.get()},
         {"strategies", std::to_string(strategy_registry_->active().size())}});

    // ==================== Phase 1: Latency Tracker ====================
    latency_tracker_ = std::make_unique<PipelineLatencyTracker>(metrics_, logger_);
    logger_->info("pipeline", "Latency tracker инициализирован");

    // ==================== Phase 2: Order Watchdog ====================
    {
        OrderWatchdogConfig wdcfg;
        order_watchdog_ = std::make_unique<OrderWatchdog>(
            wdcfg, execution_engine_, logger_, clock_, metrics_);
        order_watchdog_->set_cancel_callback([this](const OrderId& id, const std::string& reason) {
            logger_->warn("watchdog", "Ордер принудительно отменён watchdog-ом",
                {{"order_id", id.get()}, {"reason", reason}});
        });
        logger_->info("pipeline", "Order watchdog инициализирован",
            {{"max_open_ms", "30000"}, {"max_partial_ms", "60000"}});
    }

    // ==================== Phase 4: Reconciliation ====================
    // Инициализируем reconciliation только для production/testnet (нужен REST)
    if (rest_client_) {
        if (config_.futures.enabled) {
            // Фьючерсный режим — используем futures query adapter
            futures_query_adapter_ = std::make_shared<exchange::bitget::BitgetFuturesQueryAdapter>(
                rest_client_, logger_, config_.futures);
            reconciliation::ReconciliationConfig rec_cfg;
            rec_cfg.auto_resolve_state_mismatches = true;
            rec_cfg.balance_tolerance_pct = 1.0;
            reconciliation_engine_ = std::make_shared<reconciliation::ReconciliationEngine>(
                rec_cfg, futures_query_adapter_, logger_, clock_, metrics_);
            logger_->info("pipeline", "Continuous reconciliation engine инициализирован (Futures режим)");
        } else {
            // Спотовый режим — используем spot query adapter
            exchange_query_adapter_ = std::make_shared<exchange::bitget::BitgetExchangeQueryAdapter>(
                rest_client_, logger_);
            reconciliation::ReconciliationConfig rec_cfg;
            rec_cfg.auto_resolve_state_mismatches = true;
            rec_cfg.balance_tolerance_pct = 1.0;
            reconciliation_engine_ = std::make_shared<reconciliation::ReconciliationEngine>(
                rec_cfg, exchange_query_adapter_, logger_, clock_, metrics_);
            logger_->info("pipeline", "Continuous reconciliation engine инициализирован (Spot режим)");
        }
    }
}

TradingPipeline::~TradingPipeline() {
    stop();
}

void TradingPipeline::set_symbol_precision(int quantity_precision, int price_precision) {
    ExchangeSymbolRules rules;
    rules.symbol = symbol_;
    rules.quantity_precision = quantity_precision;
    rules.price_precision = price_precision;
    set_exchange_rules(rules);
}

void TradingPipeline::set_exchange_rules(const ExchangeSymbolRules& rules) {
    exchange_rules_ = rules;
    if (bitget_submitter_) {
        bitget_submitter_->set_rules(rules.symbol, rules);
        logger_->info("pipeline", "Exchange rules установлены (Spot)",
            {{"symbol", rules.symbol.get()},
             {"qty_precision", std::to_string(rules.quantity_precision)},
             {"price_precision", std::to_string(rules.price_precision)},
             {"min_trade_usdt", std::to_string(rules.min_trade_usdt)}});
    }
    if (futures_submitter_) {
        futures_submitter_->set_rules(rules.symbol, rules);
        logger_->info("pipeline", "Exchange rules установлены (Futures)",
            {{"symbol", rules.symbol.get()},
             {"qty_precision", std::to_string(rules.quantity_precision)},
             {"price_precision", std::to_string(rules.price_precision)},
             {"min_trade_usdt", std::to_string(rules.min_trade_usdt)}});
    }
}

// ==================== Загрузка точности ордеров ====================

void TradingPipeline::fetch_symbol_precision() {
    if (!rest_client_ || (!bitget_submitter_ && !futures_submitter_)) return;

    try {
        // Выбор endpoint в зависимости от режима торговли
        std::string endpoint = config_.futures.enabled
            ? "/api/v2/mix/market/contracts"
            : "/api/v2/spot/public/symbols";
        std::string params = config_.futures.enabled
            ? "productType=USDT-FUTURES&symbol=" + symbol_.get()
            : "symbol=" + symbol_.get();

        auto resp = rest_client_->get(endpoint, params);
        if (!resp.success) {
            logger_->warn("pipeline", "Не удалось запросить precision для " + symbol_.get(),
                {{"endpoint", endpoint}});
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
            double min_trade = 1.0;
            double min_qty = 0.0;
            // Spot API: quantityPrecision, pricePrecision
            // Futures API: volumePlace, pricePlace
            if (o.contains("quantityPrecision")) {
                qty_prec = static_cast<int>(parse_json_double(o.at("quantityPrecision")));
            } else if (o.contains("volumePlace")) {
                qty_prec = static_cast<int>(parse_json_double(o.at("volumePlace")));
            }
            if (o.contains("pricePrecision")) {
                price_prec = static_cast<int>(parse_json_double(o.at("pricePrecision")));
            } else if (o.contains("pricePlace")) {
                price_prec = static_cast<int>(parse_json_double(o.at("pricePlace")));
            }
            if (o.contains("minTradeUSDT")) {
                min_trade = parse_json_double(o.at("minTradeUSDT"));
                if (min_trade <= 0.0) min_trade = 1.0;
            }
            if (o.contains("minTradeNum")) {
                min_qty = parse_json_double(o.at("minTradeNum"));
            }

            ExchangeSymbolRules rules;
            rules.symbol = symbol_;
            rules.quantity_precision = qty_prec;
            rules.price_precision = price_prec;
            rules.min_trade_usdt = min_trade;
            rules.min_quantity = min_qty;
            set_exchange_rules(rules);
            logger_->info("pipeline", "Precision загружена с биржи",
                {{"symbol", sym},
                 {"qty_precision", std::to_string(qty_prec)},
                 {"price_precision", std::to_string(price_prec)},
                 {"min_trade_usdt", std::to_string(min_trade)},
                 {"min_quantity", std::to_string(min_qty)}});
            return;
        }
    } catch (const std::exception& e) {
        logger_->warn("pipeline", "Ошибка загрузки precision: " + std::string(e.what()));
    }
}

// ==================== Синхронизация баланса ====================

void TradingPipeline::sync_balance_from_exchange() {
    logger_->info("pipeline", "Запрос балансов с биржи...");

    // Выбор endpoint в зависимости от режима торговли
    std::string endpoint = config_.futures.enabled
        ? "/api/v2/mix/account/accounts?productType=USDT-FUTURES"
        : "/api/v2/spot/account/assets";

    // Запрашиваем ВСЕ ассеты (без фильтра по монете)
    auto resp = rest_client_->get(endpoint);
    if (!resp.success) {
        logger_->warn("pipeline", "Не удалось запросить баланс",
            {{"error", resp.error_message},
             {"endpoint", endpoint}});
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
            // Futures API uses "marginCoin", Spot API uses "coin"
            std::string coin;
            bool is_futures = asset.contains("marginCoin");
            if (is_futures) {
                coin = std::string(asset.at("marginCoin").as_string());
            } else if (asset.contains("coin")) {
                coin = std::string(asset.at("coin").as_string());
            } else {
                continue;
            }
            double available = 0.0;
            if (is_futures && asset.contains("usdtEquity")) {
                // Для фьючерсов используем usdtEquity (полный equity),
                // а не available (который уже вычел margin открытых позиций).
                // Это предотвращает двойной учёт margin при расчёте portfolio exposure.
                available = std::stod(
                    std::string(asset.at("usdtEquity").as_string()));
            } else if (asset.contains("available")) {
                available = std::stod(
                    std::string(asset.at("available").as_string()));
            }

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
                    } catch (...) { logger_->debug("pipeline", "Не удалось разобрать usdtValue для dust-фильтра"); }
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

                // Проверяем, есть ли уже позиция по этому символу.
                // Если да — НЕ добавляем (balance sync не должен дублировать позицию).
                auto existing_pos = portfolio_->get_position(symbol_);
                if (existing_pos.has_value()) {
                    // Позиция уже отслеживается — пропускаем
                    logger_->debug("pipeline", "Позиция уже отслеживается, balance sync пропущен",
                        {{"coin", base_coin},
                         {"tracked_size", std::to_string(existing_pos->size.get())},
                         {"exchange_size", std::to_string(base_available)}});
                } else {
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
                }
            } else {
                logger_->info("pipeline", "Пылевой баланс проигнорирован",
                    {{"coin", base_coin},
                     {"size", std::to_string(base_available)},
                     {"notional", std::to_string(estimated_notional)}});
            }
        }

        // Фьючерсы: синхронизация открытых позиций с биржей
        if (config_.futures.enabled && futures_query_adapter_) {
            auto existing_pos = portfolio_->get_position(symbol_);
            auto fp_result = futures_query_adapter_->get_positions(symbol_);
            if (fp_result) {
                double exchange_qty = 0.0;
                PositionSide exchange_side = PositionSide::Long;
                double exchange_entry = 0.0;
                for (const auto& fp : *fp_result) {
                    if (fp.total.get() > 0.0) {
                        exchange_qty = fp.total.get();
                        exchange_side = fp.position_side;
                        exchange_entry = fp.entry_price.get();
                        break;
                    }
                }

                if (exchange_qty > 0.0 && !existing_pos.has_value()) {
                    // Биржа имеет позицию, но портфель — нет. Восстанавливаем.
                    portfolio::Position sync_pos;
                    sync_pos.symbol = symbol_;
                    sync_pos.side = (exchange_side == PositionSide::Long) ? Side::Buy : Side::Sell;
                    sync_pos.size = Quantity(exchange_qty);
                    sync_pos.avg_entry_price = Price(exchange_entry);
                    sync_pos.opened_at = clock_->now();
                    sync_pos.strategy_id = StrategyId("sync_from_exchange");
                    portfolio_->open_position(sync_pos);
                    current_position_side_ = exchange_side;
                    // Инициализация trailing state для recovered позиции
                    reset_trailing_state();
                    highest_price_since_entry_ = exchange_entry;
                    lowest_price_since_entry_ = exchange_entry;
                    position_entry_time_ns_ = clock_->now().get();
                    initial_position_size_ = exchange_qty;

                    logger_->info("pipeline", "Восстановлена фьючерсная позиция с биржи",
                        {{"symbol", symbol_.get()},
                         {"side", exchange_side == PositionSide::Long ? "long" : "short"},
                         {"size", std::to_string(exchange_qty)},
                         {"entry", std::to_string(exchange_entry)}});
                } else if (exchange_qty <= 0.0 && existing_pos.has_value()) {
                    // Портфель имеет позицию, но биржа — нет. Фантом: очищаем.
                    logger_->warn("pipeline", "Фантомная позиция обнаружена на старте — очистка",
                        {{"symbol", symbol_.get()},
                         {"portfolio_size", std::to_string(existing_pos->size.get())}});
                    portfolio_->close_position(symbol_, Price(0.0), 0.0);
                }
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

    std::string endpoint = config_.futures.enabled
        ? "/api/v2/mix/account/accounts?productType=USDT-FUTURES"
        : "/api/v2/spot/account/assets";
    auto resp = rest_client_->get(endpoint);
    if (!resp.success) return 0.0;

    try {
        auto doc = boost::json::parse(resp.body);
        auto& obj = doc.as_object();
        if (obj.at("code").as_string() != "00000") return 0.0;

        auto& data = obj.at("data").as_array();
        for (const auto& item : data) {
            auto& asset = item.as_object();
            // Futures: "marginCoin", Spot: "coin"
            std::string asset_coin;
            if (asset.contains("marginCoin")) {
                asset_coin = std::string(asset.at("marginCoin").as_string());
            } else if (asset.contains("coin")) {
                asset_coin = std::string(asset.at("coin").as_string());
            } else {
                continue;
            }
            if (asset_coin == coin) {
                return std::stod(std::string(asset.at("available").as_string()));
            }
        }
    } catch (const std::exception& e) {
        logger_->warn("pipeline", "Ошибка при запросе баланса актива",
            {{"coin", coin}, {"error", e.what()}});
    } catch (...) {
        logger_->warn("pipeline", "Неизвестная ошибка при запросе баланса актива",
            {{"coin", coin}});
    }

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
    std::string path = config_.futures.enabled
        ? "/api/v2/mix/market/candles"
        : "/api/v2/spot/market/candles";

    // --- Шаг 1: 200 минутных свечей для прогрева индикаторов ---
    logger_->info("pipeline", "Загрузка 200 минутных свечей...");
    std::string granularity_1m = config_.futures.enabled ? "1m" : "1min";
    std::string query_1m = "symbol=" + symbol_.get()
        + "&granularity=" + granularity_1m + "&limit=200";
    if (config_.futures.enabled) {
        query_1m += "&productType=" + config_.futures.product_type;
    }
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

                // Bitget API v2 отдаёт свечи в хронологическом порядке (oldest→newest)
                // Порядок уже правильный для feature_engine

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
    std::string path = config_.futures.enabled
        ? "/api/v2/mix/market/candles"
        : "/api/v2/spot/market/candles";
    std::string granularity_htf = config_.futures.enabled ? "1H" : "1h";
    std::string query = "symbol=" + symbol_.get()
        + "&granularity=" + granularity_htf + "&limit=200";
    if (config_.futures.enabled) {
        query += "&productType=" + config_.futures.product_type;
    }

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
        // Bitget API v2 отдаёт свечи в хронологическом порядке (oldest→newest)
        std::vector<double> closes;
        std::vector<double> highs;
        std::vector<double> lows;
        closes.reserve(data.size());
        highs.reserve(data.size());
        lows.reserve(data.size());

        for (const auto& item : data) {
            const auto& arr = item.as_array();
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
    // Runtime-переоцениваемый gate: проверяет условия на КАЖДОМ тике.
    // Если условия ухудшились после получения readiness — market_ready_ сбрасывается.
    // Стоп-лосс и защита позиции работают НЕЗАВИСИМО от market_ready_.

    bool was_ready = market_ready_;

    // 1. HTF должен быть загружен
    if (!htf_valid_) {
        if (market_ready_) {
            logger_->warn("pipeline", "Market readiness отозвана: HTF данные невалидны");
        }
        market_ready_ = false;
        if (tick_count_ % 500 == 0) {
            logger_->info("pipeline", "Ожидание HTF-данных перед началом торговли...");
        }
        return false;
    }

    // 2. Индикаторы рабочего таймфрейма должны быть готовы
    if (!snapshot.technical.sma_valid || !snapshot.technical.rsi_valid
        || !snapshot.technical.adx_valid || !snapshot.technical.macd_valid) {
        if (market_ready_) {
            logger_->warn("pipeline", "Market readiness отозвана: индикаторы невалидны");
        }
        market_ready_ = false;
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
        if (market_ready_) {
            logger_->warn("pipeline",
                "Market readiness отозвана: HTF RSI в экстремальной зоне",
                {{"htf_rsi", std::to_string(htf_rsi_14_)}});
        }
        market_ready_ = false;
        if (tick_count_ % 1000 == 0) {
            logger_->warn("pipeline",
                "HTF RSI в экстремальной зоне — ожидание нормализации",
                {{"htf_rsi", std::to_string(htf_rsi_14_)}});
        }
        return false;
    }

    // 4. Не входить при сильном даунтренде на HTF (сила > 0.6)
    if (htf_trend_direction_ == -1 && htf_trend_strength_ > 0.6) {
        bool macd_reversal = htf_macd_histogram_ > 0.0;
        bool rsi_recovery = htf_rsi_14_ > 35.0;

        if (!macd_reversal && !rsi_recovery) {
            if (market_ready_) {
                logger_->warn("pipeline",
                    "Market readiness отозвана: сильный даунтренд на HTF");
            }
            market_ready_ = false;
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
    if (!was_ready) {
        market_ready_since_tick_ = tick_count_;
        logger_->info("pipeline", "=== РЫНОК ГОТОВ К ТОРГОВЛЕ ===",
            {{"tick", std::to_string(tick_count_)},
             {"htf_trend", htf_trend_direction_ == 1 ? "UP" :
                           htf_trend_direction_ == -1 ? "DOWN" : "SIDEWAYS"},
             {"htf_strength", std::to_string(htf_trend_strength_)},
             {"htf_rsi", std::to_string(htf_rsi_14_)}});
    }
    market_ready_ = true;
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
    current_trail_mult_ = config_.trading_params.atr_stop_multiplier;
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
            logger_->info("pipeline", "Position entry time записано",
                {{"symbol", symbol_.get()},
                 {"size", std::to_string(initial_position_size_)},
                 {"max_hold_abs_min", std::to_string(config_.trading_params.max_hold_absolute_minutes)},
                 {"max_hold_loss_min", std::to_string(config_.trading_params.max_hold_loss_minutes)}});
        }

        // ATR должен быть валидным
        if (!snapshot.technical.atr_valid || snapshot.technical.atr_14 <= 0.0) continue;
        double atr = snapshot.technical.atr_14;

        // Dynamic ATR stop multiplier based on multiple market factors
        {
            double base = config_.trading_params.atr_stop_multiplier;

            // ADX factor: strong trend → tighter, range → wider
            double adx_factor = 1.0;
            if (snapshot.technical.adx_valid) {
                if (snapshot.technical.adx > 30.0) {
                    adx_factor = 0.85;
                } else if (snapshot.technical.adx > 20.0) {
                    adx_factor = 1.0;
                } else {
                    adx_factor = 1.15;
                }
            }

            // Order book depth factor: thin books need wider stops
            double depth_factor = 1.0;
            if (snapshot.microstructure.liquidity_valid) {
                double depth = snapshot.microstructure.bid_depth_5_notional
                             + snapshot.microstructure.ask_depth_5_notional;
                if (depth < 500.0) {
                    depth_factor = 1.3;
                } else if (depth < 2000.0) {
                    depth_factor = 1.1;
                }
            }

            // Spread factor: wide spreads need wider stops
            double spread_factor = 1.0;
            if (snapshot.microstructure.spread_valid) {
                if (snapshot.microstructure.spread_bps > 50.0) {
                    spread_factor = 1.3;
                } else if (snapshot.microstructure.spread_bps > 20.0) {
                    spread_factor = 1.1;
                }
            }

            // Bollinger Band width factor: high volatility → wider stop
            double bb_factor = 1.0;
            if (snapshot.technical.bb_valid) {
                if (snapshot.technical.bb_bandwidth > 0.05) {
                    bb_factor = 1.2;
                } else if (snapshot.technical.bb_bandwidth > 0.03) {
                    bb_factor = 1.1;
                }
            }

            // Buy/sell pressure factor: heavy sell pressure on longs → tighter stop
            double pressure_factor = 1.0;
            if (snapshot.microstructure.trade_flow_valid &&
                pos.side == Side::Buy &&
                snapshot.microstructure.buy_sell_ratio < 0.3) {
                pressure_factor = 0.8;
            }

            current_trail_mult_ = base * adx_factor * depth_factor
                                * spread_factor * bb_factor * pressure_factor;
            current_trail_mult_ = std::clamp(current_trail_mult_, 0.8, 3.0);

            if (tick_count_ % 500 == 0) {
                logger_->debug("pipeline", "Dynamic stop multiplier computed",
                    {{"base", std::to_string(base)},
                     {"adx_f", std::to_string(adx_factor)},
                     {"depth_f", std::to_string(depth_factor)},
                     {"spread_f", std::to_string(spread_factor)},
                     {"bb_f", std::to_string(bb_factor)},
                     {"pressure_f", std::to_string(pressure_factor)},
                     {"result", std::to_string(current_trail_mult_)},
                     {"symbol", symbol_.get()}});
            }
        }

        if (pos.side == Side::Buy) {
            highest_price_since_entry_ = std::max(highest_price_since_entry_, price);
            // Fix: обновляем MAE для лонга на каждом тике
            update_current_mae(price, /*is_long=*/true);
            double profit_in_atr = (price - entry) / atr;

            // Фаза 1: До breakeven — используем только ФИКСИРОВАННЫЙ стоп (safety net).
            // Trailing stop НЕ активен — даём цене свободно двигаться.
            if (!breakeven_activated_) {
                if (profit_in_atr >= config_.trading_params.breakeven_atr_threshold) {
                    // Достигли breakeven уровня → активируем trailing
                    breakeven_activated_ = true;
                    // Лочим 50% текущего профита выше entry
                    double current_profit = price - entry;
                    double breakeven_level = entry + current_profit * 0.5;
                    // Fee-aware floor: стоп минимум покрывает round-trip комиссии
                    double min_fee_offset = entry * common::fees::kDefaultTakerFeePct * 3.0;
                    breakeven_level = std::max(breakeven_level, entry + min_fee_offset);
                    // Гарантируем: стоп минимум на 1 тик выше entry
                    breakeven_level = std::max(breakeven_level, entry + 0.001);
                    current_stop_level_ = breakeven_level;
                    logger_->info("pipeline", "Breakeven + Trailing активирован",
                        {{"entry", std::to_string(entry)},
                         {"stop", std::to_string(breakeven_level)},
                         {"locked_pct", std::to_string((breakeven_level - entry) / entry * 100.0)},
                         {"profit_atr", std::to_string(profit_in_atr)},
                         {"fee_floor", std::to_string(min_fee_offset)},
                         {"symbol", symbol_.get()}});
                } else if (current_stop_level_ <= 0.0) {
                    // Начальный ATR-стоп: защита до активации breakeven
                    current_stop_level_ = entry - current_trail_mult_ * atr;
                }
            } else {
                // Фаза 2: После breakeven — классический Chandelier Exit
                double new_stop = highest_price_since_entry_ - current_trail_mult_ * atr;
                // Стоп только поднимается
                current_stop_level_ = std::max(current_stop_level_, new_stop);
            }
        } else {
            lowest_price_since_entry_ = std::min(lowest_price_since_entry_, price);
            // Fix: обновляем MAE для шорта на каждом тике
            update_current_mae(price, /*is_long=*/false);
            double profit_in_atr = (entry - price) / atr;

            if (!breakeven_activated_) {
                if (profit_in_atr >= config_.trading_params.breakeven_atr_threshold) {
                    breakeven_activated_ = true;
                    double current_profit = entry - price;
                    double breakeven_level = entry - current_profit * 0.5;
                    // Fee-aware floor: стоп минимум покрывает round-trip комиссии
                    double min_fee_offset = entry * common::fees::kDefaultTakerFeePct * 3.0;
                    breakeven_level = std::min(breakeven_level, entry - min_fee_offset);
                    breakeven_level = std::min(breakeven_level, entry - 0.001);
                    current_stop_level_ = breakeven_level;
                    logger_->info("pipeline", "Breakeven + Trailing активирован (SELL)",
                        {{"entry", std::to_string(entry)},
                         {"stop", std::to_string(breakeven_level)},
                         {"locked_pct", std::to_string((entry - breakeven_level) / entry * 100.0)},
                         {"fee_floor", std::to_string(min_fee_offset)},
                         {"symbol", symbol_.get()}});
                } else if (current_stop_level_ <= 0.0) {
                    // Начальный ATR-стоп: защита до активации breakeven
                    current_stop_level_ = entry + current_trail_mult_ * atr;
                }
            } else {
                double new_stop = lowest_price_since_entry_ + current_trail_mult_ * atr;
                if (current_stop_level_ <= 0.0 || current_stop_level_ > new_stop * 2.0) {
                    current_stop_level_ = new_stop;
                } else {
                    current_stop_level_ = std::min(current_stop_level_, new_stop);
                }
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
            if (profit_in_atr >= config_.trading_params.partial_tp_atr_threshold) {
                partial_tp_triggered = true;
            }
        }

        // === 3. Фиксированный стоп (safety net): убыток > X% капитала ===
        double loss_pct_of_capital = 0.0;
        if (port_snap.total_capital > 0.0) {
            loss_pct_of_capital = std::abs(std::min(pos.unrealized_pnl, 0.0))
                                / port_snap.total_capital * 100.0;
        }
        bool fixed_stop_triggered = loss_pct_of_capital >= config_.trading_params.max_loss_per_trade_pct;

        // === 3b. Per-trade price stop: ценовой убыток превысил порог от цены входа ===
        // Для long: цена упала ниже entry. Для short: цена выросла выше entry.
        bool price_stop_triggered = false;
        if (entry > 0.0) {
            double price_loss_pct = 0.0;
            bool is_long_position = (pos.side == Side::Buy);
            // На фьючерсах определяем по сохранённой стороне позиции
            if (config_.futures.enabled) {
                is_long_position = (current_position_side_ == PositionSide::Long);
            }
            if (is_long_position) {
                price_loss_pct = (entry - price) / entry * 100.0;
            } else {
                price_loss_pct = (price - entry) / entry * 100.0;
            }
            if (price_loss_pct >= config_.trading_params.price_stop_loss_pct) {
                price_stop_triggered = true;
                logger_->warn("pipeline", "PRICE STOP: убыток превысил порог от цены входа",
                    {{"price_loss_pct", std::to_string(price_loss_pct)},
                     {"threshold_pct", std::to_string(config_.trading_params.price_stop_loss_pct)},
                     {"entry", std::to_string(entry)},
                     {"current", std::to_string(price)},
                     {"position_side", is_long_position ? "Long" : "Short"},
                     {"symbol", symbol_.get()}});
            }
        }

        // === 4. Time-based exit: закрытие по таймауту ===
        bool time_exit_triggered = false;
        if (position_entry_time_ns_ > 0) {
            int64_t now_check = clock_->now().get();
            int64_t hold_duration = now_check - position_entry_time_ns_;
            int64_t max_hold_loss = static_cast<int64_t>(config_.trading_params.max_hold_loss_minutes)
                                 * 60LL * 1'000'000'000LL;
            int64_t max_hold_abs = static_cast<int64_t>(config_.trading_params.max_hold_absolute_minutes)
                                 * 60LL * 1'000'000'000LL;
            int64_t hold_min = hold_duration / 60'000'000'000LL;

            // Smart extension: if momentum favors position, delay TIME_EXIT
            bool momentum_favorable = false;
            if (snapshot.technical.momentum_valid) {
                bool is_long = (pos.side == Side::Buy);
                if (config_.futures.enabled) {
                    is_long = (current_position_side_ == PositionSide::Long);
                }
                momentum_favorable = is_long
                    ? (snapshot.technical.momentum_5 > 0.001)
                    : (snapshot.technical.momentum_5 < -0.001);
            }

            // Убыточная позиция > N мин → закрыть (only if loss > 0.3% of entry)
            double loss_pct_of_entry = (entry > 0.0 && pos.size.get() > 0.0)
                ? std::abs(pos.unrealized_pnl) / (entry * pos.size.get()) * 100.0
                : 0.0;
            if (pos.unrealized_pnl < 0.0 && loss_pct_of_entry > 0.3 && hold_duration >= max_hold_loss) {
                // If momentum is favorable — extend by another 50% of max_hold_loss
                if (momentum_favorable && hold_duration < max_hold_loss * 3 / 2) {
                    if (tick_count_ % 200 == 0) {
                        logger_->info("pipeline", "TIME_EXIT отложен — momentum благоприятный",
                            {{"hold_min", std::to_string(hold_min)},
                             {"momentum_5", std::to_string(snapshot.technical.momentum_5)},
                             {"pnl", std::to_string(pos.unrealized_pnl)},
                             {"symbol", symbol_.get()}});
                    }
                } else {
                    time_exit_triggered = true;
                    logger_->warn("pipeline", "TIME_EXIT: убыточная позиция по таймауту",
                        {{"hold_min", std::to_string(hold_min)},
                         {"max_hold_loss_min", std::to_string(config_.trading_params.max_hold_loss_minutes)},
                         {"pnl", std::to_string(pos.unrealized_pnl)},
                         {"loss_pct", std::to_string(loss_pct_of_entry)},
                         {"symbol", symbol_.get()}});
                }
            }
            // Абсолютный таймаут > M мин → закрыть (extend if profitable + momentum ok)
            if (!time_exit_triggered && hold_duration >= max_hold_abs) {
                if (pos.unrealized_pnl > 0.0 && momentum_favorable && hold_duration < max_hold_abs * 3 / 2) {
                    if (tick_count_ % 200 == 0) {
                        logger_->info("pipeline", "TIME_EXIT абсолютный отложен — прибыльная позиция + momentum",
                            {{"hold_min", std::to_string(hold_min)},
                             {"momentum_5", std::to_string(snapshot.technical.momentum_5)},
                             {"pnl", std::to_string(pos.unrealized_pnl)},
                             {"symbol", symbol_.get()}});
                    }
                } else {
                    time_exit_triggered = true;
                    logger_->warn("pipeline", "TIME_EXIT: абсолютный таймаут позиции",
                        {{"hold_min", std::to_string(hold_min)},
                         {"max_hold_abs_min", std::to_string(config_.trading_params.max_hold_absolute_minutes)},
                         {"pnl", std::to_string(pos.unrealized_pnl)},
                         {"symbol", symbol_.get()}});
                }
            }
        }

        if (!trailing_stop_triggered && !partial_tp_triggered && !fixed_stop_triggered && !price_stop_triggered && !time_exit_triggered) continue;

        // === Стоп/TP сработал: проверяем возможность закрытия ===

        // Cooldown стоп-лосса (отдельный от обычных ордеров — стоп экстренный)
        int64_t now_ns = clock_->now().get();
        if (last_stop_loss_time_ns_ > 0 &&
            (now_ns - last_stop_loss_time_ns_) < kStopLossCooldownNs) {
            // Уже пытались недавно — не спамим биржу, но сигнализируем pipeline
            // что стоп-лосс активен (блокировать стратегии)
            return true;
        }

        // Определяем реальный размер позиции для закрытия
        double actual_qty = 0.0;
        if (config_.futures.enabled && futures_query_adapter_) {
            // Фьючерсы: запрашиваем позицию напрямую (не баланс base coin)
            auto fp_list = futures_query_adapter_->get_positions(symbol_);
            bool exchange_query_ok = fp_list.has_value();
            if (fp_list) {
                for (const auto& fp : *fp_list) {
                    if (fp.position_side == current_position_side_ && fp.total.get() > 0.0) {
                        actual_qty = fp.total.get();
                        break;
                    }
                }
            }
            if (actual_qty <= 0.0 && exchange_query_ok) {
                // Биржа подтвердила: позиции нет. Очищаем фантомную позицию из портфеля.
                logger_->warn("pipeline", "Позиция не найдена на бирже — очистка фантомной позиции",
                    {{"symbol", symbol_.get()},
                     {"portfolio_size", std::to_string(pos.size.get())}});
                portfolio_->close_position(symbol_, pos.current_price, 0.0);
                reset_trailing_state();
                return true;
            }
            if (actual_qty <= 0.0) actual_qty = pos.size.get(); // fallback (query failed)
        } else {
            // Спот: запрашиваем реальный баланс base coin с биржи
            actual_qty = query_asset_balance(extract_base_coin(symbol_.get()));
            if (actual_qty <= 0.0) actual_qty = pos.size.get(); // fallback
        }
        if (actual_qty < 0.00001) {
            // Токен уже продан (стоп-лосс сработал ранее), но позиция
            // осталась в портфеле. Очищаем её принудительно.
            logger_->info("pipeline", "Стоп-лосс: актив уже продан, очищаем позицию из портфеля",
                {{"qty", std::to_string(actual_qty)},
                 {"symbol", symbol_.get()}});
            portfolio_->close_position(symbol_, pos.current_price, pos.unrealized_pnl);
            reset_trailing_state();
            return true;
        }
        // Пылевая позиция — невозможно продать на бирже, пропускаем
        double actual_notional = actual_qty * price;
        if (actual_notional < common::exchange_limits::kDustNotionalUsdt) {
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
            // Partial TP: закрываем сконфигурированную долю позиции
            close_qty = actual_qty * config_.trading_params.partial_tp_fraction;
            // Минимальный ордер Bitget
            double min_qty_notional = common::exchange_limits::kMinBitgetNotionalUsdt / price;
            if (close_qty < min_qty_notional) close_qty = actual_qty;
            is_full_close = false;
            partial_tp_taken_ = true;
            reason = "PARTIAL_TP: профит >= " + std::to_string(config_.trading_params.partial_tp_atr_threshold)
                   + "×ATR, закрытие " + std::to_string(static_cast<int>(config_.trading_params.partial_tp_fraction * 100)) + "%";

            logger_->info("pipeline", "PARTIAL TAKE-PROFIT — закрытие 50% позиции",
                {{"symbol", symbol_.get()},
                 {"entry", std::to_string(entry)},
                 {"current", std::to_string(price)},
                 {"close_qty", std::to_string(close_qty)},
                 {"remaining", std::to_string(actual_qty - close_qty)}});
        } else if (trailing_stop_triggered) {
            reason = "TRAILING_STOP: цена " + std::to_string(price)
                   + " прошла стоп " + std::to_string(current_stop_level_);
        } else if (price_stop_triggered) {
            double price_loss_pct = (entry - price) / entry * 100.0;
            reason = "PRICE_STOP: убыток " + std::to_string(price_loss_pct) + "% от цены входа";
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
        close_intent.snapshot_mid_price = snapshot.mid_price;
        close_intent.suggested_quantity = Quantity(close_qty);

        // Фьючерсы: устанавливаем trade_side=Close и position_side для корректного ордера.
        // Без этого Bitget Mix API получит tradeSide=open и откроет встречную позицию
        // вместо закрытия текущей.
        if (config_.futures.enabled) {
            close_intent.trade_side = TradeSide::Close;
            close_intent.position_side = current_position_side_;
            close_intent.signal_intent = (current_position_side_ == PositionSide::Long)
                ? strategy::SignalIntent::LongExit : strategy::SignalIntent::ShortExit;
            // Для short-позиции side=Buy (закрытие short = покупка на фьючерсах)
            if (current_position_side_ == PositionSide::Short) {
                close_intent.side = Side::Buy;
            }
        }

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

        auto order_result = execution_engine_->execute(close_intent, risk_decision, exec_alpha,
            uncertainty::UncertaintySnapshot{});  // Стоп-лосс обходит неопределённость
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
                // Записываем данные для fingerprint/thompson, но НЕ вызываем
                // portfolio_->close_position() повторно — иначе PnL будет двойным.

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

    // Фьючерсы: установка margin mode и hold mode на бирже ПЕРЕД первым ордером.
    // Без этого Bitget может использовать one-way mode, и ордера
    // с tradeSide/holdSide будут отклонены.
    if (futures_submitter_) {
        // 1. Position mode = hedge_mode (long + short одновременно)
        bool hold_ok = futures_submitter_->set_hold_mode(
            config_.futures.product_type, "hedge_mode");
        if (!hold_ok) {
            logger_->warn("pipeline",
                "Не удалось установить position mode (hedge_mode). "
                "Фьючерсные ордера могут быть отклонены биржей.");
        }

        // 2. Margin mode (isolated/crossed) для текущего символа
        bool margin_ok = futures_submitter_->set_margin_mode(
            symbol_, config_.futures.margin_mode);
        if (!margin_ok) {
            logger_->warn("pipeline",
                "Не удалось установить margin mode. "
                "Используется текущий режим маржи на бирже.",
                {{"requested_mode", config_.futures.margin_mode},
                 {"symbol", symbol_.get()}});
        }

        // 3. Установка default leverage для обеих сторон
        futures_submitter_->set_leverage(symbol_, config_.futures.default_leverage, "long");
        futures_submitter_->set_leverage(symbol_, config_.futures.default_leverage, "short");

        logger_->info("pipeline", "Фьючерсный режим настроен",
            {{"hold_mode", "hedge_mode"},
             {"margin_mode", config_.futures.margin_mode},
             {"default_leverage", std::to_string(config_.futures.default_leverage)},
             {"symbol", symbol_.get()}});
    }

    // Загрузка исторических свечей для прогрева индикаторов.
    // КРИТИЧНО: без этого SMA/RSI/ADX/EMA не имеют данных,
    // и scalp_engine торгует вслепую.
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
    // Проверяем размер позиции: на спот size > 0 для long,
    // на фьючерсах size > 0 для любого направления (long или short)
    if (pos->size.get() > 0.0) return true;
    // Для фьючерсов: дополнительно проверяем позицию на бирже,
    // т.к. portfolio может не отслеживать short корректно
    if (config_.futures.enabled && futures_query_adapter_) {
        auto positions = futures_query_adapter_->get_positions(symbol_);
        if (positions) {
            for (const auto& fp : *positions) {
                if (fp.total.get() > 0.0) return true;
            }
        }
    }
    return false;
}

bool TradingPipeline::is_idle(int64_t threshold_ns) const {
    int64_t last = last_activity_ns_.load(std::memory_order_relaxed);
    if (last == 0) return false; // ещё не было ни одного тика
    int64_t now = clock_->now().get();
    return (now - last) > threshold_ns;
}

// ==================== Основной торговый цикл ====================

void TradingPipeline::on_feature_snapshot(features::FeatureSnapshot snapshot) {
    if (!running_) return;
    std::lock_guard<std::mutex> lock(pipeline_mutex_);

    ++tick_count_;

    int64_t now_ns = clock_->now().get();

    // ─── Phase 1: Freshness Gate ──────────────────────────────────────────
    // Отклоняем устаревшие котировки ДО любой обработки.
    // Порог: 5 секунд. Stale ticks могут вызвать ложные сигналы и bad fills.
    if (snapshot.computed_at.get() > 0) {
        int64_t age_ns = now_ns - snapshot.computed_at.get();
        constexpr int64_t kMaxFreshnessNs = 5'000'000'000LL;
        if (age_ns > kMaxFreshnessNs) {
            if (tick_count_ % 50 == 0) {
                logger_->warn("pipeline", "Stale tick отклонён",
                    {{"age_ms", std::to_string(age_ns / 1'000'000)},
                     {"max_ms", "5000"},
                     {"tick", std::to_string(tick_count_)}});
            }
            if (metrics_) {
                metrics_->counter("pipeline_stale_ticks_total", {{"symbol", symbol_.get()}})->increment();
            }
            return;
        }
    }

    // ─── Phase 1: Backlog Detection ───────────────────────────────────────
    // Если предыдущая обработка тика была слишком долгой (pipeline не успевает),
    // логируем предупреждение. Детектируем по gap между тиками.
    {
        if (last_tick_ingress_ns_ > 0) {
            int64_t gap_ns = now_ns - last_tick_ingress_ns_;
            constexpr int64_t kMaxTickGapNs = 2'000'000'000LL;  // 2 секунды
            if (gap_ns > kMaxTickGapNs && tick_count_ > kMinWarmupTicks) {
                logger_->warn("pipeline", "Tick gap обнаружен — возможен backlog",
                    {{"gap_ms", std::to_string(gap_ns / 1'000'000)},
                     {"tick", std::to_string(tick_count_)}});
                if (metrics_) {
                    metrics_->counter("pipeline_tick_gaps_total", {{"symbol", symbol_.get()}})->increment();
                }
            }
        }
        last_tick_ingress_ns_ = now_ns;
    }

    // ─── Phase 2/4: Periodic Background Tasks ────────────────────────────
    run_periodic_tasks(now_ns);

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
        const auto ent = entropy_filter_->compute();
        ml_snapshot_.computed_at_ns = clock_->now().get();
        ml_snapshot_.composite_entropy = ent.composite_entropy;
        ml_snapshot_.signal_quality = ent.signal_quality;
        ml_snapshot_.is_noisy = ent.is_noisy;
        ml_snapshot_.entropy_status = ent.component_status;
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
        const auto c = cascade_detector_->evaluate();
        ml_snapshot_.cascade_probability = c.probability;
        ml_snapshot_.cascade_imminent = c.cascade_imminent;
        ml_snapshot_.cascade_direction = c.direction;
        ml_snapshot_.cascade_status = c.component_status;
    }

    // 0.ml3. Монитор корреляций: обновляем цену основного актива
    if (correlation_monitor_ && snapshot.mid_price.get() > 0.0) {
        correlation_monitor_->on_primary_tick(snapshot.mid_price.get());
        const auto corr = correlation_monitor_->evaluate();
        ml_snapshot_.avg_correlation = corr.avg_correlation;
        ml_snapshot_.correlation_break = corr.any_break;
        ml_snapshot_.correlation_risk_multiplier = corr.risk_multiplier;
        ml_snapshot_.correlation_status = corr.component_status;
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

    // 0c.gov. — removed (governance module deleted)

    // 0d. Периодическое обновление HTF индикаторов (каждый час или экстренно)
    maybe_update_htf(snapshot);

    // 0g. Фильтр энтропии: блокируем торговлю при зашумлённом рынке
    if (entropy_filter_ && entropy_filter_->is_noisy()) {
        if (tick_count_ % 200 == 0) {
            auto ent = entropy_filter_->compute();
            logger_->info("pipeline",
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
    bool thompson_bypass = false;  // Если pending entry активирован — пропустить Thompson ниже
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
                pe.intent.snapshot_mid_price = snapshot.mid_price;
            }
            // Сбрасываем pending и помечаем bypass, чтобы Thompson ниже
            // НЕ создал новый pending (иначе бесконечный цикл Wait1)
            pending_entry_.reset();
            thompson_bypass = true;
        } else {
            // Ещё ждём — пропускаем тик
            return;
        }
    }

    // 0f. TWAP: проверяем есть ли активный TWAP ордер для исполнения следующего слайса
    if (twap_executor_ && twap_executor_->has_active_twap()) {
        auto twap = twap_executor_->get_active_twap();
        if (twap && !twap_executor_->is_complete(*twap)) {
            auto now_ms = clock_->now().get() / 1'000'000;  // нс → мс
            auto slice_intent = twap_executor_->get_next_slice(*twap, snapshot, now_ms);
            if (slice_intent) {
                // TWAP-слайсы проходят через risk engine для критических проверок
                // (kill switch, daily loss, max drawdown), но размер уже зафиксирован
                uncertainty::UncertaintySnapshot twap_uncertainty{};
                auto exec_alpha = execution_alpha_->evaluate(*slice_intent, snapshot, twap_uncertainty);

                // Прогоняем через risk engine вместо ручного Approved
                auto port_snap = portfolio_->snapshot();

                // Формируем SizingResult из TWAP-слайса (размер уже определён)
                portfolio_allocator::SizingResult twap_sizing;
                twap_sizing.approved_quantity = slice_intent->suggested_quantity;
                twap_sizing.approved_notional = NotionalValue(
                    slice_intent->suggested_quantity.get() * snapshot.mid_price.get());

                auto slice_risk = risk_engine_->evaluate(
                    *slice_intent, twap_sizing, port_snap, snapshot, exec_alpha, twap_uncertainty);

                if (slice_risk.verdict == risk::RiskVerdict::Denied) {
                    logger_->warn("pipeline", "TWAP slice заблокирован risk engine",
                        {{"reason", slice_risk.summary},
                         {"slice", std::to_string(twap->next_slice)}});
                    // Abort TWAP при критическом блоке (kill switch, daily loss)
                    twap_executor_->clear_active_twap();
                    return;
                }

                // Risk может уменьшить размер — используем approved_quantity
                slice_intent->suggested_quantity = slice_risk.approved_quantity;

                auto result = execution_engine_->execute(*slice_intent, slice_risk, exec_alpha, twap_uncertainty);
                if (result) {
                    risk_engine_->record_order_sent();
                    // Записываем fill только после успешного исполнения
                    twap_executor_->record_slice_fill(*twap, twap->next_slice - 1,
                        slice_risk.approved_quantity,
                        slice_intent->limit_price ? *slice_intent->limit_price : snapshot.mid_price);
                } else {
                    logger_->warn("pipeline", "TWAP slice execution failed",
                        {{"slice", std::to_string(twap->next_slice)},
                         {"error", "execute returned error"}});
                }
            }
            // Записываем изменённый TWAP обратно
            twap_executor_->set_active_twap(*twap);
            return;  // Не обрабатываем новые сигналы, пока TWAP активен
        } else {
            // TWAP завершён — сбрасываем
            twap_executor_->clear_active_twap();
        }
    }

    // 1. Мировая модель
    auto world = world_model_->update(snapshot);

    // 2. Режим рынка
    auto regime = regime_engine_->classify(snapshot);
    risk_engine_->set_current_regime(regime.detailed);

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
    // На СПОТ: без позиции нужны только BUY сигналы, с позицией — только SELL.
    // На ФЬЮЧЕРСАХ: допускаются любые сигналы (LongEntry, LongExit, ShortEntry, ShortExit)
    // Фильтрация ДО стратегий экономит CPU и избавляет от лог-спама (только для спота).
    bool has_long_position = false;
    bool has_short_position = false;
    {
        auto port_snap = portfolio_->snapshot();
        for (const auto& pos : port_snap.positions) {
            if (pos.symbol.get() == symbol_.get()) {
                if (pos.side == Side::Buy && pos.size.get() >= 0.00001) {
                    has_long_position = true;
                }
                if (pos.side == Side::Sell && pos.size.get() >= 0.00001) {
                    has_short_position = true;
                }
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
        ctx.futures_enabled = config_.futures.enabled;

        // Strategy Engine: передаём позицию и системное состояние
        ctx.position.has_position = has_long_position || has_short_position;
        if (ctx.position.has_position) {
            auto port_snap = portfolio_->snapshot();
            for (const auto& pos : port_snap.positions) {
                if (pos.symbol.get() == symbol_.get()) {
                    ctx.position.side = pos.side;
                    ctx.position.position_side = (pos.side == Side::Buy) ? PositionSide::Long : PositionSide::Short;
                    ctx.position.size = pos.size.get();
                    ctx.position.avg_entry_price = pos.avg_entry_price.get();
                    ctx.position.unrealized_pnl = pos.unrealized_pnl;
                    ctx.position.entry_time_ns = position_entry_time_ns_;
                    break;
                }
            }
        }
        ctx.data_fresh = snapshot.execution_context.is_feed_fresh;
        ctx.exchange_ok = true; // при отключении WebSocket pipeline не вызывается

        auto intent = strat->evaluate(ctx);
        if (intent.has_value()) {
            ++total_intents;

            // ФИЛЬТРАЦИЯ СИГНАЛОВ: различается для спота и фьючерсов
            if (!config_.futures.enabled) {
                // СПОТ режим (только long позиции):
                // - BUY при открытой long-позиции — нарастить нельзя
                // - SELL без открытой long-позиции бесполезен
                bool is_buy = (intent->side == Side::Buy);
                if (is_buy && has_long_position) {
                    ++filtered_intents;
                    continue;
                }
                if (!is_buy && !has_long_position) {
                    ++filtered_intents;
                    continue;
                }
            }
            // ФЬЮЧЕРСЫ: допускаем все сигналы - risk_engine проверит их валидность

            // Генерируем correlation_id если стратегия не установила его
            if (intent->correlation_id.get().empty()) {
                intent->correlation_id = CorrelationId(
                    intent->strategy_id.get() + "-" + std::to_string(tick_count_));
            }

            intents.push_back(std::move(*intent));
        }
    }

    // Логируем состояние сигналов каждые 200 тиков для диагностики
    if (tick_count_ % 200 == 0) {
        logger_->info("pipeline", "Итоги оценки стратегий",
            {{"total_intents", std::to_string(total_intents)},
             {"filtered", std::to_string(filtered_intents)},
             {"actionable", std::to_string(intents.size())},
             {"has_position", (has_long_position || has_short_position) ? "true" : "false"},
             {"tick", std::to_string(tick_count_)}});
    }

    if (intents.empty()) return;

    // 8. Агрегация решений комитетом (с portfolio/features context)
    auto decision = decision_engine_->aggregate(
        symbol_, intents, allocation, regime, world, uncertainty,
        portfolio_->snapshot(), snapshot);

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
        ml_snapshot_.fingerprint_edge = fp_edge;
        ml_snapshot_.fingerprint_status = fingerprinter_->status();
        if (auto stats = fingerprinter_->get_stats(fp); stats.has_value()) {
            ml_snapshot_.fingerprint_win_rate = stats->win_rate;
            ml_snapshot_.fingerprint_sample_count = static_cast<int>(stats->count);
        }
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
        bayesian_conviction_adj = adapted_threshold - config_.decision.min_conviction_threshold;
        ml_snapshot_.adapted_conviction_threshold = adapted_threshold;
        ml_snapshot_.adapted_atr_stop_mult = bayesian_adapter_->get_adapted_value(
            "global", "atr_stop_multiplier", regime.detailed);
    }
    if (bayesian_adapter_) {
        ml_snapshot_.bayesian_status = bayesian_adapter_->status();
    }

    // 8b. Проверка порога conviction
    {
        double effective_threshold = config_.decision.min_conviction_threshold
                                   + bayesian_conviction_adj;

        // Абсолютный потолок — conviction шкала [0, 1]
        effective_threshold = std::min(effective_threshold, 0.90);

        // ═══════════════════════════════════════════════════════════
        // v11: УПРОЩЁННЫЙ фильтр — только критические блокировки
        // Стратегии уже имеют собственные RSI/EMA/BB фильтры.
        // Pipeline НЕ дублирует их, а блокирует только ЭКСТРЕМАЛЬНЫЕ ситуации.
        // ═══════════════════════════════════════════════════════════

        // Hard block: RSI > 92 BUY = экстремальный перегрев (крайне редко)
        if (intent.side == Side::Buy &&
            snapshot.technical.rsi_valid &&
            snapshot.technical.rsi_14 > 92.0) {
            if (tick_count_ % 200 == 0) {
                logger_->warn("pipeline", "RSI EXTREME: BUY заблокирован (RSI>92)",
                    {{"rsi_14", std::to_string(snapshot.technical.rsi_14)},
                     {"strategy", intent.strategy_id.get()},
                     {"symbol", symbol_.get()}});
            }
            return;
        }

        // Hard block: RSI < 8 SELL = экстремальная перепроданность
        if (intent.side == Side::Sell &&
            snapshot.technical.rsi_valid &&
            snapshot.technical.rsi_14 < 8.0) {
            if (tick_count_ % 200 == 0) {
                logger_->warn("pipeline", "RSI EXTREME: SELL заблокирован (RSI<8)",
                    {{"rsi_14", std::to_string(snapshot.technical.rsi_14)},
                     {"strategy", intent.strategy_id.get()},
                     {"symbol", symbol_.get()}});
            }
            return;
        }

        if (intent.conviction < effective_threshold) {
            if (tick_count_ % 200 == 0) {
                logger_->info("pipeline", "Conviction ниже порога",
                    {{"conviction", std::to_string(intent.conviction)},
                     {"threshold", std::to_string(effective_threshold)},
                     {"strategy", intent.strategy_id.get()}});
            }
            return;
        }
    }

    // 8b2. Risk:Reward pre-entry filter — пропускаем сделки с плохим R:R
    // Scientific: Katz & McCormick p.523 — minimum 1.5:1 R:R for positive EV
    if (snapshot.technical.atr_valid && snapshot.technical.atr_14 > 0.0 &&
        !has_open_position()) {
        double atr = snapshot.technical.atr_14;
        double price = snapshot.last_price.get();
        
        // Риск = расстояние до стопа (ATR-based, tighter for scientific edge)
        double risk_distance = atr * config_.trading_params.atr_stop_multiplier;
        
        // Потенциал прибыли: expected move based on regime
        double reward_distance = atr * config_.trading_params.partial_tp_atr_threshold;
        
        // Бонус к reward в сильном тренде (ADX > 35 = strong trend, +40% move potential)
        if (snapshot.technical.adx_valid && snapshot.technical.adx > 35.0) {
            reward_distance *= 1.4;
        }
        // Бонус если HTF тренд совпадает с направлением сделки (+30%)
        if ((intent.side == Side::Buy && htf_trend_direction_ == 1) ||
            (intent.side == Side::Sell && htf_trend_direction_ == -1)) {
            reward_distance *= 1.3;
        }
        // ADX-based adjustment: low ADX = ranging = less directional edge
        if (snapshot.technical.adx_valid && snapshot.technical.adx < 20.0) {
            reward_distance *= 0.90; // Mild penalty in ranging for scalping
        }
        
        double rr_ratio = (risk_distance > 0.0) ? (reward_distance / risk_distance) : 0.0;
        if (rr_ratio < config_.trading_params.min_risk_reward_ratio) {
            if (tick_count_ % 200 == 0) {
                logger_->info("pipeline", "Trade skipped: poor Risk:Reward",
                    {{"rr_ratio", std::to_string(rr_ratio)},
                     {"min_rr", std::to_string(config_.trading_params.min_risk_reward_ratio)},
                     {"risk", std::to_string(risk_distance / price * 100.0)},
                     {"reward", std::to_string(reward_distance / price * 100.0)},
                     {"strategy", intent.strategy_id.get()},
                     {"symbol", symbol_.get()}});
            }
            return;
        }
    }

    // 8c. Корреляционный монитор: снижаем размер позиции при разрыве корреляции
    double correlation_risk_mult = 1.0;
    if (correlation_monitor_) {
        auto corr_result = correlation_monitor_->evaluate();
        correlation_risk_mult = corr_result.risk_multiplier;
        ml_snapshot_.avg_correlation = corr_result.avg_correlation;
        ml_snapshot_.correlation_break = corr_result.any_break;
        ml_snapshot_.correlation_risk_multiplier = corr_result.risk_multiplier;
        ml_snapshot_.correlation_status = corr_result.component_status;
        if (corr_result.any_break && tick_count_ % 200 == 0) {
            logger_->warn("pipeline",
                "Разрыв корреляции обнаружен — размер позиции снижен",
                {{"risk_mult", std::to_string(correlation_risk_mult)},
                 {"avg_corr", std::to_string(corr_result.avg_correlation)}});
        }
    }

    // 8d. Thompson Sampling: выбираем момент входа
    // thompson_bypass = true когда pending entry только что активировался —
    // повторно вызывать Thompson нельзя, иначе бесконечный цикл Wait→Activate→Wait
    ml::EntryAction thompson_action = ml::EntryAction::EnterNow;
    if (thompson_sampler_ && !thompson_bypass) {
        thompson_action = thompson_sampler_->select_action();
        ml_snapshot_.recommended_wait_periods = ml::wait_periods(thompson_action);
        ml_snapshot_.thompson_status = thompson_sampler_->status();
        for (const auto& arm : thompson_sampler_->get_arms()) {
            if (arm.action == thompson_action) {
                ml_snapshot_.entry_confidence = (arm.alpha + arm.beta > 0.0)
                    ? arm.alpha / (arm.alpha + arm.beta)
                    : 0.5;
                break;
            }
        }
        int wait = ml::wait_periods(thompson_action);

        if (thompson_action == ml::EntryAction::Skip) {
            // Пропускаем сигнал полностью
            consecutive_wait1_count_ = 0;
            if (tick_count_ % 200 == 0) {
                logger_->debug("pipeline",
                    "Thompson Sampling: сигнал пропущен",
                    {{"strategy", intent.strategy_id.get()}});
            }
            return;
        }

        if (wait > 0) {
            // Track consecutive waits for rotation trigger
            ++consecutive_wait1_count_;
            if (consecutive_wait1_count_ >= kMaxConsecutiveWait1) {
                // Thompson repeatedly says Wait — market uncertain for this pair.
                // Force pipeline idle so main.cpp rotation thread picks a new pair.
                last_activity_ns_.store(0, std::memory_order_relaxed);
                logger_->warn("pipeline",
                    "Thompson Wait1 спам: " + std::to_string(consecutive_wait1_count_) +
                    "× подряд — pipeline помечен как idle для ротации пары",
                    {{"consecutive_wait1", std::to_string(consecutive_wait1_count_)},
                     {"threshold", std::to_string(kMaxConsecutiveWait1)},
                     {"symbol", symbol_.get()}});
                consecutive_wait1_count_ = 0;
                return;
            }

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
        consecutive_wait1_count_ = 0;
    }
    ml_snapshot_.compute_aggregates();

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

    // 9a. Cooldown — не торгуем чаще настроенного интервала
    now_ns = clock_->now().get();  // обновляем значение (объявлено выше, в freshness gate)
    int64_t order_cooldown_ns = static_cast<int64_t>(config_.trading_params.order_cooldown_seconds)
                               * 1'000'000'000LL;
    if (last_order_time_ns_ > 0 && (now_ns - last_order_time_ns_) < order_cooldown_ns) {
        return;
    }

    // 9a2. Усиленный cooldown после стоп-лосса — не входить в тот же откат
    int64_t sl_cooldown_ns = static_cast<int64_t>(config_.trading_params.stop_loss_cooldown_seconds)
                            * 1'000'000'000LL;
    if (last_stop_loss_time_ns_ > 0 && (now_ns - last_stop_loss_time_ns_) < sl_cooldown_ns) {
        if (tick_count_ % 500 == 0) {
            int64_t remaining_s = (sl_cooldown_ns - (now_ns - last_stop_loss_time_ns_)) / 1'000'000'000LL;
            logger_->info("pipeline", "Post-SL cooldown активен",
                {{"remaining_s", std::to_string(remaining_s)},
                 {"symbol", symbol_.get()}});
        }
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

        // Минимальный торгуемый нотионал на Bitget (реальный минимум $1 USDT, с запасом $1.10).
        // Пылевые остатки ниже этого порога не считаются реальной позицией.
        const double kMinTradeableNotional = common::exchange_limits::kMinBitgetNotionalUsdt;
        double position_notional = position_size.get() * snapshot.mid_price.get();

        if (config_.futures.enabled) {
            // === Фьючерсная логика определения закрытия / открытия ===

            // Проверяем signal_intent из стратегии для определения семантики:
            //   LongExit / ShortExit → закрытие соответствующей позиции
            //   ShortEntry → открытие short (SELL + Open)
            //   LongEntry → открытие long (BUY + Open)
            //   ReducePosition → частичное сокращение текущей позиции

            const bool is_exit_signal =
                intent.signal_intent == strategy::SignalIntent::LongExit ||
                intent.signal_intent == strategy::SignalIntent::ShortExit ||
                intent.signal_intent == strategy::SignalIntent::ReducePosition;

            if (is_exit_signal && has_position && position_notional >= kMinTradeableNotional) {
                // Закрытие существующей позиции
                is_closing_position = true;
                closing_qty = position_size;
                position_size_for_log = position_size.get();

                // Устанавливаем trade_side=Close и корректную position_side
                intent.trade_side = TradeSide::Close;
                intent.position_side = current_position_side_;

                // Корректируем side для API: закрытие long=sell, закрытие short=buy
                intent.side = (current_position_side_ == PositionSide::Long)
                    ? Side::Sell : Side::Buy;

            } else if (is_exit_signal && !has_position) {
                // Сигнал закрытия, но позиции нет — пропускаем
                logger_->debug("pipeline",
                    "Пропуск EXIT-сигнала: нет открытой позиции (фьючерсы)",
                    {{"symbol", symbol_.get()},
                     {"signal", intent.signal_name}});
                return;

            } else if (!is_exit_signal) {
                // Открытие новой позиции (LongEntry / ShortEntry)
                intent.trade_side = TradeSide::Open;

                if (intent.signal_intent == strategy::SignalIntent::ShortEntry) {
                    intent.position_side = PositionSide::Short;
                    intent.side = Side::Sell;
                } else {
                    intent.position_side = PositionSide::Long;
                    intent.side = Side::Buy;
                }

                // Не открываем позицию при уже существующей в том же направлении
                if (has_position && position_notional >= kMinTradeableNotional) {
                    bool same_direction =
                        (intent.position_side == PositionSide::Long && position_is_long) ||
                        (intent.position_side == PositionSide::Short && !position_is_long);
                    if (same_direction) {
                        logger_->debug("pipeline",
                            "Пропуск: уже есть позиция в том же направлении (фьючерсы)",
                            {{"symbol", symbol_.get()},
                             {"position_side", std::string(to_string(intent.position_side))}});
                        return;
                    }
                }
            }

            // Minimum hold time для стратегического закрытия (не стоп)
            if (is_closing_position && position_entry_time_ns_ > 0 && intent.urgency < 1.0) {
                int64_t hold_ns = clock_->now().get() - position_entry_time_ns_;
                int64_t min_hold_ns = static_cast<int64_t>(config_.trading_params.min_hold_minutes) * 60LL * 1'000'000'000LL;
                if (hold_ns < min_hold_ns) {
                    logger_->debug("pipeline",
                        "Пропуск: позиция слишком молодая для стратегического закрытия",
                        {{"hold_sec", std::to_string(hold_ns / 1'000'000'000LL)},
                         {"symbol", symbol_.get()}});
                    return;
                }
            }

            // Фильтр: стратегический EXIT должен иметь conviction >= 0.5
            if (is_closing_position && intent.urgency < 1.0 && intent.conviction < 0.5) {
                logger_->debug("pipeline",
                    "Пропуск: conviction слишком низкий для стратегического закрытия (фьючерсы)",
                    {{"conviction", std::to_string(intent.conviction)},
                     {"symbol", symbol_.get()}});
                return;
            }
        } else {
            // === Спотовая логика (без изменений) ===

            // Спот-специфичная проверка: SELL без длинной позиции
            if (!intent_is_buy && (!has_position || !position_is_long)) {
                logger_->info("pipeline",
                    "Пропуск SELL: нет длинной позиции для продажи (спот режим)",
                    {{"symbol", symbol_.get()}});
                return;
            }

            // Не наращиваем позицию в том же направлении (только на спот).
            // Пылевые позиции (< min notional) игнорируем — они не могут быть проданы
            // и не должны блокировать новые ордера.
            if (has_position && (position_is_long == intent_is_buy)
                && position_notional >= kMinTradeableNotional) {
                logger_->info("pipeline",
                    "Пропуск: уже есть позиция в том же направлении (спот режим)",
                    {{"symbol", symbol_.get()},
                     {"side", intent_is_buy ? "BUY" : "SELL"},
                     {"size", std::to_string(position_size.get())}});
                return;
            }

            // SELL при открытой BUY = закрытие позиции (только если нотионал достаточный)
            if (!intent_is_buy && has_position && position_is_long
                && position_notional >= kMinTradeableNotional) {
                // Minimum hold time: don't allow strategy SELL within min_hold_minutes
                // Exception: stop-loss strategies bypass this (urgency == 1.0)
                if (position_entry_time_ns_ > 0 && intent.urgency < 1.0) {
                    int64_t hold_ns = clock_->now().get() - position_entry_time_ns_;
                    int64_t min_hold_ns = static_cast<int64_t>(config_.trading_params.min_hold_minutes) * 60LL * 1'000'000'000LL;
                    if (hold_ns < min_hold_ns) {
                        logger_->debug("pipeline",
                            "Пропуск SELL: позиция слишком молодая для стратегического закрытия",
                            {{"hold_sec", std::to_string(hold_ns / 1'000'000'000LL)},
                             {"min_hold_min", std::to_string(config_.trading_params.min_hold_minutes)},
                             {"symbol", symbol_.get()}});
                        return;
                    }
                }

                // Фильтр: стратегический SELL (не стоп) должен иметь conviction >= 0.5
                if (intent.urgency < 1.0 && intent.conviction < 0.5) {
                    logger_->debug("pipeline",
                        "Пропуск SELL: conviction стратегии слишком низкий для закрытия",
                        {{"conviction", std::to_string(intent.conviction)},
                         {"min_exit_conviction", "0.50"},
                         {"strategy", intent.strategy_id.get()},
                         {"symbol", symbol_.get()}});
                    return;
                }

                is_closing_position = true;
                closing_qty = position_size;
                position_size_for_log = position_size.get();
            }
        }
    }

    // 9c. Установить цену для расчёта размера позиции (если стратегия не задала)
    if (!intent.limit_price.has_value() && snapshot.mid_price.get() > 0.0) {
        intent.limit_price = snapshot.mid_price;
    }
    // Всегда устанавливаем snapshot_mid_price как fallback для market ордеров
    if (snapshot.mid_price.get() > 0.0) {
        intent.snapshot_mid_price = snapshot.mid_price;
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
    auto exec_alpha = execution_alpha_->evaluate(intent, snapshot, uncertainty);

    if (is_closing_position) {
        // Закрытие позиции ОБЯЗАНО исполниться → market order.
        // Лимитный ордер на закрытие может не заполниться, и позиция зависнет.
        exec_alpha.recommended_style = execution_alpha::ExecutionStyle::Aggressive;
        exec_alpha.should_execute = true;
        exec_alpha.urgency_score = 1.0;
        intent.urgency = 1.0;

        // Определяем размер для закрытия в зависимости от режима
        if (config_.futures.enabled) {
            // Фьючерсы: размер позиции берём из портфеля (на фьючерсах нет "баланса base coin",
            // позиция — это маржинальный контракт с размером в base currency)
            if (futures_query_adapter_) {
                auto positions = futures_query_adapter_->get_positions(symbol_);
                if (positions) {
                    for (const auto& fp : *positions) {
                        if (fp.position_side == current_position_side_ && fp.total.get() > 0.0) {
                            closing_qty = fp.total;
                            logger_->info("pipeline", "Фьючерсная позиция для закрытия",
                                {{"exchange_qty", std::to_string(fp.total.get())},
                                 {"portfolio_qty", std::to_string(position_size_for_log)},
                                 {"position_side", (current_position_side_ == PositionSide::Long) ? "long" : "short"}});
                            break;
                        }
                    }
                }
            }
            // Fallback на размер из портфеля
            if (closing_qty.get() < 1e-8) {
                closing_qty = Quantity(position_size_for_log);
            }
        } else {
            // Спот: запрашиваем актуальный баланс base coin,
            // чтобы избежать "Insufficient balance" (портфель может расходиться)
            std::string base_coin = extract_base_coin(symbol_.get());
            double actual_base = query_asset_balance(base_coin);
            if (actual_base > 1e-8) {
                closing_qty = Quantity(actual_base);
                logger_->info("pipeline", "Актуальный баланс " + base_coin + " для закрытия",
                    {{"portfolio_qty", std::to_string(position_size_for_log)},
                     {"exchange_qty", std::to_string(actual_base)}});
            }
        }

        // Минимальный ордер Bitget: 0.00001 BTC (или ~$5)
        if (closing_qty.get() < 0.00001) {
            logger_->warn("pipeline", "Размер позиции слишком мал для закрытия",
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
        // 9d. Opportunity Cost — оценка целесообразности входа
        //     Вызывается ПЕРЕД sizing/risk для новых позиций.
        //     Закрытие позиции обходит этот шаг (безусловно исполняется).
        {
            auto port_snap = portfolio_->snapshot();

            opportunity_cost::PortfolioContext oc_ctx;
            // exposure_pct из PortfolioEngine приходит в масштабе 0-100%,
            // а пороги в конфиге — в масштабе 0-1. Нормализуем:
            oc_ctx.gross_exposure_pct = port_snap.exposure.exposure_pct / 100.0;
            oc_ctx.net_exposure_pct = (port_snap.total_capital > 0.0)
                ? port_snap.exposure.net_exposure / port_snap.total_capital : 0.0;
            oc_ctx.available_capital = port_snap.available_capital;
            oc_ctx.total_capital = port_snap.total_capital;
            oc_ctx.open_positions_count = port_snap.exposure.open_positions_count;
            oc_ctx.current_drawdown_pct = port_snap.pnl.current_drawdown_pct / 100.0;
            oc_ctx.consecutive_losses = port_snap.pnl.consecutive_losses;

            // Расчёт концентрации по символу и стратегии
            double symbol_notional = 0.0;
            double strategy_notional = 0.0;
            for (const auto& pos : port_snap.positions) {
                if (pos.symbol.get() == intent.symbol.get()) {
                    symbol_notional += std::abs(pos.notional.get());
                }
                if (pos.strategy_id.get() == intent.strategy_id.get()) {
                    strategy_notional += std::abs(pos.notional.get());
                }
            }
            // Для фьючерсов: пересчитываем exposure по margin (notional / leverage),
            // чтобы корректно сравнивать с порогами concentration (0-1 масштаб).
            double leverage_div = config_.futures.enabled
                ? std::max(1.0, static_cast<double>(config_.futures.default_leverage))
                : 1.0;
            if (port_snap.total_capital > 0.0) {
                oc_ctx.symbol_exposure_pct = (symbol_notional / leverage_div) / port_snap.total_capital;
                oc_ctx.strategy_exposure_pct = (strategy_notional / leverage_div) / port_snap.total_capital;
            }

            auto oc_result = opportunity_cost_engine_->evaluate(
                intent, exec_alpha, oc_ctx,
                config_.decision.min_conviction_threshold);

            if (oc_result.action == opportunity_cost::OpportunityAction::Suppress) {
                logger_->info("pipeline", "Вход отклонён opportunity cost: Suppress",
                    {{"symbol", symbol_.get()},
                     {"reason", opportunity_cost::to_string(oc_result.reason)},
                     {"net_bps", std::to_string(oc_result.score.net_expected_bps)},
                     {"score", std::to_string(oc_result.score.score)}});
                return;
            }

            if (oc_result.action == opportunity_cost::OpportunityAction::Defer) {
                logger_->info("pipeline", "Вход отложен opportunity cost: Defer",
                    {{"symbol", symbol_.get()},
                     {"reason", opportunity_cost::to_string(oc_result.reason)},
                     {"net_bps", std::to_string(oc_result.score.net_expected_bps)}});
                return;
            }

            // Execute или Upgrade — продолжаем к sizing
            if (oc_result.action == opportunity_cost::OpportunityAction::Upgrade) {
                logger_->info("pipeline", "Opportunity cost рекомендует Upgrade",
                    {{"symbol", symbol_.get()},
                     {"net_bps", std::to_string(oc_result.score.net_expected_bps)}});
                // Upgrade будет обработан: пока продолжаем как Execute,
                // закрытие худшей позиции реализуется в следующих фазах.
            }
        }

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

            // Реальные win_rate/win_loss_ratio (default values)
            double win_rate = 0.5;
            double win_loss_ratio = 1.5;

            portfolio_allocator_->set_market_context(
                realized_vol_annual, regime.detailed, win_rate, win_loss_ratio);
        }

        // Combined size multiplier
        double combined_size_mult = uncertainty.size_multiplier *
                correlation_risk_mult;
        if (config_.trading.initial_capital < 100.0) {
            combined_size_mult = std::max(combined_size_mult, 0.80);
        }

        auto sizing = portfolio_allocator_->compute_size(
            intent, portfolio_->snapshot(), combined_size_mult);

        if (!sizing.approved || sizing.approved_quantity.get() <= 0.0) {
            logger_->info("pipeline", "Размер позиции не одобрен аллокатором");
            return;
        }

        // 11. Проверка риск-движком — ОБЯЗАТЕЛЬНА для новых позиций
        risk_decision = risk_engine_->evaluate(
            intent, sizing, portfolio_->snapshot(), snapshot, exec_alpha, uncertainty);

        if (risk_decision.verdict == risk::RiskVerdict::Denied ||
            risk_decision.verdict == risk::RiskVerdict::Throttled) {
            logger_->warn("pipeline", "Сделка отклонена риск-движком",
                {{"verdict", risk::to_string(risk_decision.verdict)},
                 {"reasons", std::to_string(risk_decision.reasons.size())}});
            return;
        }
    }

    // 11a. Smart TWAP: разбиваем крупные ордера на слайсы.
    // Execution Alpha является основным арбитром: если он рекомендует нарезку
    // (slice_plan.has_value()), это учитывается в приоритете над оценкой TWAP.
    const bool exec_alpha_wants_twap = exec_alpha.slice_plan.has_value();
    const bool twap_independently_triggered = twap_executor_
        && twap_executor_->should_use_twap(intent, snapshot);

    if ((exec_alpha_wants_twap || twap_independently_triggered) && twap_executor_) {
        auto twap_plan = twap_executor_->create_twap_plan(
            intent, snapshot, risk_decision.approved_quantity);
        twap_executor_->set_active_twap(twap_plan);
        logger_->info("pipeline", "Активирован Smart TWAP",
            {{"symbol", symbol_.get()},
             {"total_qty", std::to_string(twap_plan.total_qty.get())},
             {"slices", std::to_string(twap_plan.slices.size())},
             {"trigger", exec_alpha_wants_twap ? "execution_alpha" : "twap_engine"}});
        return;  // Первый слайс отправится на следующем тике
    }

    // 11b. Адаптивное кредитное плечо для фьючерсов (только если enabled)
    if (leverage_engine_) {
        // Рассчитываем ATR-нормализованную волатильность
        double atr_normalized = 0.0;
        if (snapshot.technical.atr_valid && snapshot.technical.atr_14 > 0.0 && snapshot.mid_price.get() > 0.0) {
            atr_normalized = snapshot.technical.atr_14 / snapshot.mid_price.get();
        }

        // Вычисляем оптимальное плечо на основе рыночных условий
        leverage::LeverageContext lev_ctx;
        lev_ctx.regime = regime.label;
        lev_ctx.uncertainty = uncertainty.level;
        lev_ctx.atr_normalized = atr_normalized;
        lev_ctx.drawdown_pct = portfolio_->snapshot().pnl.current_drawdown_pct;
        lev_ctx.adversarial_severity = 0.0;  // adversarial module removed
        lev_ctx.conviction = intent.conviction;
        lev_ctx.funding_rate = current_funding_rate_;
        lev_ctx.position_side = intent.position_side;
        lev_ctx.entry_price = snapshot.mid_price.get();

        auto lev_decision = leverage_engine_->compute_leverage(lev_ctx);

        // Проверяем, что плечо безопасно (достаточный буфер до ликвидации)
        if (!lev_decision.is_safe) {
            logger_->warn("pipeline", "Ликвидационный буфер недостаточен — сделка отклонена",
                {{"leverage", std::to_string(lev_decision.leverage)},
                 {"liquidation_buffer_pct", std::to_string(lev_decision.liquidation_buffer_pct)},
                 {"min_buffer_pct", std::to_string(config_.futures.liquidation_buffer_pct)},
                 {"symbol", symbol_.get()}});
            return;
        }

        // Устанавливаем плечо на бирже перед отправкой ордера
        if (futures_submitter_ && intent.trade_side == TradeSide::Open) {
            std::string hold_side = (intent.position_side == PositionSide::Long) ? "long" : "short";
            bool set_lev_ok = futures_submitter_->set_leverage(symbol_, lev_decision.leverage, hold_side);
            if (!set_lev_ok) {
                logger_->error("pipeline",
                    "Не удалось установить плечо на бирже — ордер отклонён. "
                    "Торговля с неверным плечом может привести к ликвидации.",
                    {{"leverage", std::to_string(lev_decision.leverage)},
                     {"hold_side", hold_side},
                     {"symbol", symbol_.get()}});
                // Устанавливаем cooldown чтобы не спамить биржу на каждом тике
                last_order_time_ns_ = std::chrono::steady_clock::now().time_since_epoch().count();
                return;
            }
            // v11: синхронизируем leverage в execution engine для корректного reserve_cash
            if (execution_engine_) {
                execution_engine_->set_leverage(static_cast<double>(lev_decision.leverage));
            }
            logger_->debug("pipeline", "Плечо установлено на бирже",
                {{"leverage", std::to_string(lev_decision.leverage)},
                 {"hold_side", hold_side},
                 {"rationale", lev_decision.rationale}});
        }
    }

    // 12. Исполнение ордера
    // Устанавливаем cooldown ПЕРЕД отправкой — чтобы даже при ошибке
    // не спамить биржу повторными запросами.
    last_order_time_ns_ = clock_->now().get();
    last_activity_ns_.store(last_order_time_ns_, std::memory_order_relaxed);

    auto order_result = execution_engine_->execute(intent, risk_decision, exec_alpha, uncertainty);
    if (order_result) {
        consecutive_rejections_ = 0;  // Успешный ордер — сброс backoff
        risk_engine_->record_order_sent();
        logger_->info("pipeline", "ОРДЕР ОТПРАВЛЕН",
            {{"order_id", order_result->get()},
             {"strategy", intent.strategy_id.get()},
             {"side", intent.side == Side::Buy ? "BUY" : "SELL"},
             {"qty", std::to_string(risk_decision.approved_quantity.get())},
             {"symbol", symbol_.get()},
             {"conviction", std::to_string(intent.conviction)}});

        // Инициализация trailing stop при открытии новой позиции
        // Фьючерсы: открытие = trade_side==Open (и Long, и Short)
        // Спот: открытие = side==Buy
        const bool is_new_position = config_.futures.enabled
            ? (intent.trade_side == TradeSide::Open)
            : (intent.side == Side::Buy);

        if (is_new_position) {
            current_position_strategy_    = intent.strategy_id;
            current_position_side_        = intent.position_side;
            current_position_conviction_  = intent.conviction;
            current_entry_thompson_action_ = thompson_action;
            // Fix: сохраняем проскальзывание из execution_context для последующей записи в alpha decay
            current_position_slippage_bps_ = snapshot.execution_context.estimated_slippage_bps;
            // Fix: сбрасываем MAE для новой позиции
            current_max_adverse_excursion_bps_ = 0.0;
            // Сохраняем exec_alpha для C/C fee estimation при закрытии позиции
            last_exec_alpha_ = exec_alpha;
            reset_trailing_state();
            double entry_price = snapshot.mid_price.get();
            highest_price_since_entry_ = entry_price;
            lowest_price_since_entry_ = entry_price;
            initial_position_size_ = risk_decision.approved_quantity.get();
            position_entry_time_ns_ = clock_->now().get();

            // Сохраняем fingerprint на входе для записи результата при закрытии
            if (fingerprinter_) {
                last_entry_fingerprint_ = fingerprinter_->create_fingerprint(snapshot);
            }
        }

        // Сброс trailing state при полном закрытии позиции
        // Фьючерсы: закрытие = trade_side==Close
        // Спот: закрытие = side==Sell и is_closing_position
        const bool is_position_close = config_.futures.enabled
            ? (intent.trade_side == TradeSide::Close && is_closing_position)
            : (intent.side == Side::Sell && is_closing_position);

        if (is_position_close) {
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
            risk_engine_->record_trade_close(current_position_strategy_, symbol_, closing_pnl);

            reset_trailing_state();
        }
    } else {
        ++consecutive_rejections_;
        // Экспоненциальный backoff: cooldown × 2^(rejections-1), макс 10 минут
        int64_t base_cooldown = static_cast<int64_t>(config_.trading_params.order_cooldown_seconds)
                               * 1'000'000'000LL;
        int64_t backoff = base_cooldown * (1LL << std::min(consecutive_rejections_, 8));
        backoff = std::min(backoff, kMaxRejectionBackoffNs);
        last_order_time_ns_ = clock_->now().get() + backoff - base_cooldown;
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
         {"adx", fmt(snap.technical.adx, 1)},
         {"mom5", fmt(snap.technical.momentum_5, 4)},
         {"ema", ema_signal},
         {"bb_pos", fmt(bb_pos, 2)},
         {"atr", fmt(snap.technical.atr_14, atr_prec)},
         {"regime", std::string(to_string(regime.label))},
         {"world", std::string(world_model::to_string(world.state))},
         {"positions", std::to_string(portfolio_->snapshot().exposure.open_positions_count)}});
}

void TradingPipeline::update_current_mae(double current_price, bool is_long) {
    if (current_price <= 0.0) return;
    // MAE в бп относительно цены входа
    double entry = 0.0;
    auto pos = portfolio_->get_position(symbol_);
    if (pos.has_value() && pos->avg_entry_price.get() > 0.0) {
        entry = pos->avg_entry_price.get();
    }
    if (entry <= 0.0) return;

    double adverse_bps = 0.0;
    if (is_long) {
        // Для лонга: неблагоприятное движение = цена ниже входа
        adverse_bps = (entry - current_price) / entry * 10000.0;
    } else {
        // Для шорта: неблагоприятное движение = цена выше входа
        adverse_bps = (current_price - entry) / entry * 10000.0;
    }
    if (adverse_bps > current_max_adverse_excursion_bps_) {
        current_max_adverse_excursion_bps_ = adverse_bps;
    }
}

// ==================== Phase 1: Freshness Validation ====================

FreshnessResult TradingPipeline::check_quote_freshness(
    const features::FeatureSnapshot& snapshot) const
{
    FreshnessResult result;
    if (snapshot.computed_at.get() <= 0) return result;  // unknown — считаем свежим
    result.age_ns = clock_->now().get() - snapshot.computed_at.get();
    result.is_fresh = (result.age_ns <= result.max_age_ns);
    return result;
}

// ==================== Phase 1/2/4: Periodic Tasks ====================

void TradingPipeline::run_periodic_tasks(int64_t now_ns) {
    run_order_watchdog(now_ns);
    run_continuous_reconciliation(now_ns);

    // Периодическая синхронизация баланса с биржей (каждые 5 минут).
    // Обнаруживает расхождения после ручных сделок, сбоев API, или пропущенных fill'ов.
    if (rest_client_ &&
        (last_balance_sync_ns_ == 0 || (now_ns - last_balance_sync_ns_) >= kBalanceSyncIntervalNs)) {
        last_balance_sync_ns_ = now_ns;
        sync_balance_from_exchange();
    }

    // Периодическое обновление funding rate для фьючерсов (каждые 5 минут).
    // Funding rate используется LeverageEngine для адаптивного снижения плеча при высоком фандинге.
    if (futures_query_adapter_ &&
        (last_funding_rate_update_ns_ == 0 || (now_ns - last_funding_rate_update_ns_) >= kFundingRateUpdateIntervalNs)) {
        last_funding_rate_update_ns_ = now_ns;
        double new_rate = futures_query_adapter_->get_current_funding_rate(symbol_);
        if (std::abs(new_rate - current_funding_rate_) > 1e-8) {
            logger_->info("pipeline", "Funding rate обновлён",
                {{"symbol", symbol_.get()},
                 {"old_rate", std::to_string(current_funding_rate_)},
                 {"new_rate", std::to_string(new_rate)}});
        }
        current_funding_rate_ = new_rate;
    }
}

void TradingPipeline::run_order_watchdog(int64_t now_ns) {
    if (!order_watchdog_) return;
    if (last_watchdog_ns_ > 0 && (now_ns - last_watchdog_ns_) < kWatchdogIntervalNs) return;
    last_watchdog_ns_ = now_ns;

    auto reports = order_watchdog_->run_check();
    for (const auto& rep : reports) {
        if (rep.action != WatchdogOrderAction::Ok) {
            const char* action_str = [&]() -> const char* {
                switch (rep.action) {
                    case WatchdogOrderAction::Cancel:       return "cancel";
                    case WatchdogOrderAction::RecoverState: return "recover_state";
                    case WatchdogOrderAction::ForceClose:   return "force_close";
                    default:                                return "ok";
                }
            }();
            logger_->warn("watchdog", "Обнаружена проблема с ордером",
                {{"order_id", rep.order_id.get()},
                 {"action", action_str},
                 {"reason", rep.reason},
                 {"age_ms", std::to_string(rep.age_ms)}});
        }
    }

    // Очистка терминальных ордеров старше 1 часа для предотвращения утечки памяти
    static constexpr int64_t kTerminalOrderMaxAgeNs = 3600'000'000'000LL;
    execution_engine_->cleanup_terminal_orders(kTerminalOrderMaxAgeNs);
}

void TradingPipeline::run_continuous_reconciliation(int64_t now_ns) {
    if (!reconciliation_engine_) return;
    if (last_reconciliation_ns_ > 0 &&
        (now_ns - last_reconciliation_ns_) < kReconciliationIntervalNs) return;
    last_reconciliation_ns_ = now_ns;

    auto active_orders = execution_engine_->active_orders();
    if (active_orders.empty()) return;

    auto result = reconciliation_engine_->reconcile_active_orders(active_orders);

    if (!result.mismatches.empty()) {
        logger_->warn("reconciliation", "Обнаружены расхождения в runtime reconciliation",
            {{"mismatches", std::to_string(result.mismatches.size())},
             {"symbol", symbol_.get()}});

        for (const auto& mismatch : result.mismatches) {
            logger_->warn("reconciliation", "Расхождение",
                {{"type", reconciliation::to_string(mismatch.type)},
                 {"order_id", mismatch.order_id.get()},
                 {"resolved", mismatch.resolved ? "true" : "false"}});
        }
    } else if (tick_count_ % 500 == 0) {
        logger_->debug("reconciliation", "Runtime reconciliation OK",
            {{"active_orders", std::to_string(active_orders.size())},
             {"symbol", symbol_.get()}});
    }
}

} // namespace tb::pipeline
