/**
 * @file trading_pipeline.cpp
 * @brief Реализация торгового pipeline
 */

#include "pipeline/trading_pipeline.hpp"
#include "exchange/bitget/bitget_rest_client.hpp"
#include "execution/order_submitter.hpp"
#include "common/enums.hpp"
#include "common/constants.hpp"
#include "recovery/recovery_service.hpp"
#include "persistence/memory_storage_adapter.hpp"
#include "persistence/persistence_layer.hpp"
#include "persistence/postgres_storage_adapter.hpp"
#include <boost/json.hpp>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cmath>
#include <numeric>
#include <cstdlib>
#include <functional>
#include <random>
#include <thread>
#include <chrono>
#include <limits>

namespace {
/// PRNG для anti-fingerprinting (order timing jitter, size noise).
/// Используется thread_local для потокобезопасности без мьютекса.
/// std::mt19937 — Mersenne Twister (Matsumoto & Nishimura, 1998),
/// статистически качественный для моделирования, но NOT для криптографии.
thread_local std::mt19937 g_rng{std::random_device{}()};

/// Добавить шум ±noise_pct% к размеру ордера (anti-fingerprinting).
/// Размеры 1.000, 1.000, 1.000 USDT → легко отслеживаются.
/// Размеры 0.987, 1.012, 0.993 USDT → статистически нераспознаваемы.
double apply_quantity_noise(double qty, double noise_pct = 2.0) {
    if (qty <= 0.0) return qty;
    std::uniform_real_distribution<double> dist(-noise_pct, noise_pct);
    double noise = dist(g_rng) / 100.0;
    return qty * (1.0 + noise);
}

/// Сгенерировать случайную задержку [min_ms, max_ms] для order timing jitter.
/// Предотвращает обнаружение паттернов по времени подачи ордеров.
int generate_order_jitter_ms(int min_ms = 50, int max_ms = 300) {
    std::uniform_int_distribution<int> dist(min_ms, max_ms);
    return dist(g_rng);
}

int64_t stable_periodic_offset_ns(const std::string& symbol,
                                  const char* salt,
                                  int64_t max_offset_ns) {
    if (symbol.empty() || !salt || max_offset_ns <= 0) {
        return 0;
    }

    std::string key = symbol;
    key += ':';
    key += salt;
    const auto hash = std::hash<std::string>{}(key);
    return static_cast<int64_t>(hash % static_cast<uint64_t>(max_offset_ns));
}

int64_t monotonic_time_ns() {
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1'000'000'000LL + ts.tv_nsec;
}

class TickStageProfiler {
public:
    enum class Stage : size_t {
        Ingress = 0,
        Ml,
        Context,
        Signal,
        Risk,
        Exec,
        Count
    };

    class Scope {
    public:
        Scope(TickStageProfiler& owner, Stage stage)
            : owner_(owner)
            , stage_(stage)
            , start_ns_(monotonic_time_ns())
        {}

        ~Scope() {
            owner_.add(stage_, (monotonic_time_ns() - start_ns_) / 1'000);
        }

        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
        Scope(Scope&&) = delete;
        Scope& operator=(Scope&&) = delete;

    private:
        TickStageProfiler& owner_;
        Stage stage_;
        int64_t start_ns_{0};
    };

    TickStageProfiler(tb::pipeline::PipelineLatencyTracker* tracker,
                      std::shared_ptr<tb::logging::ILogger> logger,
                      std::string symbol,
                      int* slow_tick_log_counter)
        : tracker_(tracker)
        , logger_(std::move(logger))
        , symbol_(std::move(symbol))
        , slow_tick_log_counter_(slow_tick_log_counter)
        , total_start_ns_(monotonic_time_ns())
    {}

    [[nodiscard]] Scope scope(Stage stage) {
        return Scope(*this, stage);
    }

    ~TickStageProfiler() {
        const int64_t total_us = (monotonic_time_ns() - total_start_ns_) / 1'000;
        if (tracker_) {
            for (size_t i = 0; i < stage_totals_us_.size(); ++i) {
                if (stage_totals_us_[i] > 0) {
                    tracker_->record(stage_name(static_cast<Stage>(i)), stage_totals_us_[i]);
                }
            }
            tracker_->record("tick_total", total_us);
            tracker_->check_sla("tick_total", total_us, tb::pipeline::kBudgetTotalUs);
        }

        if (total_us <= tb::pipeline::kBudgetTotalUs || !logger_ || !slow_tick_log_counter_) {
            return;
        }

        const int current_log_count = ++(*slow_tick_log_counter_);
        const bool should_log_breakdown = current_log_count <= 12 || total_us >= 10'000;
        if (!should_log_breakdown) {
            return;
        }

        size_t dominant_index = 0;
        int64_t accounted_us = 0;
        for (size_t i = 0; i < stage_totals_us_.size(); ++i) {
            accounted_us += stage_totals_us_[i];
            if (stage_totals_us_[i] > stage_totals_us_[dominant_index]) {
                dominant_index = i;
            }
        }

        logger_->warn("latency", "Slow tick breakdown",
            {{"symbol", symbol_},
             {"total_us", std::to_string(total_us)},
             {"ingress_us", std::to_string(stage_totals_us_[static_cast<size_t>(Stage::Ingress)])},
             {"ml_us", std::to_string(stage_totals_us_[static_cast<size_t>(Stage::Ml)])},
             {"context_us", std::to_string(stage_totals_us_[static_cast<size_t>(Stage::Context)])},
             {"signal_us", std::to_string(stage_totals_us_[static_cast<size_t>(Stage::Signal)])},
             {"risk_us", std::to_string(stage_totals_us_[static_cast<size_t>(Stage::Risk)])},
             {"exec_us", std::to_string(stage_totals_us_[static_cast<size_t>(Stage::Exec)])},
             {"unaccounted_us", std::to_string(std::max<int64_t>(0, total_us - accounted_us))},
             {"dominant_stage", std::string(stage_name(static_cast<Stage>(dominant_index)))},
             {"dominant_stage_us", std::to_string(stage_totals_us_[dominant_index])},
             {"slow_tick_log_count", std::to_string(current_log_count)}});
    }

private:
    void add(Stage stage, int64_t duration_us) {
        stage_totals_us_[static_cast<size_t>(stage)] += std::max<int64_t>(0, duration_us);
    }

    static std::string_view stage_name(Stage stage) {
        switch (stage) {
            case Stage::Ingress: return "tick_ingress";
            case Stage::Ml: return "tick_ml";
            case Stage::Context: return "tick_context";
            case Stage::Signal: return "tick_signal";
            case Stage::Risk: return "tick_risk";
            case Stage::Exec: return "tick_exec";
            case Stage::Count: break;
        }
        return "tick_unknown";
    }

    tb::pipeline::PipelineLatencyTracker* tracker_{nullptr};
    std::shared_ptr<tb::logging::ILogger> logger_;
    std::string symbol_;
    int* slow_tick_log_counter_{nullptr};
    int64_t total_start_ns_{0};
    std::array<int64_t, static_cast<size_t>(Stage::Count)> stage_totals_us_{};
};

/// Парсинг double из JSON значения (строки или числа)
double parse_json_double(const boost::json::value& v) {
    if (v.is_string()) return std::stod(std::string(v.as_string()));
    if (v.is_double()) return v.as_double();
    if (v.is_int64()) return static_cast<double>(v.as_int64());
    return 0.0;
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
    const std::string& symbol,
    std::shared_ptr<portfolio::IPortfolioEngine> shared_portfolio)
    : config_(config)
    , symbol_(Symbol(symbol.empty() ? "BTCUSDT" : symbol))
    , secret_provider_(std::move(secret_provider))
    , logger_(std::move(logger))
    , clock_(std::move(clock))
    , metrics_(std::move(metrics))
{
    // ===== FUTURES-ONLY ENFORCEMENT =====
    if (!config_.futures.enabled) {
        throw std::runtime_error(
            "Bot is FUTURES-ONLY (USDT-M). Set futures.enabled=true in config.");
    }

    // 1. Индикаторы и признаки
    indicator_engine_ = std::make_shared<indicators::IndicatorEngine>(logger_);
    feature_engine_ = std::make_shared<features::FeatureEngine>(
        features::FeatureEngine::Config{}, indicator_engine_, clock_, logger_, metrics_);

    // 2. Стакан ордеров
    order_book_ = std::make_shared<order_book::LocalOrderBook>(symbol_, logger_, metrics_);

    // 3. Аналитический слой
    world_model_ = std::make_shared<world_model::RuleBasedWorldModelEngine>(
        config_.world_model, logger_, clock_);
    regime_engine_ = std::make_shared<regime::RuleBasedRegimeEngine>(logger_, clock_, metrics_, config_.regime);
    uncertainty_engine_ = std::make_shared<uncertainty::RuleBasedUncertaintyEngine>(
        config_.uncertainty, logger_, clock_);

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
    features::VpinConfig vpin_cfg;
    vpin_cfg.toxic_threshold = config_.execution_alpha.vpin_toxic_threshold;
    advanced_features_ = std::make_shared<features::AdvancedFeatureEngine>(
        features::CusumConfig{}, vpin_cfg,
        features::VolumeProfileConfig{}, features::TimeOfDayConfig{},
        logger_, clock_);

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

    // 6. Портфель (shared account-level или локальный fallback)
    if (shared_portfolio) {
        portfolio_ = std::move(shared_portfolio);
    } else {
        portfolio_ = std::make_shared<portfolio::InMemoryPortfolioEngine>(
            config_.trading.initial_capital, logger_, clock_, metrics_);
    }
    portfolio_->set_leverage(static_cast<double>(config_.futures.default_leverage));

    // 7. Размер позиции (адаптация под размер капитала)
    portfolio_allocator::HierarchicalAllocator::Config alloc_cfg;
    alloc_cfg.budget.global_budget = config_.trading.initial_capital;
    // Фьючерсы — масштабировать бюджет на leverage
    alloc_cfg.max_leverage = static_cast<double>(config_.futures.default_leverage);
    alloc_cfg.budget.global_budget *= alloc_cfg.max_leverage;
    // Для маленьких аккаунтов (< $100) — ослабляем лимиты концентрации
    // и повышаем target_vol, чтобы не блокировать минимальные ордера биржи ($1 USDT)
    if (config_.trading.initial_capital < 100.0) {
        alloc_cfg.max_concentration_pct = 0.90;    // Микро-аккаунт: вся ставка на одну позицию
        alloc_cfg.max_strategy_allocation_pct = 0.90;
        alloc_cfg.budget.symbol_budget_pct = 0.90;
        alloc_cfg.target_annual_vol = 1.50;       // 150% — адекватнее для крипто-фьючерсов
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
        ea_cfg.taker_fee_bps                   = config_.execution_alpha.taker_fee_bps;
        ea_cfg.maker_fee_bps                   = config_.execution_alpha.maker_fee_bps;
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
        oc_cfg.consecutive_loss_penalty   = config_.opportunity_cost.consecutive_loss_penalty;

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
    risk_cfg.max_intraday_drawdown_pct = config_.risk.max_intraday_drawdown_pct;
    risk_cfg.utc_cutoff_hour = config_.risk.utc_cutoff_hour;
    // Для маленьких аккаунтов — снижаем порог минимальной ликвидности
    if (config_.trading.initial_capital < 100.0) {
        risk_cfg.min_liquidity_depth = 1.0;
    }
    // v11: Фьючерсы с плечом — экспозиция и leverage пропорциональны max_leverage.
    // LeverageEngine может выставить плечо до max_leverage, поэтому пределы risk engine
    // должны использовать max_leverage, а не default_leverage.
    // С плечом 20x notional = 20× capital → exposure = 2000%. Headroom 10%.
    {
        const double max_lev = static_cast<double>(config_.futures.max_leverage);
        risk_cfg.max_gross_exposure_pct = max_lev * 110.0;      // 20x → 2200% headroom
        risk_cfg.max_leverage = max_lev;                         // 20x → 20x (точный лимит)
        risk_cfg.max_symbol_concentration_pct = max_lev * 110.0; // Same for single-pair
    }
    risk_engine_ = std::make_shared<risk::ProductionRiskEngine>(
        risk_cfg, logger_, clock_, metrics_);

    // 9.5. Адаптивное управление кредитным плечом (USDT-M фьючерсы)
    leverage_engine_ = std::make_shared<leverage::LeverageEngine>(config_.futures);
    logger_->info("pipeline", "LeverageEngine инициализирован",
        {{"max_leverage", std::to_string(config_.futures.max_leverage)},
         {"margin_mode", config_.futures.margin_mode},
         {"liquidation_buffer_pct", std::to_string(config_.futures.liquidation_buffer_pct)}});

    // 10. Исполнение — выбор submitter по режиму
    std::shared_ptr<execution::IOrderSubmitter> submitter;

    if (config_.trading.mode == TradingMode::Paper) {
        // Paper mode — имитация ордеров без реальных API вызовов
        auto paper_submitter = std::make_shared<execution::PaperOrderSubmitter>();
        paper_submitter_ = paper_submitter;
        submitter = paper_submitter;
        logger_->info("pipeline", "Режим исполнения: PAPER (ордера не отправляются)");

        // Всё равно создаём REST клиент для получения рыночных данных
        auto key_result = secret_provider_->get_secret(
            security::SecretRef{config_.exchange.api_key_ref});
        auto secret_result = secret_provider_->get_secret(
            security::SecretRef{config_.exchange.api_secret_ref});
        auto pass_result = secret_provider_->get_secret(
            security::SecretRef{config_.exchange.passphrase_ref});

        if (key_result && secret_result && pass_result) {
            auto rest_client = std::make_shared<exchange::bitget::BitgetRestClient>(
                config_.exchange.endpoint_rest,
                *key_result, *secret_result, *pass_result,
                logger_, config_.exchange.timeout_ms);
            rest_client_ = rest_client;
        }
    } else {
        // Production — реальная торговля, загружаем API ключи
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

        rest_client_ = rest_client;

        auto futures_sub = std::make_shared<exchange::bitget::BitgetFuturesOrderSubmitter>(
            rest_client, logger_, config_.futures);
        futures_submitter_ = futures_sub;
        submitter = futures_sub;
        logger_->info("pipeline",
            "Режим исполнения: ФЬЮЧЕРСЫ USDT-M (Bitget Mix API v2)");
    }

    execution_engine_ = std::make_shared<execution::ExecutionEngine>(
        submitter, portfolio_, logger_, clock_, metrics_);
    execution_engine_->set_leverage(static_cast<double>(config_.futures.default_leverage));

    // 10a. Smart TWAP — адаптивное нарезание крупных ордеров
    twap_executor_ = std::make_shared<execution::SmartTwapExecutor>(
        execution::TwapConfig{}, logger_);

    // 11. Шлюз рыночных данных
    market_data::GatewayConfig gw_cfg;
    gw_cfg.ws_config.url = config_.exchange.endpoint_ws;
    gw_cfg.symbols = {symbol_};
    gw_cfg.inst_type = "USDT-FUTURES";

    gateway_ = std::make_shared<market_data::MarketDataGateway>(
        gw_cfg, feature_engine_, order_book_, logger_, metrics_, clock_,
        [this](features::FeatureSnapshot snap) { on_feature_snapshot(std::move(snap)); },
        [this](double price, double volume, bool is_buy) {
            if (advanced_features_) {
                advanced_features_->on_trade(price, volume, is_buy);
            }
        });

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
        // Фьючерсный режим — используем futures query adapter
        futures_query_adapter_ = std::make_shared<exchange::bitget::BitgetFuturesQueryAdapter>(
            rest_client_, logger_, config_.futures);
        reconciliation::ReconciliationConfig rec_cfg;
        rec_cfg.auto_resolve_state_mismatches = true;
        rec_cfg.balance_tolerance_pct = 1.0;
        reconciliation_engine_ = std::make_shared<reconciliation::ReconciliationEngine>(
            rec_cfg, futures_query_adapter_, logger_, clock_, metrics_);
        logger_->info("pipeline", "Continuous reconciliation engine инициализирован (Futures режим)");

        recovery::RecoveryConfig recovery_cfg;
        recovery_cfg.enabled = true;
        recovery_cfg.close_orphan_positions = false;
        recovery_cfg.symbol_filter = symbol_;
        // ИСПРАВЛЕНИЕ H3: использовать реальный minTradeUSDT вместо hardcoded 5.0.
        // exchange_rules_.min_trade_usdt устанавливается из ScannerEngine данных.
        if (exchange_rules_.min_trade_usdt > 0.0) {
            recovery_cfg.min_position_value_usd = exchange_rules_.min_trade_usdt;
        } else if (config_.trading.mode == TradingMode::Production) {
            // ИСПРАВЛЕНИЕ H1 (аудит): в production нельзя запускать recovery
            // без знания min_trade_usdt — используем безопасный fallback $5 (Bitget BTC minimum)
            recovery_cfg.min_position_value_usd = 5.0;
            logger_->warn("pipeline",
                "Exchange rules не установлены при startup recovery — "
                "используется безопасный fallback min_position_value=$5",
                {{"symbol", symbol_.get()}});
        }

        persistence::PersistenceConfig recovery_persistence_cfg;
        // ИСПРАВЛЕНИЕ H2: persistence включена при наличии PostgreSQL storage.
        // Если pg storage недоступен — используем memory adapter (persistence off по умолчанию).
        recovery_persistence_cfg.enabled = (shared_pg_storage != nullptr);
        auto recovery_storage = shared_pg_storage
            ? shared_pg_storage
            : std::static_pointer_cast<persistence::IStorageAdapter>(
                std::make_shared<persistence::MemoryStorageAdapter>());
        auto recovery_persistence = std::make_shared<persistence::PersistenceLayer>(
            recovery_storage, recovery_persistence_cfg);

        std::shared_ptr<reconciliation::IExchangeQueryService> recovery_query =
            std::static_pointer_cast<reconciliation::IExchangeQueryService>(futures_query_adapter_);

        if (recovery_query) {
            recovery::RecoveryService recovery_service(
                recovery_cfg,
                recovery_query,
                portfolio_,
                recovery_persistence,
                logger_,
                clock_,
                metrics_);

            auto recovery_result = recovery_service.recover_on_startup();
            if (recovery_result.status == recovery::RecoveryStatus::Failed) {
                logger_->critical("pipeline", "Startup recovery завершился ошибкой",
                    {{"symbol", symbol_.get()},
                     {"errors", std::to_string(recovery_result.errors)}});
                throw std::runtime_error("startup recovery failed for symbol " + symbol_.get());
            }

            logger_->info("pipeline", "Startup recovery завершён",
                {{"symbol", symbol_.get()},
                 {"status", std::string(recovery::to_string(recovery_result.status))},
                 {"positions", std::to_string(recovery_result.recovered_positions.size())},
                 {"balance", std::to_string(recovery_result.recovered_cash_balance)}});

            // Обновляем бюджет аллокатора реальным балансом с биржи
            if (recovery_result.recovered_cash_balance > 0.0 && portfolio_allocator_) {
                double real_budget = recovery_result.recovered_cash_balance
                                   * static_cast<double>(config_.futures.default_leverage);
                portfolio_allocator_->update_global_budget(real_budget);
                logger_->info("pipeline", "Allocator budget обновлён из биржи",
                    {{"exchange_balance", std::to_string(recovery_result.recovered_cash_balance)},
                     {"leveraged_budget", std::to_string(real_budget)}});
            }
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
    if (futures_submitter_) {
        futures_submitter_->set_rules(rules.symbol, rules);
        logger_->info("pipeline", "Exchange rules установлены (Futures)",
            {{"symbol", rules.symbol.get()},
             {"qty_precision", std::to_string(rules.quantity_precision)},
             {"price_precision", std::to_string(rules.price_precision)},
             {"min_trade_usdt", std::to_string(rules.min_trade_usdt)}});
    }
    if (paper_submitter_) {
        paper_submitter_->set_rules(rules.symbol, rules);
        logger_->info("pipeline", "Exchange rules установлены (Paper)",
            {{"symbol", rules.symbol.get()},
             {"qty_precision", std::to_string(rules.quantity_precision)},
             {"price_precision", std::to_string(rules.price_precision)},
             {"min_trade_usdt", std::to_string(rules.min_trade_usdt)}});
    }
    // Propagate per-symbol min notional to risk engine
    if (risk_engine_) {
        risk_engine_->set_min_notional_usdt(rules.min_trade_usdt);
    }
    // Propagate to TWAP executor
    if (twap_executor_) {
        twap_executor_->set_min_notional_usdt(rules.min_trade_usdt);
    }
    // ИСПРАВЛЕНИЕ H5: Propagate to execution engine (устраняет рассогласование min notional)
    if (execution_engine_) {
        execution_engine_->set_min_notional_usdt(rules.min_trade_usdt);
    }
}

// ==================== Загрузка точности ордеров ====================

void TradingPipeline::fetch_symbol_precision() {
    if (!rest_client_ || (!futures_submitter_ && !paper_submitter_)) return;

    try {
        std::string endpoint = "/api/v2/mix/market/contracts";
        std::string params = "productType=USDT-FUTURES&symbol=" + symbol_.get();

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
            // Futures API: volumePlace, pricePlace (fallback: quantityPrecision)
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

    // Futures-only: Mix account API
    std::string endpoint = "/api/v2/mix/account/accounts?productType=USDT-FUTURES";

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

        auto& data = obj.at("data").as_array();
        for (const auto& item : data) {
            auto& asset = item.as_object();
            // USDT-M Futures API: каждый аккаунт имеет "marginCoin" (USDT)
            if (!asset.contains("marginCoin")) continue;
            std::string coin(asset.at("marginCoin").as_string());
            if (coin != "USDT") continue;

            // Используем usdtEquity (полный equity), а не available
            // (который уже вычел margin открытых позиций).
            // Это предотвращает двойной учёт margin при расчёте portfolio exposure.
            if (asset.contains("usdtEquity")) {
                usdt_available = std::stod(
                    std::string(asset.at("usdtEquity").as_string()));
            } else if (asset.contains("available")) {
                usdt_available = std::stod(
                    std::string(asset.at("available").as_string()));
            }
            break; // USDT-M: единственный margin coin
        }

        logger_->info("pipeline", "Баланс получен с биржи",
            {{"USDT", std::to_string(usdt_available)}});

        // Обновить капитал USDT — при shared portfolio (num_pipelines>1)
        // устанавливаем полный баланс, т.к. shared portfolio видит ВСЕ позиции.
        // При одиночном pipeline используем весь баланс напрямую.
        portfolio_->set_capital(usdt_available);

        logger_->info("pipeline", "Капитал для этого pipeline",
            {{"total_usdt", std::to_string(usdt_available)},
             {"num_pipelines", std::to_string(num_pipelines_)}});

        // ИСПРАВЛЕНИЕ C1: Фьючерсы — полная leg-aware синхронизация позиций
        // В hedge-режиме Bitget позволяет одновременно long + short.
        // Нужно восстановить ОБЕ ноги и корректно установить hedge_active_.
        if (futures_query_adapter_) {
            auto fp_result = futures_query_adapter_->get_positions(symbol_);
            if (fp_result) {
                // Собираем ВСЕ активные ноги (не break после первой!)
                struct LegInfo {
                    double qty{0.0};
                    double entry{0.0};
                    PositionSide side{PositionSide::Long};
                };
                std::optional<LegInfo> long_leg;
                std::optional<LegInfo> short_leg;

                for (const auto& fp : *fp_result) {
                    if (fp.total.get() <= 0.0) continue;
                    LegInfo leg;
                    leg.qty = fp.total.get();
                    leg.entry = fp.entry_price.get();
                    leg.side = fp.position_side;
                    if (fp.position_side == PositionSide::Long) {
                        long_leg = leg;
                    } else {
                        short_leg = leg;
                    }
                }

                const bool has_long = long_leg.has_value();
                const bool has_short = short_leg.has_value();

                // Синхронизируем каждую ногу в портфель отдельно
                auto sync_leg = [&](const LegInfo& leg) {
                    auto local = portfolio_->get_position(symbol_, leg.side);
                    if (!local.has_value()) {
                        portfolio::Position sync_pos;
                        sync_pos.symbol = symbol_;
                        sync_pos.side = (leg.side == PositionSide::Long) ? Side::Buy : Side::Sell;
                        sync_pos.position_side = leg.side;
                        sync_pos.size = Quantity(leg.qty);
                        sync_pos.avg_entry_price = Price(leg.entry);
                        sync_pos.opened_at = clock_->now();
                        sync_pos.strategy_id = StrategyId("sync_from_exchange");
                        portfolio_->open_position(sync_pos);
                        logger_->info("pipeline", "Восстановлена фьючерсная нога с биржи",
                            {{"symbol", symbol_.get()},
                             {"side", leg.side == PositionSide::Long ? "long" : "short"},
                             {"size", std::to_string(leg.qty)},
                             {"entry", std::to_string(leg.entry)}});
                    } else {
                        logger_->info("pipeline", "Нога уже в портфеле (recovery)",
                            {{"symbol", symbol_.get()},
                             {"side", leg.side == PositionSide::Long ? "long" : "short"},
                             {"size", std::to_string(leg.qty)}});
                    }
                };

                if (has_long) sync_leg(*long_leg);
                if (has_short) sync_leg(*short_leg);

                if (has_long && has_short) {
                    // Обе ноги присутствуют → это хедж, восстанавливаем hedge state
                    hedge_active_ = true;
                    hedge_already_used_ = true;

                    // Определяем: основная нога — та, что была первой (с большей позицией
                    // или long по умолчанию). Хедж — противоположная.
                    // Если размеры равны — принимаем long за основную.
                    if (long_leg->qty >= short_leg->qty) {
                        current_position_side_ = PositionSide::Long;
                        hedge_position_side_ = PositionSide::Short;
                        hedge_entry_price_ = short_leg->entry;
                        hedge_size_ = short_leg->qty;
                        hedge_entry_time_ns_ = clock_->now().get();
                        // trailing state по основной (long) ноге
                        reset_trailing_state();
                        highest_price_since_entry_ = long_leg->entry;
                        lowest_price_since_entry_ = long_leg->entry;
                        position_entry_time_ns_ = clock_->now().get();
                        initial_position_size_ = long_leg->qty;
                    } else {
                        current_position_side_ = PositionSide::Short;
                        hedge_position_side_ = PositionSide::Long;
                        hedge_entry_price_ = long_leg->entry;
                        hedge_size_ = long_leg->qty;
                        hedge_entry_time_ns_ = clock_->now().get();
                        reset_trailing_state();
                        highest_price_since_entry_ = short_leg->entry;
                        lowest_price_since_entry_ = short_leg->entry;
                        position_entry_time_ns_ = clock_->now().get();
                        initial_position_size_ = short_leg->qty;
                    }

                    logger_->warn("pipeline", "HEDGE STATE ВОССТАНОВЛЕН после рестарта",
                        {{"symbol", symbol_.get()},
                         {"main_side", current_position_side_ == PositionSide::Long ? "long" : "short"},
                         {"hedge_side", hedge_position_side_ == PositionSide::Long ? "long" : "short"},
                         {"long_qty", std::to_string(long_leg->qty)},
                         {"short_qty", std::to_string(short_leg->qty)}});

                } else if (has_long || has_short) {
                    // Одна нога — обычная позиция, хедж не активен
                    const auto& leg = has_long ? *long_leg : *short_leg;
                    current_position_side_ = leg.side;
                    hedge_active_ = false;

                    reset_trailing_state();
                    highest_price_since_entry_ = leg.entry;
                    lowest_price_since_entry_ = leg.entry;
                    position_entry_time_ns_ = clock_->now().get();
                    initial_position_size_ = leg.qty;

                    logger_->info("pipeline", "Синхронизация position_side с биржей",
                        {{"symbol", symbol_.get()},
                         {"side", leg.side == PositionSide::Long ? "long" : "short"},
                         {"size", std::to_string(leg.qty)}});
                } else {
                    // Нет позиций на бирже — очистить фантомы из портфеля
                    auto phantom_long = portfolio_->get_position(symbol_, PositionSide::Long);
                    auto phantom_short = portfolio_->get_position(symbol_, PositionSide::Short);
                    if (phantom_long.has_value()) {
                        logger_->warn("pipeline", "Фантомная long позиция — очистка",
                            {{"symbol", symbol_.get()},
                             {"size", std::to_string(phantom_long->size.get())}});
                        portfolio_->close_position(symbol_, PositionSide::Long, Price(0.0), 0.0);
                    }
                    if (phantom_short.has_value()) {
                        logger_->warn("pipeline", "Фантомная short позиция — очистка",
                            {{"symbol", symbol_.get()},
                             {"size", std::to_string(phantom_short->size.get())}});
                        portfolio_->close_position(symbol_, PositionSide::Short, Price(0.0), 0.0);
                    }
                }
            }
        }

    } catch (const std::exception& e) {
        logger_->warn("pipeline", "Ошибка парсинга баланса",
            {{"error", std::string(e.what())}});
    }
}

// ==================== Загрузка исторических свечей ====================

void TradingPipeline::bootstrap_historical_candles() {
    // Профессиональная загрузка истории для краткосрочной торговли:
    // 1. 200 минутных свечей (~3.3 часа) → прогрев всех индикаторов (ADX, EMA50, RSI, MACD)
    // 2. Затем вызываем bootstrap_htf_candles() для 7-дневной истории часовых свечей
    //
    // Bitget API: GET /api/v2/mix/market/candles
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
    std::string path = "/api/v2/mix/market/candles";

    // --- Шаг 1: 200 минутных свечей для прогрева индикаторов ---
    logger_->info("pipeline", "Загрузка 200 минутных свечей...");
    std::string granularity_1m = "1m";
    std::string query_1m = "symbol=" + symbol_.get()
        + "&granularity=" + granularity_1m + "&limit=200"
        + "&productType=" + config_.futures.product_type;
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
    std::string path = "/api/v2/mix/market/candles";
    std::string granularity_htf = "1H";
    std::string query = "symbol=" + symbol_.get()
        + "&granularity=" + granularity_htf + "&limit=200"
        + "&productType=" + config_.futures.product_type;

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
    // Вычисляем индикаторы на часовом таймфрейме через IndicatorEngine.
    // Это "старший фильтр" — определяет общее направление рынка.

    const size_t n = closes.size();
    if (n < 51) return;

    // --- EMA20 и EMA50 через IndicatorEngine ---
    {
        auto ema20 = indicator_engine_->ema(closes, 20);
        auto ema50 = indicator_engine_->ema(closes, 50);
        if (ema20.valid) htf_ema_20_ = ema20.value;
        if (ema50.valid) htf_ema_50_ = ema50.value;
    }

    // --- RSI(14) через IndicatorEngine ---
    {
        auto rsi = indicator_engine_->rsi(closes, 14);
        if (rsi.valid) htf_rsi_14_ = rsi.value;
    }

    // --- MACD(12,26,9) через IndicatorEngine ---
    {
        auto macd = indicator_engine_->macd(closes, 12, 26, 9);
        if (macd.valid) htf_macd_histogram_ = macd.histogram;
    }

    // --- ADX(14) через IndicatorEngine с реальными highs/lows ---
    {
        bool have_hlc = (htf_highs_buffer_.size() == n && htf_lows_buffer_.size() == n);
        if (have_hlc) {
            auto adx = indicator_engine_->adx(htf_highs_buffer_, htf_lows_buffer_, closes, 14);
            if (adx.valid) htf_adx_ = adx.adx;
        } else {
            // Fallback: closes-only прокси через ATR-подобный расчёт
            // ADX без highs/lows некорректен — ставим консервативное значение
            htf_adx_ = 20.0;
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

    // 4. HTF тренд — информационное (стратегия сама фильтрует направление)
    // Не блокируем market_ready: шорты при даунтренде, лонги при аптренде допустимы
    if (tick_count_ % 2000 == 0 && htf_trend_strength_ > 0.5) {
        logger_->info("pipeline",
            "HTF тренд: направленный",
            {{"htf_trend", htf_trend_direction_ == 1 ? "UP" :
                          htf_trend_direction_ == -1 ? "DOWN" : "SIDEWAYS"},
             {"htf_strength", std::to_string(htf_trend_strength_)},
             {"htf_rsi", std::to_string(htf_rsi_14_)}});
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
    lowest_price_since_entry_ = std::numeric_limits<double>::max();
    current_stop_level_ = 0.0;
    breakeven_activated_ = false;
    partial_tp_taken_ = false;
    close_order_pending_ = false;
    initial_position_size_ = 0.0;
    current_trail_mult_ = config_.trading_params.atr_stop_multiplier;
    position_entry_time_ns_ = 0;
    hedge_already_used_ = false;
}

void TradingPipeline::reset_hedge_state() {
    hedge_active_ = false;
    hedge_entry_price_ = 0.0;
    hedge_size_ = 0.0;
    hedge_entry_time_ns_ = 0;
    original_loss_at_hedge_ = 0.0;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Индикаторная оценка рыночной ситуации для конкретной стороны позиции
// Возвращает score [-1.0 .. +1.0]:
//   > 0 = рынок благоприятен для этой стороны (стоит держать)
//   < 0 = рынок против (стоит закрывать)
// Используется для hedge management и smart exit decisions
// ═══════════════════════════════════════════════════════════════════════════════
double TradingPipeline::evaluate_exit_score(const features::FeatureSnapshot& snapshot,
                                             PositionSide for_side) const {
    double score = 0.0;
    int signals = 0;
    const bool is_long = (for_side == PositionSide::Long);

    // ── 1. Momentum (вес 0.25) ─────────────────────────────────────────
    // Скорость и направление ценового движения — ключевой индикатор для скальпинга
    if (snapshot.technical.momentum_valid) {
        double mom = snapshot.technical.momentum_5;
        double mom_signal = is_long ? mom : -mom;
        // Нормализация: ±0.005 = сильный сигнал
        score += std::clamp(mom_signal / 0.005, -1.0, 1.0) * 0.25;
        ++signals;
    }

    // ── 2. RSI (вес 0.15) ──────────────────────────────────────────────
    // Перекупленность/перепроданность — сигнал разворота
    if (snapshot.technical.rsi_valid) {
        double rsi = snapshot.technical.rsi_14;
        if (is_long) {
            // Long: RSI > 70 → перекупленность → вероятен откат, RSI < 40 → слабость
            if (rsi > 70.0) score += -0.15;
            else if (rsi > 60.0) score += 0.05;
            else if (rsi < 35.0) score += -0.10;
            else score += 0.08;
        } else {
            // Short: RSI < 30 → перепроданность → вероятен отскок, RSI > 60 → сила
            if (rsi < 30.0) score += -0.15;
            else if (rsi < 40.0) score += 0.05;
            else if (rsi > 65.0) score += -0.10;
            else score += 0.08;
        }
        ++signals;
    }

    // ── 3. EMA тренд (вес 0.20) ───────────────────────────────────────
    // Краткосрочный тренд: EMA20 vs EMA50
    if (snapshot.technical.ema_valid) {
        bool ema_bullish = snapshot.technical.ema_20 > snapshot.technical.ema_50;
        double ema_gap_pct = 0.0;
        if (snapshot.technical.ema_50 > 0.0) {
            ema_gap_pct = (snapshot.technical.ema_20 - snapshot.technical.ema_50)
                        / snapshot.technical.ema_50;
        }
        double ema_signal = is_long ? ema_gap_pct : -ema_gap_pct;
        score += std::clamp(ema_signal * 50.0, -1.0, 1.0) * 0.20;
        ++signals;
    }

    // ── 4. MACD гистограмма (вес 0.15) ────────────────────────────────
    // Ускорение/замедление тренда
    if (snapshot.technical.macd_valid) {
        double macd_h = snapshot.technical.macd_histogram;
        double macd_signal = is_long ? macd_h : -macd_h;
        // Нормализация через ATR
        double norm = (snapshot.technical.atr_valid && snapshot.technical.atr_14 > 0.0)
            ? snapshot.technical.atr_14 * 0.1 : 1.0;
        score += std::clamp(macd_signal / norm, -1.0, 1.0) * 0.15;
        ++signals;
    }

    // ── 5. ADX + DI (вес 0.10) ────────────────────────────────────────
    // Сила тренда и его направление
    if (snapshot.technical.adx_valid) {
        double adx = snapshot.technical.adx;
        bool trend_strong = adx > 25.0;
        bool di_favorable = is_long
            ? (snapshot.technical.plus_di > snapshot.technical.minus_di)
            : (snapshot.technical.minus_di > snapshot.technical.plus_di);

        if (trend_strong && di_favorable) score += 0.10;
        else if (trend_strong && !di_favorable) score += -0.10;
        else score += 0.0;  // Слабый тренд — нейтрально
        ++signals;
    }

    // ── 6. Order Book Imbalance (вес 0.10) ────────────────────────────
    // Давление покупателей vs продавцов
    if (snapshot.microstructure.book_imbalance_valid) {
        double imb = snapshot.microstructure.book_imbalance_5;
        double imb_signal = is_long ? imb : -imb;
        score += std::clamp(imb_signal * 2.0, -1.0, 1.0) * 0.10;
        ++signals;
    }

    // ── 7. VPIN токсичность (вес 0.05) ────────────────────────────────
    // Высокий VPIN = информированные трейдеры → опасно
    if (snapshot.microstructure.vpin_valid && snapshot.microstructure.vpin_toxic) {
        score -= 0.05;
        ++signals;
    }

    // Если нет индикаторов — нейтральный результат
    if (signals == 0) return 0.0;

    return std::clamp(score, -1.0, 1.0);
}

bool TradingPipeline::close_hedge_leg(const features::FeatureSnapshot& snapshot,
                                       PositionSide leg_side, double qty,
                                       const std::string& reason) {
    int64_t now_ns = clock_->now().get();
    Side close_side = (leg_side == PositionSide::Long) ? Side::Sell : Side::Buy;

    strategy::TradeIntent close_intent;
    close_intent.symbol = symbol_;
    close_intent.side = close_side;
    close_intent.conviction = 1.0;
    close_intent.urgency = 1.0;
    close_intent.strategy_id = StrategyId("hedge_recovery");
    close_intent.limit_price = snapshot.mid_price;
    close_intent.snapshot_mid_price = snapshot.mid_price;
    close_intent.suggested_quantity = Quantity(qty);
    close_intent.trade_side = TradeSide::Close;
    close_intent.position_side = leg_side;
    close_intent.signal_intent = (leg_side == PositionSide::Long)
        ? strategy::SignalIntent::LongExit : strategy::SignalIntent::ShortExit;
    close_intent.correlation_id = CorrelationId(
        "HEDGE-" + std::string(leg_side == PositionSide::Long ? "L" : "S") + "-" + std::to_string(now_ns));

    risk::RiskDecision rd;
    rd.decided_at = clock_->now();
    rd.approved_quantity = Quantity(qty);
    rd.verdict = risk::RiskVerdict::Approved;
    rd.summary = reason;

    execution_alpha::ExecutionAlphaResult ea;
    ea.should_execute = true;
    ea.recommended_style = execution_alpha::ExecutionStyle::Aggressive;
    ea.urgency_score = 1.0;
    ea.rationale = "HEDGE_RECOVERY: " + reason;

    auto r = execution_engine_->execute(close_intent, rd, ea, uncertainty::UncertaintySnapshot{});
    if (r) {
        risk_engine_->record_order_sent();
        logger_->info("pipeline", "HEDGE: нога закрыта",
            {{"order_id", r->get()},
             {"side", leg_side == PositionSide::Long ? "Long" : "Short"},
             {"reason", reason},
             {"symbol", symbol_.get()}});
        return true;
    }
    return false;
}

// ═══════════════════════════════════════════════════════════════════════════════
// HEDGE RECOVERY — профессиональное хеджирование с индикаторным анализом
//
// Стратегия (Lock → Analyze → Release):
// 1. Позиция в убытке + индикаторы подтверждают тренд против → открыть хедж (lock)
// 2. Мониторинг обеих ног: индикаторы показывают разворот → закрыть убыточную ногу
// 3. Прибыльная нога растёт → закрыть по обычному trailing/TP
//
// Отличие от примитивного хеджа:
// - Открытие: требуется подтверждение 3+ индикаторов что тренд против
// - Управление: поэтапное закрытие ног по индикаторам (не обе сразу)
// - Таймаут: аварийный, не основной механизм выхода
// ═══════════════════════════════════════════════════════════════════════════════
bool TradingPipeline::check_hedge_recovery(const features::FeatureSnapshot& snapshot) {
    if (!config_.trading_params.hedge_recovery_enabled) return false;

    auto port_snap = portfolio_->snapshot();
    if (port_snap.positions.empty()) {
        if (hedge_active_) reset_hedge_state();
        return false;
    }

    // Найти обе ноги
    const portfolio::Position* original_pos = nullptr;
    const portfolio::Position* hedge_pos = nullptr;
    for (const auto& pos : port_snap.positions) {
        if (pos.symbol.get() != symbol_.get()) continue;
        if (pos.avg_entry_price.get() <= 0.0 || pos.current_price.get() <= 0.0) continue;

        bool is_long_side = (pos.side == Side::Buy);
        bool is_original = (is_long_side && current_position_side_ == PositionSide::Long)
                        || (!is_long_side && current_position_side_ == PositionSide::Short);

        if (hedge_active_) {
            if (is_original) original_pos = &pos;
            else hedge_pos = &pos;
        } else {
            if (is_original) original_pos = &pos;
        }
    }

    if (!original_pos) {
        if (hedge_active_) reset_hedge_state();
        return false;
    }

    int64_t now_ns = clock_->now().get();
    double price = snapshot.mid_price.get();

    // ═══════════════════════════════════════════════════════════════════
    // ФАЗА 2: УПРАВЛЕНИЕ ХЕДЖЕМ (Lock активен)
    // Индикаторный выход: оцениваем каждую ногу отдельно
    // ═══════════════════════════════════════════════════════════════════
    if (hedge_active_) {
        if (close_order_pending_) return true;

        double original_pnl = original_pos->unrealized_pnl;
        double hedge_pnl = hedge_pos ? hedge_pos->unrealized_pnl : 0.0;
        double net_pnl = original_pnl + hedge_pnl;

        double total_notional = original_pos->size.get() * price;
        if (hedge_pos) total_notional += hedge_pos->size.get() * price;
        double round_trip_fees = total_notional * common::fees::kDefaultTakerFeePct * 2.0;
        double min_profit = round_trip_fees * config_.trading_params.hedge_profit_close_fee_mult;

        int64_t hedge_hold_ns = now_ns - hedge_entry_time_ns_;
        int64_t max_hedge_ns = static_cast<int64_t>(config_.trading_params.hedge_max_hold_minutes)
                             * 60LL * 1'000'000'000LL;

        // Оценка рыночной ситуации для обеих ног
        double original_score = evaluate_exit_score(snapshot, current_position_side_);
        double hedge_score = evaluate_exit_score(snapshot, hedge_position_side_);

        // ── Стратегия 1: Чистая прибыль покрывает комиссии → закрыть обе ──
        if (net_pnl > min_profit) {
            logger_->warn("pipeline", "HEDGE: чистая прибыль — закрытие обеих ног",
                {{"net_pnl", std::to_string(net_pnl)},
                 {"min_profit", std::to_string(min_profit)},
                 {"symbol", symbol_.get()}});

            close_hedge_leg(snapshot, current_position_side_, original_pos->size.get(), "HEDGE_PROFIT_ORIG");
            if (hedge_pos) {
                close_hedge_leg(snapshot, hedge_position_side_, hedge_pos->size.get(), "HEDGE_PROFIT_HEDGE");
            }
            for (const auto& s : strategy_registry_->active()) s->notify_position_closed();
            close_order_pending_ = true;
            last_order_time_ns_ = now_ns;
            last_activity_ns_.store(now_ns, std::memory_order_relaxed);
            reset_hedge_state();
            reset_trailing_state();
            return true;
        }

        // ── Стратегия 2: Индикаторы показали разворот → закрыть убыточную ногу ──
        // Если score для оригинальной позиции стал положительным (> +0.15),
        // значит рынок развернулся в нашу сторону → закрываем хедж-ногу (она убыточная)
        // и оставляем оригинальную — она должна восстановиться.
        if (original_score > 0.15 && hedge_pos && hedge_hold_ns > 5'000'000'000LL) {
            // Дополнительная проверка: momentum реально развернулся в нашу сторону
            bool momentum_confirms = false;
            if (snapshot.technical.momentum_valid) {
                bool is_orig_long = (current_position_side_ == PositionSide::Long);
                momentum_confirms = is_orig_long
                    ? (snapshot.technical.momentum_5 > 0.001)
                    : (snapshot.technical.momentum_5 < -0.001);
            }

            if (momentum_confirms) {
                logger_->warn("pipeline", "HEDGE: разворот по индикаторам — закрытие хедж-ноги",
                    {{"original_score", std::to_string(original_score)},
                     {"hedge_score", std::to_string(hedge_score)},
                     {"hedge_pnl", std::to_string(hedge_pnl)},
                     {"momentum", std::to_string(snapshot.technical.momentum_5)},
                     {"rsi", std::to_string(snapshot.technical.rsi_14)},
                     {"symbol", symbol_.get()}});

                close_hedge_leg(snapshot, hedge_position_side_, hedge_pos->size.get(), "HEDGE_REVERSAL_CLOSE");
                close_order_pending_ = true;
                last_order_time_ns_ = now_ns;
                last_activity_ns_.store(now_ns, std::memory_order_relaxed);
                reset_hedge_state();
                // НЕ сбрасываем trailing — оригинальная позиция продолжает жить
                return false;  // Позволяем обычному stop-loss управлять оставшейся ногой
            }
        }

        // ── Стратегия 3: Индикаторы ещё сильнее против оригинальной → закрыть её ──
        // Если hedge_score > +0.15 и original_score < -0.15,
        // рынок ушёл ещё дальше → закрываем оригинальную (убыточную),
        // оставляем хедж-ногу (прибыльную) работать.
        if (hedge_score > 0.15 && original_score < -0.15 && hedge_pos
            && hedge_hold_ns > 10'000'000'000LL) {
            logger_->warn("pipeline", "HEDGE: тренд усилился — закрытие оригинальной ноги",
                {{"original_score", std::to_string(original_score)},
                 {"hedge_score", std::to_string(hedge_score)},
                 {"original_pnl", std::to_string(original_pnl)},
                 {"symbol", symbol_.get()}});

            close_hedge_leg(snapshot, current_position_side_, original_pos->size.get(), "HEDGE_TREND_CUT_LOSS");
            // Хедж-нога становится новой "основной"
            current_position_side_ = hedge_position_side_;
            close_order_pending_ = true;
            last_order_time_ns_ = now_ns;
            last_activity_ns_.store(now_ns, std::memory_order_relaxed);
            reset_hedge_state();
            // Пересчитываем trailing для новой (хедж) позиции
            highest_price_since_entry_ = price;
            lowest_price_since_entry_ = price;
            position_entry_time_ns_ = hedge_entry_time_ns_;
            return false;  // Обычный stop-loss для оставшейся ноги
        }

        // ── Стратегия 4: Аварийный таймаут — закрыть обе ноги ──
        if (hedge_hold_ns >= max_hedge_ns) {
            logger_->warn("pipeline", "HEDGE TIMEOUT — аварийное закрытие обеих ног",
                {{"hold_min", std::to_string(hedge_hold_ns / 60'000'000'000LL)},
                 {"net_pnl", std::to_string(net_pnl)},
                 {"original_score", std::to_string(original_score)},
                 {"hedge_score", std::to_string(hedge_score)},
                 {"symbol", symbol_.get()}});

            close_hedge_leg(snapshot, current_position_side_, original_pos->size.get(), "HEDGE_TIMEOUT_ORIG");
            if (hedge_pos) {
                close_hedge_leg(snapshot, hedge_position_side_, hedge_pos->size.get(), "HEDGE_TIMEOUT_HEDGE");
            }
            for (const auto& s : strategy_registry_->active()) s->notify_position_closed();
            close_order_pending_ = true;
            last_order_time_ns_ = now_ns;
            last_activity_ns_.store(now_ns, std::memory_order_relaxed);
            reset_hedge_state();
            reset_trailing_state();
            return true;
        }

        // Логирование состояния хеджа каждые 100 тиков
        if (tick_count_ % 100 == 0) {
            logger_->info("pipeline", "HEDGE STATUS",
                {{"original_pnl", std::to_string(original_pnl)},
                 {"hedge_pnl", std::to_string(hedge_pnl)},
                 {"net_pnl", std::to_string(net_pnl)},
                 {"original_score", std::to_string(original_score)},
                 {"hedge_score", std::to_string(hedge_score)},
                 {"hold_sec", std::to_string(hedge_hold_ns / 1'000'000'000LL)},
                 {"symbol", symbol_.get()}});
        }

        return true;  // Хедж активен — блокировать обычный stop-loss
    }

    // ═══════════════════════════════════════════════════════════════════
    // ФАЗА 1: ПРИНЯТИЕ РЕШЕНИЯ О ХЕДЖИРОВАНИИ
    // Не просто по убытку — требуется подтверждение индикаторов
    // ═══════════════════════════════════════════════════════════════════
    if (close_order_pending_) return false;
    if (hedge_already_used_) return false;  // Не хеджируем повторно

    double unrealized = original_pos->unrealized_pnl;
    if (unrealized >= 0.0) return false;

    double loss_pct_of_capital = std::abs(unrealized) / std::max(port_snap.total_capital, 0.01) * 100.0;
    if (loss_pct_of_capital < config_.trading_params.hedge_trigger_loss_pct) return false;

    // Минимальное время удержания
    int64_t hold_ns = (position_entry_time_ns_ > 0) ? (now_ns - position_entry_time_ns_) : 0;
    int64_t min_hold_ns = static_cast<int64_t>(config_.trading_params.hedge_min_hold_seconds)
                        * 1'000'000'000LL;
    if (hold_ns < min_hold_ns) return false;

    // ── ИНДИКАТОРНАЯ ПРОВЕРКА: тренд действительно против нас? ──
    // Оцениваем ситуацию для нашей стороны. Если score < -0.15,
    // значит минимум 3 индикатора показывают что рынок против.
    double our_score = evaluate_exit_score(snapshot, current_position_side_);

    if (our_score >= -0.10) {
        // Индикаторы не подтверждают — возможно временный откат, не хеджируем
        if (tick_count_ % 200 == 0) {
            logger_->debug("pipeline", "HEDGE: убыток есть, но индикаторы не подтверждают",
                {{"loss_pct", std::to_string(loss_pct_of_capital)},
                 {"exit_score", std::to_string(our_score)},
                 {"symbol", symbol_.get()}});
        }
        return false;
    }

    // ═══════════════════════════════════════════════════════════════════
    // ОТКРЫТИЕ ХЕДЖА: индикаторы подтвердили — тренд против нас
    // ═══════════════════════════════════════════════════════════════════
    hedge_position_side_ = (current_position_side_ == PositionSide::Long)
        ? PositionSide::Short : PositionSide::Long;
    Side hedge_side = (hedge_position_side_ == PositionSide::Long) ? Side::Buy : Side::Sell;
    double hedge_qty = original_pos->size.get();

    double hedge_notional = hedge_qty * price;
    const double min_notional = exchange_rules_.min_trade_usdt > 0.0
        ? exchange_rules_.min_trade_usdt
        : common::exchange_limits::kMinBitgetNotionalUsdt;
    if (hedge_notional < min_notional) return false;

    // ИСПРАВЛЕНИЕ H4: синхронизация плеча для хедж-ноги перед ордером.
    // В Bitget hedge-mode плечо задаётся PER-SIDE (long/short отдельно).
    // Если плечо хедж-стороны != текущего — биржа может отклонить ордер или
    // использовать неверный margin → неконтролируемый риск ликвидации.
    if (futures_submitter_) {
        std::string hedge_hold_side = (hedge_position_side_ == PositionSide::Long)
            ? "long" : "short";
        int& hedge_last_applied = (hedge_position_side_ == PositionSide::Long)
            ? last_applied_leverage_long_ : last_applied_leverage_short_;
        // Для хеджа используем минимальное плечо из конфига — консервативно.
        // Хедж — защитная позиция, не спекулятивная.
        int hedge_lev = std::max(1, config_.futures.min_leverage);
        if (leverage_engine_) {
            // Используем текущее плечо основной позиции как потолок
            auto main_hold_side = (current_position_side_ == PositionSide::Long)
                ? "long" : "short";
            hedge_lev = std::max(1, last_applied_leverage_long_);
            if (current_position_side_ == PositionSide::Short) {
                hedge_lev = std::max(1, last_applied_leverage_short_);
            }
        }
        if (hedge_lev != hedge_last_applied) {
            bool set_ok = futures_submitter_->set_leverage(symbol_, hedge_lev, hedge_hold_side);
            if (!set_ok) {
                logger_->error("pipeline",
                    "HEDGE: Не удалось установить плечо для хедж-ноги — пропуск хеджа",
                    {{"leverage", std::to_string(hedge_lev)},
                     {"hold_side", hedge_hold_side},
                     {"symbol", symbol_.get()}});
                return false;
            }
            hedge_last_applied = hedge_lev;
            logger_->info("pipeline", "HEDGE: плечо синхронизировано для хедж-ноги",
                {{"leverage", std::to_string(hedge_lev)},
                 {"hold_side", hedge_hold_side},
                 {"symbol", symbol_.get()}});
        }
    }

    logger_->warn("pipeline", "HEDGE RECOVERY — индикаторы подтвердили, открытие хеджа",
        {{"loss_pct", std::to_string(loss_pct_of_capital)},
         {"exit_score", std::to_string(our_score)},
         {"momentum", std::to_string(snapshot.technical.momentum_5)},
         {"rsi", std::to_string(snapshot.technical.rsi_14)},
         {"adx", std::to_string(snapshot.technical.adx)},
         {"macd_h", std::to_string(snapshot.technical.macd_histogram)},
         {"hedge_side", hedge_side == Side::Buy ? "BUY" : "SELL"},
         {"hedge_qty", std::to_string(hedge_qty)},
         {"symbol", symbol_.get()}});

    strategy::TradeIntent hedge_intent;
    hedge_intent.symbol = symbol_;
    hedge_intent.side = hedge_side;
    hedge_intent.conviction = 1.0;
    hedge_intent.urgency = 1.0;
    hedge_intent.strategy_id = StrategyId("hedge_recovery");
    hedge_intent.limit_price = snapshot.mid_price;
    hedge_intent.snapshot_mid_price = snapshot.mid_price;
    hedge_intent.suggested_quantity = Quantity(hedge_qty);
    hedge_intent.trade_side = TradeSide::Open;
    hedge_intent.position_side = hedge_position_side_;
    hedge_intent.signal_intent = (hedge_position_side_ == PositionSide::Long)
        ? strategy::SignalIntent::LongEntry : strategy::SignalIntent::ShortEntry;
    hedge_intent.correlation_id = CorrelationId("HEDGE-" + std::to_string(now_ns));

    risk::RiskDecision rd;
    rd.decided_at = clock_->now();
    rd.approved_quantity = Quantity(hedge_qty);
    rd.verdict = risk::RiskVerdict::Approved;
    rd.summary = "HEDGE_OPEN: loss=" + std::to_string(loss_pct_of_capital)
               + "% score=" + std::to_string(our_score);

    execution_alpha::ExecutionAlphaResult ea;
    ea.should_execute = true;
    ea.recommended_style = execution_alpha::ExecutionStyle::Aggressive;
    ea.urgency_score = 1.0;
    ea.rationale = "HEDGE_RECOVERY: confirmed by indicators";

    last_order_time_ns_ = now_ns;
    last_activity_ns_.store(now_ns, std::memory_order_relaxed);

    auto order_result = execution_engine_->execute(hedge_intent, rd, ea, uncertainty::UncertaintySnapshot{});
    if (order_result) {
        hedge_active_ = true;
        hedge_already_used_ = true;
        hedge_entry_price_ = price;
        hedge_size_ = hedge_qty;
        hedge_entry_time_ns_ = now_ns;
        original_loss_at_hedge_ = unrealized;
        risk_engine_->record_order_sent();

        logger_->warn("pipeline", "HEDGE ОРДЕР ОТПРАВЛЕН",
            {{"order_id", order_result->get()},
             {"hedge_side", hedge_side == Side::Buy ? "BUY" : "SELL"},
             {"qty", std::to_string(hedge_qty)},
             {"exit_score", std::to_string(our_score)},
             {"symbol", symbol_.get()}});
        return true;
    }
    logger_->error("pipeline", "HEDGE ордер не исполнен", {{"symbol", symbol_.get()}});
    return false;
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
            // Используем байесовски адаптированный базовый множитель, если он доступен
            double base = (ml_snapshot_.adapted_atr_stop_mult > 0.0)
                ? ml_snapshot_.adapted_atr_stop_mult
                : config_.trading_params.atr_stop_multiplier;

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
                // Fee-aware breakeven: не активировать, пока профит не покроет комиссии
                double min_fee_offset = entry * common::fees::kDefaultTakerFeePct * 3.0;
                double fee_floor_in_atr = (atr > 0.0) ? (min_fee_offset / atr) : 100.0;
                double effective_be_threshold = std::max(
                    config_.trading_params.breakeven_atr_threshold,
                    fee_floor_in_atr * 1.5);  // 1.5× комиссии в ATR — буфер

                if (profit_in_atr >= effective_be_threshold) {
                    // Достигли breakeven уровня → активируем trailing
                    breakeven_activated_ = true;
                    // Лочим 50% текущего профита выше entry
                    double current_profit = price - entry;
                    double breakeven_level = entry + current_profit * 0.5;
                    // Fee-aware floor: стоп минимум покрывает round-trip комиссии
                    breakeven_level = std::max(breakeven_level, entry + min_fee_offset);
                    // Гарантируем: стоп минимум на 0.1 ATR выше entry (пропорционально для любой цены)
                    breakeven_level = std::max(breakeven_level, entry + atr * 0.1);
                    // Не ставить стоп выше текущей цены — иначе мгновенный триггер
                    breakeven_level = std::min(breakeven_level, price - atr * 0.1);
                    current_stop_level_ = breakeven_level;
                    logger_->info("pipeline", "Breakeven + Trailing активирован",
                        {{"entry", std::to_string(entry)},
                         {"stop", std::to_string(breakeven_level)},
                         {"locked_pct", std::to_string((breakeven_level - entry) / entry * 100.0)},
                         {"profit_atr", std::to_string(profit_in_atr)},
                         {"fee_floor", std::to_string(min_fee_offset)},
                         {"be_threshold_atr", std::to_string(effective_be_threshold)},
                         {"symbol", symbol_.get()}});
                } else if (current_stop_level_ <= 0.0) {
                    // Начальный ATR-стоп: защита до активации breakeven
                    // Минимальная дистанция 0.3% от цены — не даём шуму убить позицию
                    double atr_distance = current_trail_mult_ * atr;
                    double min_distance = entry * 0.003;  // 0.3% от entry
                    double stop_distance = std::max(atr_distance, min_distance);
                    current_stop_level_ = entry - stop_distance;
                }
            } else {
                // Фаза 2: После breakeven — классический Chandelier Exit
                double atr_distance = current_trail_mult_ * atr;
                double min_distance = entry * 0.003;  // 0.3% от entry
                double stop_distance = std::max(atr_distance, min_distance);
                double new_stop = highest_price_since_entry_ - stop_distance;
                // Стоп только поднимается
                current_stop_level_ = std::max(current_stop_level_, new_stop);
            }
        } else {
            lowest_price_since_entry_ = std::min(lowest_price_since_entry_, price);
            // Fix: обновляем MAE для шорта на каждом тике
            update_current_mae(price, /*is_long=*/false);
            double profit_in_atr = (entry - price) / atr;

            if (!breakeven_activated_) {
                // Fee-aware breakeven: не активировать, пока профит не покроет комиссии
                double min_fee_offset = entry * common::fees::kDefaultTakerFeePct * 3.0;
                double fee_floor_in_atr = (atr > 0.0) ? (min_fee_offset / atr) : 100.0;
                double effective_be_threshold = std::max(
                    config_.trading_params.breakeven_atr_threshold,
                    fee_floor_in_atr * 1.5);  // 1.5× комиссии в ATR — буфер

                if (profit_in_atr >= effective_be_threshold) {
                    breakeven_activated_ = true;
                    // SHORT: стоп двигаем НИЖЕ entry (цена падает = профит).
                    // breakeven_level < entry, т.к. мы лочим 50% профита.
                    double current_profit = entry - price;
                    double breakeven_level = entry - current_profit * 0.5;
                    // Fee-aware floor: стоп минимум покрывает round-trip комиссии
                    breakeven_level = std::min(breakeven_level, entry - min_fee_offset);
                    // Гарантируем: стоп минимум на 0.1 ATR ниже entry (пропорционально для любой цены)
                    breakeven_level = std::min(breakeven_level, entry - atr * 0.1);
                    // Не ставить стоп ниже текущей цены — иначе мгновенный триггер
                    breakeven_level = std::max(breakeven_level, price + atr * 0.1);
                    current_stop_level_ = breakeven_level;
                    logger_->info("pipeline", "Breakeven + Trailing активирован (SHORT)",
                        {{"entry", std::to_string(entry)},
                         {"stop", std::to_string(breakeven_level)},
                         {"locked_pct", std::to_string((entry - breakeven_level) / entry * 100.0)},
                         {"fee_floor", std::to_string(min_fee_offset)},
                         {"be_threshold_atr", std::to_string(effective_be_threshold)},
                         {"symbol", symbol_.get()}});
                } else if (current_stop_level_ <= 0.0) {
                    // Начальный ATR-стоп: защита до активации breakeven
                    // Минимальная дистанция 0.3% от цены — не даём шуму убить позицию
                    double atr_distance = current_trail_mult_ * atr;
                    double min_distance = entry * 0.003;  // 0.3% от entry
                    double stop_distance = std::max(atr_distance, min_distance);
                    current_stop_level_ = entry + stop_distance;
                }
            } else {
                // После breakeven — Chandelier Exit для SHORT: стоп только опускается
                double atr_distance = current_trail_mult_ * atr;
                double min_distance = entry * 0.003;  // 0.3% от entry
                double stop_distance = std::max(atr_distance, min_distance);
                double new_stop = lowest_price_since_entry_ + stop_distance;
                if (current_stop_level_ <= 0.0) {
                    current_stop_level_ = new_stop;  // Инициализация
                } else {
                    current_stop_level_ = std::min(current_stop_level_, new_stop);  // Только вниз
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
    // 3. Фиксированный стоп (safety net): убыток > max_loss_per_trade_pct капитала → экстренное закрытие.
    //
    // Trailing stop — основной механизм выхода, фиксированный — страховка на случай сбоя.

    // Guard: если ордер на закрытие уже в процессе — не отправлять повторный стоп-лосс.
    // Без этого каждый тик до прихода fill'а генерирует дублирующий ордер.
    if (close_order_pending_) {
        return true;  // Стоп уже отправлен, блокируем дальнейшую обработку тика
    }

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

        // === 0. QUICK PROFIT EXIT: прибыль > комиссии + индикаторная проверка ===
        // Фиксируем профит, но НЕ если индикаторы сильно благоприятны (дать расти)
        bool quick_profit_triggered = false;
        {
            double unrealized = pos.unrealized_pnl;
            int64_t now_check = clock_->now().get();
            int64_t hold_ns = (position_entry_time_ns_ > 0) ? (now_check - position_entry_time_ns_) : 0;
            const int64_t min_hold_for_qp = static_cast<int64_t>(config_.trading_params.quick_profit_min_hold_seconds)
                                          * 1'000'000'000LL;
            double notional = pos.size.get() * price;
            double round_trip_fee = notional * common::fees::kDefaultTakerFeePct * 2.0;
            double min_profit = round_trip_fee * config_.trading_params.quick_profit_fee_multiplier;
            if (unrealized > min_profit && hold_ns >= min_hold_for_qp) {
                // Индикаторная проверка: если рынок сильно за нас (score > +0.3),
                // не фиксируем раннюю прибыль — дать позиции расти
                double exit_score = evaluate_exit_score(snapshot, current_position_side_);
                if (exit_score > 0.30) {
                    // Индикаторы показывают сильный тренд в нашу сторону — не фиксируем
                    if (tick_count_ % 200 == 0) {
                        logger_->info("pipeline", "QUICK_PROFIT отложен — индикаторы благоприятны",
                            {{"exit_score", std::to_string(exit_score)},
                             {"pnl", std::to_string(unrealized)},
                             {"symbol", symbol_.get()}});
                    }
                } else {
                    quick_profit_triggered = true;
                }
            }
        }

        // === 1. Trailing Stop (Chandelier Exit) ===
        bool stop_level_crossed = false;
        if (current_stop_level_ > 0.0) {
            if (pos.side == Side::Buy) {
                stop_level_crossed = price <= current_stop_level_;
            } else {
                stop_level_crossed = price >= current_stop_level_;
            }
        }
        const bool trailing_stop_triggered = breakeven_activated_ && stop_level_crossed;
        const bool protective_atr_stop_triggered = !breakeven_activated_ && stop_level_crossed;

        // === 2. Partial Take-Profit: профит >= N×ATR → закрыть позицию ===
        bool partial_tp_triggered = false;
        if (!partial_tp_taken_ && snapshot.technical.atr_valid && snapshot.technical.atr_14 > 0.0) {
            double atr = snapshot.technical.atr_14;
            double profit_in_atr = (pos.side == Side::Buy)
                ? (price - entry) / atr
                : (entry - price) / atr;
            if (profit_in_atr >= config_.trading_params.partial_tp_atr_threshold) {
                // Fee-aware guard: don't TP if gross profit < 1.5× round-trip fees
                double est_notional = pos.size.get() * price;
                double round_trip_fee = est_notional * common::fees::kDefaultTakerFeePct * 2.0;
                if (pos.unrealized_pnl >= round_trip_fee * 1.5) {
                    partial_tp_triggered = true;
                } else {
                    logger_->debug("pipeline", "TP отложен: профит < 1.5× комиссии",
                        {{"profit", std::to_string(pos.unrealized_pnl)},
                         {"min_required", std::to_string(round_trip_fee * 1.5)},
                         {"symbol", symbol_.get()}});
                }
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
            // На фьючерсах определяем по сохранённой стороне позиции
            bool is_long_position = (current_position_side_ == PositionSide::Long);
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

        // === 4. Time-based exit: индикаторный анализ при таймауте ===
        // Вместо простого momentum_5 используем полный evaluate_exit_score
        bool time_exit_triggered = false;
        if (!trailing_stop_triggered && !protective_atr_stop_triggered
            && !fixed_stop_triggered && !price_stop_triggered
            && position_entry_time_ns_ > 0) {
            int64_t now_check = clock_->now().get();
            int64_t hold_duration = now_check - position_entry_time_ns_;
            int64_t max_hold_loss = static_cast<int64_t>(config_.trading_params.max_hold_loss_minutes)
                                 * 60LL * 1'000'000'000LL;
            int64_t max_hold_abs = static_cast<int64_t>(config_.trading_params.max_hold_absolute_minutes)
                                 * 60LL * 1'000'000'000LL;
            int64_t hold_min = hold_duration / 60'000'000'000LL;

            // Полная индикаторная оценка ситуации для нашей стороны
            double exit_score = evaluate_exit_score(snapshot, current_position_side_);
            bool indicators_favorable = (exit_score > 0.10);  // Если > +0.10, рынок за нас

            // Убыточная позиция > N мин → закрыть (only if loss > 0.3% of entry)
            double loss_pct_of_entry = (entry > 0.0 && pos.size.get() > 0.0)
                ? std::abs(pos.unrealized_pnl) / (entry * pos.size.get()) * 100.0
                : 0.0;
            if (pos.unrealized_pnl < 0.0 && loss_pct_of_entry > 0.3 && hold_duration >= max_hold_loss) {
                // Если индикаторы показывают разворот в нашу сторону — подождать
                if (indicators_favorable && hold_duration < max_hold_loss * 3 / 2) {
                    if (tick_count_ % 200 == 0) {
                        logger_->info("pipeline", "TIME_EXIT отложен — индикаторы благоприятны",
                            {{"hold_min", std::to_string(hold_min)},
                             {"exit_score", std::to_string(exit_score)},
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
                         {"exit_score", std::to_string(exit_score)},
                         {"symbol", symbol_.get()}});
                }
            }
            // Абсолютный таймаут > M мин → закрыть (extend if profitable + indicators ok)
            if (!time_exit_triggered && hold_duration >= max_hold_abs) {
                if (pos.unrealized_pnl > 0.0 && indicators_favorable && hold_duration < max_hold_abs * 3 / 2) {
                    if (tick_count_ % 200 == 0) {
                        logger_->info("pipeline", "TIME_EXIT абсолютный отложен — прибыльная + индикаторы ОК",
                            {{"hold_min", std::to_string(hold_min)},
                             {"exit_score", std::to_string(exit_score)},
                             {"pnl", std::to_string(pos.unrealized_pnl)},
                             {"symbol", symbol_.get()}});
                    }
                } else {
                    time_exit_triggered = true;
                    logger_->warn("pipeline", "TIME_EXIT: абсолютный таймаут позиции",
                        {{"hold_min", std::to_string(hold_min)},
                         {"max_hold_abs_min", std::to_string(config_.trading_params.max_hold_absolute_minutes)},
                         {"pnl", std::to_string(pos.unrealized_pnl)},
                         {"exit_score", std::to_string(exit_score)},
                         {"symbol", symbol_.get()}});
                }
            }
        }

        if (!trailing_stop_triggered && !protective_atr_stop_triggered
            && !partial_tp_triggered && !fixed_stop_triggered
            && !price_stop_triggered && !time_exit_triggered
            && !quick_profit_triggered) continue;

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
        if (futures_query_adapter_) {
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
                portfolio_->close_position(symbol_, current_position_side_, pos.current_price, 0.0);
                reset_trailing_state();
                return true;
            }
            if (actual_qty <= 0.0) actual_qty = pos.size.get(); // fallback (query failed)
        }
        if (actual_qty < 0.00001) {
            // Токен уже продан (стоп-лосс сработал ранее), но позиция
            // осталась в портфеле. Очищаем её принудительно.
            logger_->info("pipeline", "Стоп-лосс: актив уже продан, очищаем позицию из портфеля",
                {{"qty", std::to_string(actual_qty)},
                 {"symbol", symbol_.get()}});
            portfolio_->close_position(symbol_, current_position_side_, pos.current_price, pos.unrealized_pnl);
            reset_trailing_state();
            return true;
        }
        // Пылевая позиция — невозможно продать на бирже, пропускаем
        double actual_notional = actual_qty * price;
        if (actual_notional < config_.trading_params.dust_threshold_usdt) {
            logger_->debug("pipeline", "Пылевая позиция, пропускаем стоп-лосс",
                {{"notional", std::to_string(actual_notional)},
                 {"symbol", symbol_.get()}});
            // Очищаем позицию из портфеля чтобы не блокировать новые сделки
            portfolio_->close_position(symbol_, current_position_side_, pos.current_price, pos.unrealized_pnl);
            reset_trailing_state();
            return false;
        }

        // Определяем количество и причину закрытия
        double close_qty = actual_qty;
        bool is_full_close = true;
        std::string reason;

        if (quick_profit_triggered && !trailing_stop_triggered && !protective_atr_stop_triggered
            && !fixed_stop_triggered && !price_stop_triggered) {
            // Quick profit: забираем плюс целиком (full close)
            reason = "QUICK_PROFIT: PnL=" + std::to_string(pos.unrealized_pnl) + " USDT";
            logger_->info("pipeline", "QUICK PROFIT — фиксируем прибыль",
                {{"symbol", symbol_.get()},
                 {"entry", std::to_string(entry)},
                 {"current", std::to_string(price)},
                 {"pnl", std::to_string(pos.unrealized_pnl)}});
        } else if (partial_tp_triggered && !trailing_stop_triggered
            && !protective_atr_stop_triggered && !fixed_stop_triggered) {
            // TP: закрываем позицию (с проверкой на dust)
            close_qty = actual_qty * config_.trading_params.partial_tp_fraction;
            // Минимальный ордер: используем per-symbol min или fallback
            const double sym_min_notional = exchange_rules_.min_trade_usdt > 0.0
                ? exchange_rules_.min_trade_usdt
                : common::exchange_limits::kMinBitgetNotionalUsdt;
            double min_qty_notional = sym_min_notional / price;
            if (close_qty < min_qty_notional) close_qty = actual_qty;

            // Anti-dust: floor close_qty по exchange precision, проверить остаток
            if (exchange_rules_.quantity_precision >= 0) {
                double floored_close = exchange_rules_.floor_quantity(close_qty);
                double remaining_after = actual_qty - floored_close;
                double remaining_notional = remaining_after * price;
                // Если остаток < min notional или < 2 лота → закрыть 100%
                if (remaining_notional < sym_min_notional
                    || remaining_after < 2.0 * std::max(exchange_rules_.min_quantity, 1.0)) {
                    close_qty = actual_qty; // Full close — no dust
                    logger_->info("pipeline", "ANTI-DUST: остаток слишком мал, закрытие 100%",
                        {{"remaining_qty", std::to_string(remaining_after)},
                         {"remaining_notional", std::to_string(remaining_notional)},
                         {"symbol", symbol_.get()}});
                }
            }

            is_full_close = (close_qty >= actual_qty - 1e-9);
            partial_tp_taken_ = true;
            reason = "TAKE_PROFIT: профит >= " + std::to_string(config_.trading_params.partial_tp_atr_threshold)
                   + "×ATR, закрытие " + std::to_string(static_cast<int>(
                       std::min(close_qty / std::max(actual_qty, 1e-9), 1.0) * 100)) + "%";

            logger_->info("pipeline", "TAKE-PROFIT — закрытие позиции",
                {{"symbol", symbol_.get()},
                 {"entry", std::to_string(entry)},
                 {"current", std::to_string(price)},
                 {"close_qty", std::to_string(close_qty)},
                 {"is_full", is_full_close ? "true" : "false"},
                 {"pnl", std::to_string(pos.unrealized_pnl)}});
        } else if (price_stop_triggered) {
            double price_loss_pct = (current_position_side_ == PositionSide::Short)
                ? (price - entry) / entry * 100.0
                : (entry - price) / entry * 100.0;
            reason = "PRICE_STOP: loss " + std::to_string(price_loss_pct) + "% from entry";
        } else if (fixed_stop_triggered) {
            reason = "FIXED_STOP: убыток " + std::to_string(loss_pct_of_capital) + "% капитала";
        } else if (protective_atr_stop_triggered) {
            reason = "ATR_STOP: цена " + std::to_string(price)
                   + " прошла защитный ATR-стоп " + std::to_string(current_stop_level_);
        } else if (trailing_stop_triggered) {
            reason = "TRAILING_STOP: цена " + std::to_string(price)
                   + " прошла trailing-стоп " + std::to_string(current_stop_level_);
        } else if (time_exit_triggered) {
            int64_t hold_dur = clock_->now().get() - position_entry_time_ns_;
            reason = "TIME_EXIT: hold=" + std::to_string(hold_dur / 60'000'000'000LL)
                   + "min, PnL=" + std::to_string(pos.unrealized_pnl);
        }

        const bool is_take_profit_close = partial_tp_triggered || quick_profit_triggered;
        const bool is_time_exit_close = time_exit_triggered;
        const char* forced_close_message = is_take_profit_close
            ? "TAKE-PROFIT СРАБОТАЛ — принудительное закрытие"
            : (is_time_exit_close
                ? "TIME-EXIT СРАБОТАЛ — принудительное закрытие"
                : "СТОП-ЛОСС СРАБОТАЛ — принудительное закрытие");
        const char* forced_close_order_message = is_take_profit_close
            ? "TAKE-PROFIT ОРДЕР ОТПРАВЛЕН"
            : (is_time_exit_close
                ? "TIME-EXIT ОРДЕР ОТПРАВЛЕН"
                : "СТОП-ЛОСС ОРДЕР ОТПРАВЛЕН");
        const char* forced_close_strategy = is_take_profit_close
            ? "take_profit"
            : (is_time_exit_close ? "time_exit" : "stop_loss");

        if (is_full_close) {
            logger_->warn("pipeline", forced_close_message,
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
        close_intent.strategy_id = StrategyId(forced_close_strategy);
        close_intent.limit_price = snapshot.mid_price;
        close_intent.snapshot_mid_price = snapshot.mid_price;
        close_intent.suggested_quantity = Quantity(close_qty);

        // Устанавливаем trade_side=Close и position_side для корректного ордера.
        // Без этого Bitget Mix API получит tradeSide=open и откроет встречную позицию
        // вместо закрытия текущей.
        close_intent.trade_side = TradeSide::Close;
        close_intent.position_side = current_position_side_;
        close_intent.signal_intent = (current_position_side_ == PositionSide::Long)
            ? strategy::SignalIntent::LongExit : strategy::SignalIntent::ShortExit;
        // Для short-позиции side=Buy (закрытие short = покупка на фьючерсах)
        if (current_position_side_ == PositionSide::Short) {
            close_intent.side = Side::Buy;
        }

        // Уникальный correlation_id для предотвращения дублирования
        close_intent.correlation_id = CorrelationId(
            "SL-" + std::to_string(now_ns));

        risk::RiskDecision risk_decision;
        risk_decision.decided_at = clock_->now();
        risk_decision.approved_quantity = Quantity(close_qty);
        risk_decision.verdict = risk::RiskVerdict::Approved;
        risk_decision.summary = "FORCED_CLOSE: " + reason;

        // Stop-loss bypass: не используем execution_alpha для экстренных ордеров.
        // ExecutionAlpha может блокировать ордер ("NoExecution: условия неблагоприятны"),
        // но стоп-лосс ОБЯЗАН исполниться для ограничения убытков.
        execution_alpha::ExecutionAlphaResult exec_alpha;
        exec_alpha.should_execute = true;
        exec_alpha.recommended_style = execution_alpha::ExecutionStyle::Aggressive;
        exec_alpha.urgency_score = 1.0;
        exec_alpha.rationale = "FORCED_CLOSE: bypass execution alpha";

        // Обновляем cooldown стоп-лосса ПЕРЕД отправкой (даже при ошибке — не спамить)
        last_stop_loss_time_ns_ = now_ns;
        // Также обновляем общий cooldown — стоп-лосс это тоже ордер
        last_order_time_ns_ = now_ns;
        last_activity_ns_.store(now_ns, std::memory_order_relaxed);

        auto order_result = execution_engine_->execute(close_intent, risk_decision, exec_alpha,
            uncertainty::UncertaintySnapshot{});  // Стоп-лосс обходит неопределённость
        if (order_result) {
            close_order_pending_ = true; // Block new entries until close fills
            risk_engine_->record_order_sent();
            logger_->warn("pipeline", is_full_close
                    ? forced_close_order_message
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

                // BayesianAdapter: стоп-лосс тоже является наблюдением для адаптации.
                // БЕЗ этого Bayesian видит только стратегические закрытия, что создаёт
                // survivorship bias — модель не учитывает неудачные сделки.
                if (bayesian_adapter_) {
                    ml::ParameterObservation obs;
                    // Normalize: use PnL % / max_loss_per_trade_pct, clamped to [-1, +1]
                    const double risk_ref = std::max(config_.trading_params.max_loss_per_trade_pct, 0.5);
                    double pnl_pct = pos.unrealized_pnl_pct;
                    obs.reward = std::clamp(pnl_pct / risk_ref, -1.0, 1.0);
                    obs.regime = regime_engine_->classify(snapshot).detailed;
                    bayesian_adapter_->record_observation("global", obs);
                }

                // Thompson Sampling: стоп-лосс — используем действие входа
                if (thompson_sampler_) {
                    double ts_reward = (pos.unrealized_pnl > 0) ? 1.0 : -1.0;
                    thompson_sampler_->record_reward(current_entry_thompson_action_, ts_reward);
                }

                // ИСПРАВЛЕНИЕ H6 (аудит): feedback в world model для forced exits.
                // Без этого world model учится только на "чистых" закрытиях
                // и недоучитывает принудительные исходы (survivorship bias).
                if (world_model_) {
                    double sl_pnl_pct = (pos.notional.get() > 0.0)
                        ? pos.unrealized_pnl / pos.notional.get() * 100.0
                        : 0.0;
                    world_model::WorldStateFeedback wfb;
                    wfb.state = current_entry_world_state_;
                    wfb.strategy_id = current_position_strategy_;
                    wfb.pnl_bps = sl_pnl_pct * 100.0;  // pct → bps
                    wfb.slippage_bps = current_position_slippage_bps_;
                    wfb.max_adverse_excursion_bps = current_max_adverse_excursion_bps_;
                    wfb.was_profitable = (pos.unrealized_pnl > 0.0);
                    wfb.timestamp = clock_->now();
                    world_model_->record_feedback(wfb);
                }

                // Записываем результат в risk engine
                risk_engine_->record_trade_result(pos.unrealized_pnl < 0.0);

                // Rolling statistics: стоп-лосс тоже учитывается для Kelly Criterion.
                // Без этого win_rate/win_loss_ratio завышаются (survivorship bias).
                {
                    double sl_pnl_pct = (pos.notional.get() > 0.0)
                        ? pos.unrealized_pnl / pos.notional.get() * 100.0
                        : 0.0;
                    record_trade_for_stats(sl_pnl_pct);
                }

                // Обновляем edge stats в leverage engine
                if (leverage_engine_) {
                    leverage_engine_->update_edge_stats(rolling_win_rate(), rolling_win_loss_ratio());
                }

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
    } else if (config_.trading.mode == TradingMode::Paper) {
        logger_->info("pipeline",
            "Paper mode: пропуск биржевой настройки hold mode / margin mode / leverage",
            {{"symbol", symbol_.get()}});
    }

    if (futures_query_adapter_) {
        current_funding_rate_ = futures_query_adapter_->get_current_funding_rate(symbol_);
        if (risk_engine_) {
            risk_engine_->set_funding_rate(current_funding_rate_);
        }
    }

    // Загрузка исторических свечей для прогрева индикаторов.
    // КРИТИЧНО: без этого SMA/RSI/ADX/EMA не имеют данных,
    // и scalp_engine торгует вслепую.
    bootstrap_historical_candles();

    // Startup already performed the heavy exchange sync path. Do not repeat it
    // on the first live tick; spread the periodic jobs across symbols instead.
    {
        const int64_t startup_now_ns = clock_->now().get();
        constexpr int64_t kFiveMinuteSpreadNs = 60'000'000'000LL;
        constexpr int64_t kReferenceSpreadNs = 25'000'000'000LL;

        last_watchdog_ns_ = startup_now_ns;
        last_reconciliation_ns_ = startup_now_ns;
        last_balance_sync_ns_ = startup_now_ns
            - stable_periodic_offset_ns(symbol_.get(), "balance_sync", kFiveMinuteSpreadNs);
        last_funding_rate_update_ns_ = startup_now_ns
            - stable_periodic_offset_ns(symbol_.get(), "funding_rate", kFiveMinuteSpreadNs);
        last_pos_balance_reconciliation_ns_ = startup_now_ns
            - stable_periodic_offset_ns(symbol_.get(), "pos_balance_reconcile", kFiveMinuteSpreadNs);
        last_reference_price_update_ns_ = startup_now_ns
            - stable_periodic_offset_ns(symbol_.get(), "reference_prices", kReferenceSpreadNs);
    }

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
    // Hot path: не ходим в REST из обычного market tick.
    // Позиции уже синхронизируются через startup recovery, periodic balance sync
    // и reconciliation, поэтому здесь опираемся только на локальное состояние.
    return portfolio_->has_position(symbol_);
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

    TickStageProfiler tick_profiler(
        latency_tracker_.get(), logger_, symbol_.get(), &diag_slow_tick_breakdown_);

    ++tick_count_;

    int64_t now_ns = clock_->now().get();

    {
        auto ingress_stage = tick_profiler.scope(TickStageProfiler::Stage::Ingress);

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
    }

    {
        auto ml_stage = tick_profiler.scope(TickStageProfiler::Stage::Ml);

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
    }

    {
        auto ingress_stage = tick_profiler.scope(TickStageProfiler::Stage::Ingress);

        // 0a. HEDGE RECOVERY: проверяем хедж-позицию ПЕРЕД стоп-лоссом.
        // Если хедж активен — он управляет закрытием, обычный стоп-лосс отключён.
        if (check_hedge_recovery(snapshot)) {
            return;
        }

        // 0b. СТОП-ЛОСС: проверяем КАЖДЫЙ тик, независимо от стратегий.
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
    }

    // 0i. Thompson Sampling: проверяем отложенный вход
    bool thompson_bypass = false;  // Если pending entry активирован — пропустить Thompson ниже
    std::optional<strategy::TradeIntent> activated_pending_intent;    // A1 fix: сохраняем intent
    ml::EntryAction activated_pending_action = ml::EntryAction::EnterNow; // A1 fix: сохраняем action
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
            // A1 fix: сохраняем intent и action ДО reset, чтобы использовать ниже
            activated_pending_intent = pe.intent;
            activated_pending_action = pe.action;
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

    world_model::WorldModelSnapshot world;
    regime::RegimeSnapshot regime;
    uncertainty::UncertaintySnapshot uncertainty;
    portfolio::PortfolioSnapshot pre_trade_portfolio;
    bool pre_trade_has_position = false;
    {
        auto context_stage = tick_profiler.scope(TickStageProfiler::Stage::Context);

        // 1. Мировая модель
        world = world_model_->update(snapshot);

        // 2. Режим рынка
        regime = regime_engine_->classify(snapshot);
        risk_engine_->set_current_regime(regime.detailed);

    // 2a. Chop regime — логируем, но НЕ блокируем вход.
    // Risk engine уже применяет chop_regime_scale (0.50) — уменьшает размер позиции.
    // Полная блокировка в Chop делала бот неактивным (крипто ~60-70% времени в боковике).
    pre_trade_portfolio = portfolio_->snapshot();
    for (const auto& pos : pre_trade_portfolio.positions) {
        if (pos.symbol.get() == symbol_.get() && pos.size.get() >= 0.00001) {
            pre_trade_has_position = true;
            break;
        }
    }

    if (!pre_trade_has_position &&
        regime.detailed == regime::DetailedRegime::Chop) {
        if (tick_count_ % 200 == 0) {
            logger_->info("pipeline", "CHOP WARNING: режим Chop — risk engine уменьшит позицию",
                std::unordered_map<std::string, std::string>{
                 {"regime", std::string(to_string(regime.label))},
                 {"adx", std::to_string(snapshot.technical.adx)},
                 {"symbol", symbol_.get()}});
        }
    }

    // 2b. WorldModel ChopNoise — логируем, но НЕ блокируем вход.
    // Аналогично: risk engine масштабирует размер по chop_regime_scale.
    if (!pre_trade_has_position &&
        world.state == world_model::WorldState::ChopNoise) {
        if (tick_count_ % 200 == 0) {
            logger_->info("pipeline", "CHOP_NOISE WARNING: WorldModel ChopNoise — risk engine уменьшит позицию",
                std::unordered_map<std::string, std::string>{
                 {"world_state", world_model::to_string(world.state)},
                 {"fragility", std::to_string(world.fragility.value)},
                 {"symbol", symbol_.get()}});
        }
    }

        // 2c. LowVolCompression — полный запрет на новый вход.
        // Это режим с узким диапазоном и слабой направленностью, где scalp-сигналы
        // статистически деградируют в churn и комиссионный drain. Выходы из позиции
        // не блокируем: барьер только для новых входов.
        if (!pre_trade_has_position &&
            regime.detailed == regime::DetailedRegime::LowVolCompression) {
            if (tick_count_ % 200 == 0) {
                logger_->info("pipeline", "LOW_VOL_COMPRESSION BLOCK: вход запрещён",
                    std::unordered_map<std::string, std::string>{
                     {"regime", std::string(to_string(regime.label))},
                     {"world_state", world_model::to_string(world.state)},
                     {"fragility", std::to_string(world.fragility.value)},
                     {"symbol", symbol_.get()}});
            }
            return;
        }

        // 2d. Minimum ATR% gate — блокируем вход если волатильность слишком низкая.
    // При ATR% < 0.1% (ATR/price < 0.001) стоп-лосс на расстоянии шума,
    // любой вход мгновенно выбивается. Только для новых позиций.
    if (!pre_trade_has_position &&
        snapshot.technical.atr_valid && snapshot.technical.atr_14 > 0.0 &&
        snapshot.mid_price.get() > 0.0) {
        double atr_pct = snapshot.technical.atr_14 / snapshot.mid_price.get();
        if (atr_pct < 0.001) {  // < 0.1% volatility
            if (tick_count_ % 200 == 0) {
                logger_->info("pipeline",
                    "LOW_VOL BLOCK: ATR% слишком низкий для надёжного стопа",
                    {{"atr_pct", std::to_string(atr_pct * 100.0)},
                     {"atr", std::to_string(snapshot.technical.atr_14)},
                     {"price", std::to_string(snapshot.mid_price.get())},
                     {"symbol", symbol_.get()}});
            }
            return;
        }
    }

        // 3. Uncertainty (v2: full context with portfolio and ML signals)
        // Вычисляем aggregate ML-поля ДО uncertainty, чтобы combined_risk_multiplier
        // и should_block_trading были доступны для UncertaintyEngine.
        // Fingerprint и Bayesian-поля заполнятся позже — они не влияют на блокировки.
        ml_snapshot_.compute_aggregates();
        uncertainty = uncertainty_engine_->assess(
            snapshot, regime, world, pre_trade_portfolio, ml_snapshot_);

        // 4. Статус рынка каждые 100 тиков (~30 секунд)
        if (tick_count_ % 100 == 0) {
            print_status(snapshot, world, regime);
        }
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

    decision::DecisionRecord decision;
    bool has_long_position = false;
    bool has_short_position = false;
    const portfolio::Position* long_position = nullptr;
    const portfolio::Position* short_position = nullptr;
    {
        auto signal_stage = tick_profiler.scope(TickStageProfiler::Stage::Signal);

        // 6. Аллокация стратегий по текущему режиму
        auto allocation = strategy_allocator_->allocate(
            strategy_registry_->active(), regime, world, uncertainty);

        // 7. Оценка каждой активной стратегии
        std::vector<strategy::TradeIntent> intents;

        // Проверяем текущую позицию ДО оценки стратегий.
        // Фьючерсы: допускаются все сигналы (LongEntry, LongExit, ShortEntry, ShortExit).
        for (const auto& pos : pre_trade_portfolio.positions) {
            if (pos.symbol.get() == symbol_.get()) {
                if (pos.side == Side::Buy && pos.size.get() >= 0.00001) {
                    has_long_position = true;
                    long_position = &pos;
                }
                if (pos.side == Side::Sell && pos.size.get() >= 0.00001) {
                    has_short_position = true;
                    short_position = &pos;
                }
            }
        }

        // Диагностика: сколько стратегий сгенерировали сигналы
        int total_intents = 0;

        for (const auto& strat : strategy_registry_->active()) {
            strategy::StrategyContext ctx;
            ctx.features = snapshot;
            ctx.regime = regime.label;
            ctx.world_state = world_model::WorldModelSnapshot::to_label(world.state);
            ctx.uncertainty = uncertainty.level;
            ctx.uncertainty_size_multiplier = uncertainty.size_multiplier;
            ctx.uncertainty_threshold_multiplier = uncertainty.threshold_multiplier;
            ctx.futures_enabled = true;
            ctx.htf_trend_direction = htf_trend_direction_;
            ctx.htf_trend_strength = htf_trend_strength_;

            // Strategy Engine: передаём позицию и системное состояние (hedge-mode aware)
            ctx.position.has_position = has_long_position || has_short_position;
            ctx.position.has_long = has_long_position;
            ctx.position.has_short = has_short_position;
            if (ctx.position.has_position) {
                if (long_position) {
                    ctx.position.long_size = long_position->size.get();
                    ctx.position.long_entry_price = long_position->avg_entry_price.get();
                    ctx.position.long_unrealized_pnl = long_position->unrealized_pnl;
                }
                if (short_position) {
                    ctx.position.short_size = short_position->size.get();
                    ctx.position.short_entry_price = short_position->avg_entry_price.get();
                    ctx.position.short_unrealized_pnl = short_position->unrealized_pnl;
                }
                // Backward-compatible: заполняем primary leg в старые поля
                // Приоритет: long leg если есть, иначе short
                if (has_long_position) {
                    ctx.position.side = Side::Buy;
                    ctx.position.position_side = PositionSide::Long;
                    ctx.position.size = ctx.position.long_size;
                    ctx.position.avg_entry_price = ctx.position.long_entry_price;
                    ctx.position.unrealized_pnl = ctx.position.long_unrealized_pnl;
                } else {
                    ctx.position.side = Side::Sell;
                    ctx.position.position_side = PositionSide::Short;
                    ctx.position.size = ctx.position.short_size;
                    ctx.position.avg_entry_price = ctx.position.short_entry_price;
                    ctx.position.unrealized_pnl = ctx.position.short_unrealized_pnl;
                }
                ctx.position.entry_time_ns = position_entry_time_ns_;
            }
            ctx.data_fresh = snapshot.execution_context.is_feed_fresh;
            ctx.exchange_ok = true; // при отключении WebSocket pipeline не вызывается

            auto intent = strat->evaluate(ctx);
            if (intent.has_value()) {
                ++total_intents;

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
                 {"actionable", std::to_string(intents.size())},
                 {"has_position", (has_long_position || has_short_position) ? "true" : "false"},
                 {"tick", std::to_string(tick_count_)}});
        }

        if (intents.empty()) return;

        // 8. Агрегация решений комитетом (с portfolio/features context)
        decision = decision_engine_->aggregate(
            symbol_, intents, allocation, regime, world, uncertainty,
            pre_trade_portfolio, snapshot);
    }

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

    // A1 fix: при активации отложенного входа Thompson подменяем intent
    // и фиксируем action, чтобы reward attribution работал корректно.
    // Без этого исполняется новый intent текущего тика, а не тот,
    // который прошёл через Thompson wait.
    if (thompson_bypass && activated_pending_intent.has_value()) {
        intent = *activated_pending_intent;
        logger_->debug("pipeline", "Thompson bypass: intent подменён на сохранённый",
            {{"strategy", intent.strategy_id.get()},
             {"action", ml::to_string(activated_pending_action)}});
    }

    double bayesian_conviction_adj = 0.0;
    double correlation_risk_mult = ml_snapshot_.correlation_risk_multiplier;
    ml::EntryAction thompson_action = thompson_bypass
        ? activated_pending_action   // A1 fix: используем action из pending entry
        : ml::EntryAction::EnterNow;
    bool is_closing_position = false;
    Quantity closing_qty{0.0};
    double position_size_for_log = 0.0;
    double closing_pnl = 0.0;
    double closing_pnl_pct = 0.0;

    {
        auto signal_stage = tick_profiler.scope(TickStageProfiler::Stage::Signal);

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

    // 8b2. Risk:Reward pre-entry filter — пропускаем сделки с плохим R:R.
    // Минимальный порог R:R зависит от WR стратегии (Kelly criterion):
    //   WR≥60% → break-even R:R = 0.67;  WR=50% → break-even R:R = 1.0.
    // Порог настраивается в конфиге: min_risk_reward_ratio.
    if (snapshot.technical.atr_valid && snapshot.technical.atr_14 > 0.0 &&
        !pre_trade_has_position) {
        double atr = snapshot.technical.atr_14;
        double price = snapshot.last_price.get();
        
        // Риск = расстояние до стопа (ATR-based, использование adapted multiplier если доступен)
        double stop_mult = (ml_snapshot_.adapted_atr_stop_mult > 0.0)
            ? ml_snapshot_.adapted_atr_stop_mult
            : config_.trading_params.atr_stop_multiplier;
        double risk_distance = atr * stop_mult;
        
        // Потенциал прибыли: expected move based on regime
        double reward_distance = atr * config_.trading_params.partial_tp_atr_threshold;
        
        // Reward adjustments: совокупный boost ограничен 1.5× (научная калибровка).
        // Lopez de Prado (2018) показал, что статические оценки reward склонны к
        // optimism bias — ограничиваем максимальный бонус.
        double reward_boost = 1.0;
        // Бонус к reward в сильном тренде (ADX > 35 = strong trend, +25% move potential)
        if (snapshot.technical.adx_valid && snapshot.technical.adx > 35.0) {
            reward_boost *= 1.25;
        }
        // Бонус если HTF тренд совпадает с направлением сделки (+20%)
        if ((intent.side == Side::Buy && htf_trend_direction_ == 1) ||
            (intent.side == Side::Sell && htf_trend_direction_ == -1)) {
            reward_boost *= 1.20;
        }
        // ADX-based adjustment: low ADX = ranging = less directional edge
        if (snapshot.technical.adx_valid && snapshot.technical.adx < 20.0) {
            reward_boost *= 0.90; // Mild penalty in ranging for scalping
        }
        // Cap combined boost: не более 1.5× (избегаем overfitting через чрезмерный optimism)
        reward_boost = std::min(reward_boost, 1.5);
        reward_distance *= reward_boost;
        
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

    // 8c. Корреляционный монитор: используем кэшированный результат из 0.ml3
    // (избегаем повторного вызова evaluate() — данные уже в ml_snapshot_)
    if (ml_snapshot_.correlation_break && tick_count_ % 200 == 0) {
        logger_->warn("pipeline",
            "Разрыв корреляции обнаружен — размер позиции снижен",
            {{"risk_mult", std::to_string(correlation_risk_mult)},
             {"avg_corr", std::to_string(ml_snapshot_.avg_correlation)}});
    }

    // 8d. Thompson Sampling: выбираем момент входа
    // thompson_bypass = true когда pending entry только что активировался —
    // повторно вызывать Thompson нельзя, иначе бесконечный цикл Wait→Activate→Wait
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
            if (++diag_thompson_block_ <= kDiagLogLimit || tick_count_ % 200 == 0) {
                logger_->info("pipeline",
                    "Thompson Sampling: сигнал пропущен (Skip)",
                    {{"strategy", intent.strategy_id.get()},
                     {"diag_count", std::to_string(diag_thompson_block_)}});
            }
            return;
        }

        if (wait > 0) {
            // Track consecutive waits for rotation trigger
            ++consecutive_wait1_count_;
            if (consecutive_wait1_count_ >= kMaxConsecutiveWait1) {
                // Thompson repeatedly says Wait — market uncertain for this pair.
                // Force pipeline idle so main.cpp rotation thread picks a new pair.
                // Use 1 instead of 0 because is_idle() treats 0 as "never had a tick"
                last_activity_ns_.store(1, std::memory_order_relaxed);
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
        // Check for existing position (direction-aware for futures)
        bool is_close_order = (intent.signal_intent == strategy::SignalIntent::LongExit ||
                               intent.signal_intent == strategy::SignalIntent::ShortExit ||
                               intent.signal_intent == strategy::SignalIntent::ReducePosition);

        bool blocked = false;
        // Block BUY in HTF downtrend (EMA20 < EMA50, strength > 0.15)
        // Понижен порог с 0.4 до 0.15: даже слабый даунтренд блокирует покупку
        if (intent.side == Side::Buy && htf_trend_direction_ == -1 && htf_trend_strength_ > 0.15) {
            // Allow only if clear reversal: MACD > 0 AND RSI > 40
            bool reversal_confirmed = (htf_macd_histogram_ > 0.0 && htf_rsi_14_ > 40.0);
            if (!reversal_confirmed && !is_close_order) {
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
        // Block SELL in HTF uptrend, but ONLY for new short entries.
        // SELL to close existing position (take-profit) is always allowed.
        // Понижен порог с 0.4 до 0.15: даже слабый аптренд блокирует новые шорты
        if (intent.side == Side::Sell && !is_close_order && htf_trend_direction_ == 1 && htf_trend_strength_ > 0.15) {
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
            ++diag_htf_block_;
            if (diag_htf_block_ <= kDiagLogLimit) {
                logger_->info("pipeline", "HTF Trend Filter блокирует сигнал",
                    {{"side", intent.side == Side::Buy ? "BUY" : "SELL"},
                     {"htf_direction", std::to_string(htf_trend_direction_)},
                     {"htf_strength", std::to_string(htf_trend_strength_)},
                     {"diag_count", std::to_string(diag_htf_block_)}});
            }
            return;
        }
        }

    // 9a. Cooldown — не торгуем чаще настроенного интервала
        now_ns = clock_->now().get();  // обновляем значение (объявлено выше, в freshness gate)
        int64_t order_cooldown_ns = static_cast<int64_t>(config_.trading_params.order_cooldown_seconds)
                                   * 1'000'000'000LL;
        if (last_order_time_ns_ > 0 && (now_ns - last_order_time_ns_) < order_cooldown_ns) {
            if (++diag_cooldown_block_ <= kDiagLogLimit) {
                int64_t remaining_ms = (order_cooldown_ns - (now_ns - last_order_time_ns_)) / 1'000'000LL;
                logger_->info("pipeline", "Order cooldown блокирует вход",
                    {{"remaining_ms", std::to_string(remaining_ms)},
                     {"diag_count", std::to_string(diag_cooldown_block_)}});
            }
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
    {
        bool has_position = false;
        bool position_is_long = false;
        Quantity position_size{0.0};
        if (long_position) {
            has_position = true;
            position_is_long = true;
            position_size = long_position->size;
            closing_pnl = long_position->unrealized_pnl;
            closing_pnl_pct = long_position->unrealized_pnl_pct;
        } else if (short_position) {
            has_position = true;
            position_is_long = false;
            position_size = short_position->size;
            closing_pnl = short_position->unrealized_pnl;
            closing_pnl_pct = short_position->unrealized_pnl_pct;
        }

        bool intent_is_buy = (intent.side == Side::Buy);

        // Per-symbol минимальный торгуемый нотионал.
        // Пылевые остатки ниже этого порога не считаются реальной позицией.
        const double kMinTradeableNotional = exchange_rules_.min_trade_usdt > 0.0
            ? exchange_rules_.min_trade_usdt
            : common::exchange_limits::kMinBitgetNotionalUsdt;
        double position_notional = position_size.get() * snapshot.mid_price.get();

        {
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
            const bool is_partial_reduce_signal =
                intent.signal_intent == strategy::SignalIntent::ReducePosition;

            if (is_exit_signal && has_position && position_notional >= kMinTradeableNotional) {
                // === HIGH-WR PnL GATE ===
                // Закрытие по сигналу стратегии ТОЛЬКО если:
                //   1. Позиция в плюсе (фиксируем профит) → всегда закрыть
                //   2. Убыток > pnl_gate_loss_pct капитала → cut losses
                //   3. Малый убыток → НЕ закрывать, пусть trailing stop решит
                constexpr double kPnlGateLossPctFallback = 0.5;
                const double pnl_gate_threshold = config_.trading_params.pnl_gate_loss_pct > 0.0
                    ? config_.trading_params.pnl_gate_loss_pct : kPnlGateLossPctFallback;
                double capital = pre_trade_portfolio.total_capital;
                bool position_is_green = (closing_pnl > 0.0);
                double loss_pct = (capital > 0.0)
                    ? (std::abs(std::min(closing_pnl, 0.0)) / capital * 100.0) : 0.0;
                bool loss_exceeds_gate = (loss_pct >= pnl_gate_threshold);

                if (!position_is_green && !loss_exceeds_gate) {
                    // Позиция в небольшом минусе — НЕ закрываем по сигналу,
                    // пусть trailing stop или TP решат судьбу позиции
                    if (++diag_pnl_gate_block_ <= 10) {
                        logger_->info("pipeline", "PnL gate: пропуск закрытия по сигналу (позиция в минусе, ждём стоп/TP)",
                            {{"symbol", symbol_.get()},
                             {"unrealized_pnl", std::to_string(closing_pnl)},
                             {"loss_pct", std::to_string(loss_pct)},
                             {"signal", intent.signal_name}});
                    }
                    return;
                }

                // Закрытие существующей позиции
                is_closing_position = true;
                closing_qty = position_size;
                if (is_partial_reduce_signal && intent.suggested_quantity.get() > 0.0) {
                    closing_qty = Quantity(std::min(intent.suggested_quantity.get(), position_size.get()));
                }
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
    }

    risk::RiskDecision risk_decision;
    risk_decision.decided_at = clock_->now();

    // Для маленьких аккаунтов (< $100) гарантируем быстрое исполнение:
    // urgency >= 0.8 → Aggressive → market order.
    // Стоимость проскальзывания на $15 ордере ~ 0.01%, стоимость
    // незаполненного лимитного ордера (потерянный вход) гораздо выше.
    if (intent.side == Side::Buy && pre_trade_portfolio.total_capital < 100.0) {
        intent.urgency = std::max(intent.urgency, 0.8);
    }

    // Execution alpha — вычисляем один раз для всех ветвей
    auto exec_alpha = execution_alpha_->evaluate(intent, snapshot, uncertainty);

    {
        auto risk_stage = tick_profiler.scope(TickStageProfiler::Stage::Risk);

    if (is_closing_position) {
        const bool is_partial_reduce_signal =
            intent.signal_intent == strategy::SignalIntent::ReducePosition;

        // Закрытие позиции ОБЯЗАНО исполниться → market order.
        // Лимитный ордер на закрытие может не заполниться, и позиция зависнет.
        exec_alpha.recommended_style = execution_alpha::ExecutionStyle::Aggressive;
        exec_alpha.should_execute = true;
        exec_alpha.urgency_score = 1.0;
        intent.urgency = 1.0;

        // Определяем размер для закрытия в зависимости от режима
        {
            // Фьючерсы: размер позиции берём из портфеля (на фьючерсах нет "баланса base coin",
            // позиция — это маржинальный контракт с размером в base currency)
            double exchange_qty = 0.0;
            if (futures_query_adapter_) {
                auto positions = futures_query_adapter_->get_positions(symbol_);
                if (positions) {
                    for (const auto& fp : *positions) {
                        if (fp.position_side == current_position_side_ && fp.total.get() > 0.0) {
                            exchange_qty = fp.total.get();
                            logger_->info("pipeline", "Фьючерсная позиция для закрытия",
                                {{"exchange_qty", std::to_string(exchange_qty)},
                                 {"portfolio_qty", std::to_string(position_size_for_log)},
                                 {"position_side", (current_position_side_ == PositionSide::Long) ? "long" : "short"}});
                            break;
                        }
                    }
                }
            }

            if (exchange_qty > 1e-8) {
                const double target_qty = is_partial_reduce_signal
                    ? std::min(closing_qty.get(), exchange_qty)
                    : exchange_qty;
                closing_qty = Quantity(target_qty);
            } else if (closing_qty.get() < 1e-8) {
                // Fallback на размер из портфеля
                closing_qty = Quantity(position_size_for_log);
            }

            if (is_partial_reduce_signal && closing_qty.get() < 1e-8) {
                logger_->warn("pipeline", "Пропуск partial reduce: нулевой объём после синхронизации",
                    {{"symbol", symbol_.get()}});
                return;
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

        logger_->info("pipeline", "Закрытие позиции (стратегия)",
            {{"symbol", symbol_.get()},
             {"qty", std::to_string(closing_qty.get())},
             {"signal", intent.signal_name},
             {"signal_intent", std::to_string(static_cast<int>(intent.signal_intent))},
             {"unrealized_pnl", std::to_string(closing_pnl)},
             {"conviction", std::to_string(intent.conviction)}});

        close_order_pending_ = true; // Block new entries until close fills
    } else {
        // Block new entries while a close order is pending execution
        if (close_order_pending_) {
            logger_->debug("pipeline", "Вход заблокирован: ожидание закрытия позиции",
                {{"symbol", symbol_.get()}});
            return;
        }

        // 9d. Opportunity Cost — оценка целесообразности входа
        //     Вызывается ПЕРЕД sizing/risk для новых позиций.
        //     Закрытие позиции обходит этот шаг (безусловно исполняется).
        {
            opportunity_cost::PortfolioContext oc_ctx;
            // exposure_pct из PortfolioEngine приходит в масштабе 0-100%,
            // а пороги в конфиге — в масштабе 0-1. Нормализуем:
            oc_ctx.gross_exposure_pct = pre_trade_portfolio.exposure.exposure_pct / 100.0;
            oc_ctx.current_drawdown_pct = pre_trade_portfolio.pnl.current_drawdown_pct / 100.0;
            oc_ctx.consecutive_losses = pre_trade_portfolio.pnl.consecutive_losses;

            // Расчёт концентрации по символу и стратегии + worst position (для Upgrade)
            double symbol_notional = 0.0;
            double strategy_notional = 0.0;
            double worst_net_bps = std::numeric_limits<double>::max();
            bool found_worst = false;
            for (const auto& pos : pre_trade_portfolio.positions) {
                if (pos.symbol.get() == intent.symbol.get()) {
                    symbol_notional += std::abs(pos.notional.get());
                }
                if (pos.strategy_id.get() == intent.strategy_id.get()) {
                    strategy_notional += std::abs(pos.notional.get());
                }
                // Worst position: наименьший unrealized P&L в bps от notional
                double abs_notional = std::abs(pos.notional.get());
                if (abs_notional > 0.0) {
                    double pos_net_bps = (pos.unrealized_pnl / abs_notional) * 10000.0;
                    if (pos_net_bps < worst_net_bps) {
                        worst_net_bps = pos_net_bps;
                        found_worst = true;
                    }
                }
            }
            oc_ctx.has_worst_position = found_worst;
            if (found_worst) {
                oc_ctx.worst_position_net_bps = worst_net_bps;
            }
            // Для фьючерсов: пересчитываем exposure по margin (notional / leverage),
            // чтобы корректно сравнивать с порогами concentration (0-1 масштаб).
            double leverage_div = std::max(1.0, static_cast<double>(config_.futures.default_leverage));
            if (pre_trade_portfolio.total_capital > 0.0) {
                oc_ctx.symbol_exposure_pct = (symbol_notional / leverage_div) / pre_trade_portfolio.total_capital;
                oc_ctx.strategy_exposure_pct = (strategy_notional / leverage_div) / pre_trade_portfolio.total_capital;
            }

            auto oc_result = opportunity_cost_engine_->evaluate(
                intent, exec_alpha, oc_ctx,
                config_.decision.min_conviction_threshold);

            if (oc_result.action == opportunity_cost::OpportunityAction::Suppress) {
                ++diag_opp_cost_block_;
                logger_->info("pipeline", "Вход отклонён opportunity cost: Suppress",
                    {{"symbol", symbol_.get()},
                     {"reason", opportunity_cost::to_string(oc_result.reason)},
                     {"net_bps", std::to_string(oc_result.score.net_expected_bps)},
                     {"score", std::to_string(oc_result.score.score)},
                     {"gross_exposure", std::to_string(oc_ctx.gross_exposure_pct)},
                     {"symbol_exposure", std::to_string(oc_ctx.symbol_exposure_pct)},
                     {"strategy_exposure", std::to_string(oc_ctx.strategy_exposure_pct)},
                     {"diag_count", std::to_string(diag_opp_cost_block_)}});
                return;
            }

            if (oc_result.action == opportunity_cost::OpportunityAction::Defer) {
                ++diag_opp_cost_block_;
                logger_->info("pipeline", "Вход отложен opportunity cost: Defer",
                    {{"symbol", symbol_.get()},
                     {"reason", opportunity_cost::to_string(oc_result.reason)},
                     {"net_bps", std::to_string(oc_result.score.net_expected_bps)},
                     {"gross_exposure", std::to_string(oc_ctx.gross_exposure_pct)},
                     {"symbol_exposure", std::to_string(oc_ctx.symbol_exposure_pct)},
                     {"strategy_exposure", std::to_string(oc_ctx.strategy_exposure_pct)},
                     {"diag_count", std::to_string(diag_opp_cost_block_)}});
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

            // Реальные win_rate/win_loss_ratio из скользящего окна последних сделок.
            // До накопления статистики используются консервативные значения
            // (0.45 / 1.2), что занижает размер позиции — безопасный старт.
            double win_rate = rolling_win_rate();
            double win_loss_ratio = rolling_win_loss_ratio();

            portfolio_allocator_->set_market_context(
                realized_vol_annual, regime.detailed, win_rate, win_loss_ratio);
        }

        // Combined size multiplier
        double combined_size_mult = uncertainty.size_multiplier *
                correlation_risk_mult;

        // Строим AllocationContext — полный контекст для compute_size_v2.
        // Все поля заполняются явно, т.к. compute_size_v2 использует
        // именно AllocationContext, а не stateful поля аллокатора.
        portfolio_allocator::AllocationContext alloc_ctx;
        {
            double realized_vol_annual_ctx = 0.0;
            if (snapshot.technical.volatility_valid && snapshot.technical.volatility_20 > 0.0) {
                realized_vol_annual_ctx = snapshot.technical.volatility_20 * std::sqrt(525600.0);
            }
            alloc_ctx.realized_vol_annual = realized_vol_annual_ctx;
        }
        alloc_ctx.regime = regime.detailed;
        alloc_ctx.win_rate = rolling_win_rate();
        alloc_ctx.avg_win_loss_ratio = rolling_win_loss_ratio();

        // Контекст ликвидности
        alloc_ctx.current_spread_bps = snapshot.microstructure.spread_bps;
        alloc_ctx.book_depth_notional = snapshot.microstructure.bid_depth_5_notional
                                      + snapshot.microstructure.ask_depth_5_notional;

        // Контекст портфеля
        alloc_ctx.current_drawdown_pct = pre_trade_portfolio.pnl.current_drawdown_pct;
        alloc_ctx.consecutive_losses = pre_trade_portfolio.pnl.consecutive_losses;
        if (exchange_rules_.min_trade_usdt > 0.0) {
            portfolio_allocator::ExchangeFilters ef;
            ef.symbol = symbol_;
            ef.min_quantity = exchange_rules_.min_quantity;
            // max_quantity=0 в ExchangeSymbolRules означает "не задан" — используем безопасный default
            ef.max_quantity = exchange_rules_.max_quantity > 0.0
                ? exchange_rules_.max_quantity : 1e12;
            ef.min_notional = exchange_rules_.min_trade_usdt;
            ef.quantity_precision = exchange_rules_.quantity_precision;
            ef.price_precision = exchange_rules_.price_precision;
            // quantity_step из precision: 10^(-precision), напр. precision=0 → step=1.0
            if (exchange_rules_.quantity_precision >= 0) {
                ef.quantity_step = std::pow(10.0, -exchange_rules_.quantity_precision);
            }
            alloc_ctx.exchange_filters = ef;
        }

        // ИСПРАВЛЕНИЕ H1: разделить капитал между pipeline, чтобы
        // каждый pipeline не размерялся на весь аккаунт.
        auto partitioned_portfolio = pre_trade_portfolio;
        if (num_pipelines_ > 1) {
            double fraction = 1.0 / static_cast<double>(num_pipelines_);
            partitioned_portfolio.total_capital *= fraction;
            partitioned_portfolio.available_capital *= fraction;
        }

        auto sizing = portfolio_allocator_->compute_size_v2(
            intent, partitioned_portfolio, alloc_ctx, combined_size_mult);

        if (!sizing.approved || sizing.approved_quantity.get() <= 0.0) {
            ++diag_sizing_block_;
            logger_->info("pipeline", "Размер позиции не одобрен аллокатором",
                {{"symbol", symbol_.get()},
                 {"approved", sizing.approved ? "true" : "false"},
                 {"qty", std::to_string(sizing.approved_quantity.get())},
                 {"reason", sizing.reduction_reason},
                 {"total_capital", std::to_string(pre_trade_portfolio.total_capital)},
                 {"available_capital", std::to_string(pre_trade_portfolio.available_capital)},
                 {"min_trade_usdt", std::to_string(exchange_rules_.min_trade_usdt)},
                 {"diag_count", std::to_string(diag_sizing_block_)}});
            // Уведомить стратегию — не переспамливать вход
            for (const auto& s : strategy_registry_->active()) {
                s->notify_entry_rejected();
            }
            return;
        }

        // 11. Проверка риск-движком — ОБЯЗАТЕЛЬНА для новых позиций
        risk_decision = risk_engine_->evaluate(
            intent, sizing, pre_trade_portfolio, snapshot, exec_alpha, uncertainty);

        if (risk_decision.verdict == risk::RiskVerdict::Denied ||
            risk_decision.verdict == risk::RiskVerdict::Throttled) {
            ++diag_risk_block_;
            logger_->warn("pipeline", "Сделка отклонена риск-движком",
                {{"verdict", risk::to_string(risk_decision.verdict)},
                 {"reasons", std::to_string(risk_decision.reasons.size())},
                 {"summary", risk_decision.summary},
                 {"diag_count", std::to_string(diag_risk_block_)}});
            // Уведомить стратегию — не переспамливать вход
            for (const auto& s : strategy_registry_->active()) {
                s->notify_entry_rejected();
            }
            return;
        }
    }

    // 11a. Smart TWAP: разбиваем крупные ордера на слайсы.
    // Execution Alpha является основным арбитром: если он рекомендует нарезку
    // (slice_plan.has_value()), это учитывается в приоритете над оценкой TWAP.
    // TWAP trigger оценивается по APPROVED quantity (после risk), а не по suggested.
    //
    // ВАЖНО: TWAP имеет смысл только для крупных ордеров ($500+).
    // При малом капитале ($5-50) TWAP создаёт N×fees, dust-позиции и
    // rejected-слайсы из-за min notional. Проверяем нотионал ДО TWAP.
    const double order_notional_for_twap = risk_decision.approved_quantity.get() * snapshot.mid_price.get();
    const double kMinTwapNotionalPipeline = 500.0;  // Не нарезаем ордера < $500
    const bool twap_eligible = (order_notional_for_twap >= kMinTwapNotionalPipeline);

    const bool exec_alpha_wants_twap = twap_eligible && exec_alpha.slice_plan.has_value();
    bool twap_independently_triggered = false;
    if (twap_eligible && twap_executor_) {
        auto twap_check_intent = intent;
        twap_check_intent.suggested_quantity = risk_decision.approved_quantity;
        twap_independently_triggered = twap_executor_->should_use_twap(twap_check_intent, snapshot);
    }

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

        const auto compute_adversarial_severity = [&]() {
            double severity = 0.0;
            int triggers = 0;

            const auto raise = [&](double candidate) {
                const double clamped = std::clamp(candidate, 0.0, 1.0);
                if (clamped > severity + 1e-9) {
                    severity = clamped;
                }
                if (clamped > 0.0) {
                    ++triggers;
                }
            };

            switch (world.state) {
                case world_model::WorldState::ToxicMicrostructure:
                    raise(0.95);
                    break;
                case world_model::WorldState::LiquidityVacuum:
                    raise(0.85);
                    break;
                case world_model::WorldState::ExhaustionSpike:
                    raise(0.65);
                    break;
                case world_model::WorldState::FragileBreakout:
                    raise(0.35);
                    break;
                case world_model::WorldState::PostShockStabilization:
                    raise(0.20);
                    break;
                default:
                    break;
            }

            if (snapshot.microstructure.spread_valid) {
                if (snapshot.microstructure.spread_bps >= 50.0) {
                    raise(0.85);
                } else if (snapshot.microstructure.spread_bps >= 20.0) {
                    raise(0.55);
                } else if (snapshot.microstructure.spread_bps >= 10.0) {
                    raise(0.25);
                }
            }

            if (snapshot.microstructure.instability_valid) {
                raise(std::clamp((snapshot.microstructure.book_instability - 0.30) / 0.50, 0.0, 1.0) * 0.85);
            }

            if (snapshot.microstructure.vpin_valid) {
                const double vpin_severity = std::clamp(
                    (snapshot.microstructure.vpin - 0.55) / 0.35, 0.0, 1.0);
                if (snapshot.microstructure.vpin_toxic) {
                    raise(std::max(0.70, vpin_severity));
                } else {
                    raise(vpin_severity * 0.60);
                }
            }

            if (snapshot.microstructure.trade_flow_valid) {
                raise(std::clamp((snapshot.microstructure.aggressive_flow - 0.60) / 0.40, 0.0, 1.0) * 0.80);
            }

            if (ml_snapshot_.fingerprint_sample_count > 0 && ml_snapshot_.fingerprint_edge < 0.0) {
                raise(std::clamp(-ml_snapshot_.fingerprint_edge / 0.25, 0.0, 1.0) * 0.80);
            }

            if (triggers > 1) {
                double bonus = 0.05 * std::log2(static_cast<double>(triggers));
                severity = std::min(1.0, severity + bonus);
            }

            return std::clamp(severity, 0.0, 1.0);
        };

        // Вычисляем оптимальное плечо на основе рыночных условий
        leverage::LeverageContext lev_ctx;
        lev_ctx.regime = regime.label;
        lev_ctx.uncertainty = uncertainty.level;
        lev_ctx.atr_normalized = atr_normalized;
        lev_ctx.drawdown_pct = pre_trade_portfolio.pnl.current_drawdown_pct;
        lev_ctx.adversarial_severity = compute_adversarial_severity();
        lev_ctx.conviction = intent.conviction;
        lev_ctx.funding_rate = current_funding_rate_;
        lev_ctx.position_side = intent.position_side;
        lev_ctx.entry_price = snapshot.mid_price.get();

        auto lev_decision = leverage_engine_->compute_leverage(lev_ctx);

        // Проверяем, что плечо безопасно (достаточный буфер до ликвидации)
        if (!lev_decision.is_safe) {
            ++diag_leverage_block_;
            logger_->warn("pipeline", "Ликвидационный буфер недостаточен — сделка отклонена",
                {{"leverage", std::to_string(lev_decision.leverage)},
                 {"liquidation_buffer_pct", std::to_string(lev_decision.liquidation_buffer_pct)},
                 {"min_buffer_pct", std::to_string(config_.futures.liquidation_buffer_pct)},
                 {"symbol", symbol_.get()},
                 {"diag_count", std::to_string(diag_leverage_block_)}});
            return;
        }

        // Для открывающего ордера синхронизируем leverage с execution engine.
        // В production дополнительно устанавливаем плечо на бирже, в paper только симулируем.
        if (intent.trade_side == TradeSide::Open) {
            std::string hold_side = (intent.position_side == PositionSide::Long) ? "long" : "short";
            int& last_applied = (intent.position_side == PositionSide::Long)
                ? last_applied_leverage_long_ : last_applied_leverage_short_;
            const int min_delta = config_.futures.leverage_engine.min_leverage_change_delta;

            if (futures_submitter_ && std::abs(lev_decision.leverage - last_applied) >= min_delta) {
                bool set_lev_ok = futures_submitter_->set_leverage(symbol_, lev_decision.leverage, hold_side);
                if (!set_lev_ok) {
                    logger_->error("pipeline",
                        "Не удалось установить плечо на бирже — ордер отклонён. "
                        "Торговля с неверным плечом может привести к ликвидации.",
                        {{"leverage", std::to_string(lev_decision.leverage)},
                         {"hold_side", hold_side},
                         {"symbol", symbol_.get()}});
                    last_order_time_ns_ = clock_->now().get();
                    return;
                }
                last_applied = lev_decision.leverage;
            }

            // v11: синхронизируем leverage в execution engine для корректного reserve_cash
            if (execution_engine_) {
                execution_engine_->set_leverage(static_cast<double>(lev_decision.leverage));
            }

            logger_->debug("pipeline",
                futures_submitter_ ? "Плечо установлено на бирже" : "Плечо смоделировано в PAPER",
                {{"leverage", std::to_string(lev_decision.leverage)},
                 {"hold_side", hold_side},
                 {"rationale", lev_decision.rationale}});
        }
    }
    }

    // 12. Исполнение ордера

    {
        auto exec_stage = tick_profiler.scope(TickStageProfiler::Stage::Exec);

    // 12a. Anti-fingerprinting: добавляем шум к размеру ордера (±2%).
    // Предотвращает обнаружение бота по повторяющимся "круглым" размерам.
    // Не применяется к стоп-лоссам и закрытиям (нужна точная сумма).
    const bool is_entry_order = (intent.trade_side == TradeSide::Open);
    if (is_entry_order) {
        double approved = risk_decision.approved_quantity.get();
        double noisy_qty = apply_quantity_noise(approved, 2.0);
        // КРИТИЧНО: никогда не превышаем одобренный risk engine размер.
        // Шум может только УМЕНЬШИТЬ размер, но не увеличить его.
        noisy_qty = std::min(noisy_qty, approved);
        noisy_qty = std::max(noisy_qty, 0.0);
        risk_decision.approved_quantity = Quantity(noisy_qty);
    }

    // 12b. Anti-fingerprinting: order timing jitter (50-300ms).
    // Предотвращает обнаружение паттернов по точному времени подачи ордеров.
    // Scalping-safe: задержка < рыночного impact на 1-минутных свечах.
    {
        int jitter_ms = generate_order_jitter_ms(50, 300);
        std::this_thread::sleep_for(std::chrono::milliseconds(jitter_ms));
    }

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
        // Фьючерсы: trade_side==Open (и Long, и Short)
        const bool is_new_position = (intent.trade_side == TradeSide::Open);

        if (is_new_position) {
            current_position_strategy_    = intent.strategy_id;
            current_position_side_        = intent.position_side;
            current_position_conviction_  = intent.conviction;
            current_entry_thompson_action_ = thompson_action;
            // A4 fix: сохраняем world state при входе для feedback
            current_entry_world_state_ = world.state;
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

            // Feedback: уведомить стратегию что позиция открыта → EntrySent → PositionOpen
            for (const auto& s : strategy_registry_->active()) {
                s->notify_position_opened(
                    snapshot.mid_price.get(),
                    risk_decision.approved_quantity.get(),
                    intent.side,
                    intent.position_side);
            }
        }

        // Сброс trailing state при полном закрытии позиции
        // Фьючерсы: trade_side==Close
        const bool is_position_close = (intent.trade_side == TradeSide::Close && is_closing_position);

        if (is_position_close) {
            // Записываем результат fingerprint и байесовское наблюдение
            if (fingerprinter_ && last_entry_fingerprint_) {
                double norm_pnl = (closing_pnl > 0) ? 1.0 : -1.0;
                fingerprinter_->record_outcome(*last_entry_fingerprint_, norm_pnl);
                last_entry_fingerprint_.reset();
            }
            if (bayesian_adapter_) {
                ml::ParameterObservation obs;
                // Normalize reward to [-1, +1]: use PnL percentage clamped by
                // max_loss_per_trade_pct as the reference scale (R-multiple idea).
                const double risk_ref = std::max(config_.trading_params.max_loss_per_trade_pct, 0.5);
                obs.reward = std::clamp(closing_pnl_pct / risk_ref, -1.0, 1.0);
                obs.regime = regime.detailed;
                bayesian_adapter_->record_observation("global", obs);
            }

            // Thompson Sampling: бинарная награда для бандита
            // Используем действие, выбранное при ВХОДЕ в позицию, а не текущее
            if (thompson_sampler_) {
                double ts_reward = (closing_pnl > 0) ? 1.0 : -1.0;
                thompson_sampler_->record_reward(current_entry_thompson_action_, ts_reward);
            }

            // A4 fix: feedback в world model — замыкаем цикл обратной связи
            if (world_model_) {
                world_model::WorldStateFeedback wfb;
                wfb.state = current_entry_world_state_;
                wfb.strategy_id = current_position_strategy_;
                wfb.pnl_bps = closing_pnl_pct * 100.0;  // pct → bps
                wfb.slippage_bps = current_position_slippage_bps_;
                wfb.max_adverse_excursion_bps = current_max_adverse_excursion_bps_;
                wfb.was_profitable = (closing_pnl > 0.0);
                wfb.timestamp = clock_->now();
                world_model_->record_feedback(wfb);
            }

            // Записываем результат в risk engine
            risk_engine_->record_trade_result(closing_pnl < 0.0);
            risk_engine_->record_trade_close(current_position_strategy_, symbol_, closing_pnl);

            // Rolling statistics для адаптивного Kelly Criterion
            record_trade_for_stats(closing_pnl_pct);

            // Обновляем edge stats в leverage engine для Kelly-bounded плеча
            if (leverage_engine_) {
                leverage_engine_->update_edge_stats(rolling_win_rate(), rolling_win_loss_ratio());
            }

            // Feedback: уведомить стратегию что позиция закрыта → PositionManaging → Cooldown
            for (const auto& s : strategy_registry_->active()) {
                s->notify_position_closed();
            }

            reset_trailing_state();
        }
    } else {
        ++consecutive_rejections_;

        // Если ордер на закрытие позиции не исполнен — снимаем блокировку,
        // чтобы на следующем тике trailing/TP/time_exit могли повторить попытку.
        if (is_closing_position) {
            close_order_pending_ = false;
        }

        // Экспоненциальный backoff: cooldown × 2^(rejections-1), макс 10 минут
        int64_t base_cooldown = static_cast<int64_t>(config_.trading_params.order_cooldown_seconds)
                               * 1'000'000'000LL;
        int64_t backoff = base_cooldown * (1LL << std::min(consecutive_rejections_, 8));
        backoff = std::min(backoff, kMaxRejectionBackoffNs);
        last_order_time_ns_ = clock_->now().get() + backoff - base_cooldown;
        logger_->warn("pipeline", "Ордер не исполнен",
            {{"side", intent.side == Side::Buy ? "BUY" : "SELL"},
             {"symbol", symbol_.get()},
             {"is_close", is_closing_position ? "true" : "false"},
             {"consecutive_rejections", std::to_string(consecutive_rejections_)},
             {"backoff_sec", std::to_string(backoff / 1'000'000'000LL)}});
        // Уведомить стратегию — не переспамливать вход
        for (const auto& s : strategy_registry_->active()) {
            s->notify_entry_rejected();
        }
    }
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
         {"imb", fmt(snap.microstructure.book_imbalance_5, 3)},
         {"bsr", fmt(snap.microstructure.buy_sell_ratio, 3)},
         {"regime", std::string(to_string(regime.label))},
         {"world", std::string(world_model::to_string(world.state))},
         {"positions", std::to_string(portfolio_->snapshot().exposure.open_positions_count)}});
}

void TradingPipeline::update_current_mae(double current_price, bool is_long) {
    if (current_price <= 0.0) return;
    // MAE в бп относительно цены входа
    double entry = 0.0;
    auto pos = portfolio_->get_position(symbol_, current_position_side_);
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

// (check_quote_freshness removed — freshness gate inlined in on_feature_snapshot)

// ==================== Phase 1/2/4: Periodic Tasks ====================

void TradingPipeline::run_periodic_tasks(int64_t now_ns) {
    run_order_watchdog(now_ns);
    run_continuous_reconciliation(now_ns);

    // Daily reset: сбрасываем intraday drawdown, daily PnL, loss streaks при смене UTC-дня.
    // Без этого INTRADAY_DRAWDOWN блокирует торговлю навсегда после достижения лимита.
    {
        // Вычисляем текущий UTC-день из наносекундного timestamp
        int64_t seconds = now_ns / 1'000'000'000LL;
        int current_day = static_cast<int>(seconds / 86400LL);
        if (last_daily_reset_day_ == 0) {
            last_daily_reset_day_ = current_day;
        } else if (current_day > last_daily_reset_day_) {
            last_daily_reset_day_ = current_day;
            if (risk_engine_) {
                risk_engine_->reset_daily();
                logger_->info("pipeline", "Daily reset выполнен: drawdown, PnL, loss streaks сброшены",
                    {{"symbol", symbol_.get()},
                     {"utc_day", std::to_string(current_day)}});
            }
            if (portfolio_) {
                portfolio_->reset_daily();
            }
        }
    }

    // Периодическая синхронизация баланса с биржей (каждые 5 минут).
    // Обнаруживает расхождения после ручных сделок, сбоев API, или пропущенных fill'ов.
    if (rest_client_ &&
        (last_balance_sync_ns_ == 0 || (now_ns - last_balance_sync_ns_) >= kBalanceSyncIntervalNs
         || reconciliation_needs_resync_)) {
        if (reconciliation_needs_resync_) {
            logger_->info("pipeline", "Ресинхронизация с биржей (reconciliation флаг)",
                {{"symbol", symbol_.get()}});
            reconciliation_needs_resync_ = false;
        }
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
        // Передать funding rate в risk engine для FundingRateCostCheck
        if (risk_engine_) {
            risk_engine_->set_funding_rate(new_rate);
        }
    }

    // Экспорт latency-метрик каждые 60 секунд
    if (latency_tracker_) {
        static constexpr int64_t kLatencyEmitIntervalNs = 60'000'000'000LL;
        if ((now_ns - last_latency_emit_ns_) >= kLatencyEmitIntervalNs) {
            last_latency_emit_ns_ = now_ns;
            latency_tracker_->emit_metrics();
        }
    }

    // Обновление reference prices для CorrelationMonitor (BTC/ETH)
    if (correlation_monitor_ && rest_client_ &&
        (last_reference_price_update_ns_ == 0 ||
         (now_ns - last_reference_price_update_ns_) >= kReferencePriceIntervalNs)) {
        last_reference_price_update_ns_ = now_ns;
        update_reference_prices();
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

    // 1. Ордерная reconciliation (каждый цикл)
    auto active_orders = execution_engine_->active_orders();
    if (!active_orders.empty()) {
        auto result = reconciliation_engine_->reconcile_active_orders(active_orders);

        if (!result.mismatches.empty()) {
            logger_->warn("reconciliation", "Обнаружены расхождения в runtime reconciliation",
                {{"mismatches", std::to_string(result.mismatches.size())},
                 {"symbol", symbol_.get()}});

            // ИСПРАВЛЕНИЕ H6: исполняем ResolutionAction, а не только логируем
            for (const auto& mismatch : result.mismatches) {
                logger_->warn("reconciliation", "Расхождение",
                    {{"type", reconciliation::to_string(mismatch.type)},
                     {"order_id", mismatch.order_id.get()},
                     {"action", reconciliation::to_string(mismatch.resolved_by)},
                     {"resolved", mismatch.resolved ? "true" : "false"}});

                if (!mismatch.resolved) continue;

                switch (mismatch.resolved_by) {
                    case reconciliation::ResolutionAction::CancelOnExchange: {
                        // Orphan-ордер на бирже — отменяем через execution engine
                        auto cancel_result = execution_engine_->cancel(mismatch.order_id);
                        if (cancel_result) {
                            logger_->info("reconciliation",
                                "Orphan-ордер отменён на бирже",
                                {{"order_id", mismatch.order_id.get()}});
                        } else {
                            logger_->error("reconciliation",
                                "Не удалось отменить orphan-ордер",
                                {{"order_id", mismatch.order_id.get()}});
                        }
                        break;
                    }
                    case reconciliation::ResolutionAction::UpdateLocalState: {
                        // Ордер отсутствует на бирже — отменяем локально
                        execution_engine_->cancel(mismatch.order_id);
                        logger_->info("reconciliation",
                            "Локальное состояние ордера обновлено (отмена)",
                            {{"order_id", mismatch.order_id.get()}});
                        break;
                    }
                    case reconciliation::ResolutionAction::SyncFromExchange: {
                        // Состояние или qty расходится — запланировать ресинхронизацию
                        reconciliation_needs_resync_ = true;
                        logger_->info("reconciliation",
                            "Запланирована ресинхронизация с биржей",
                            {{"order_id", mismatch.order_id.get()}});
                        break;
                    }
                    default:
                        break;
                }
            }
        }
    }

    // 2. Позиционная + балансовая reconciliation (каждые 5 минут — тяжёлый REST)
    static constexpr int64_t kPosBalReconcileIntervalNs = 300'000'000'000LL; // 5 min
    if (last_pos_balance_reconciliation_ns_ == 0 ||
        (now_ns - last_pos_balance_reconciliation_ns_) >= kPosBalReconcileIntervalNs) {
        last_pos_balance_reconciliation_ns_ = now_ns;

        auto snap = portfolio_->snapshot();
        double local_cash = snap.cash.total_cash;
        auto pos_result = reconciliation_engine_->reconcile_positions_and_balance(
            snap.positions, local_cash);

        if (!pos_result.mismatches.empty()) {
            logger_->warn("reconciliation", "Расхождения позиций/баланса",
                {{"mismatches", std::to_string(pos_result.mismatches.size())},
                 {"symbol", symbol_.get()}});

            for (const auto& mismatch : pos_result.mismatches) {
                logger_->warn("reconciliation", "Расхождение",
                    {{"type", reconciliation::to_string(mismatch.type)},
                     {"description", mismatch.description},
                     {"action", reconciliation::to_string(mismatch.resolved_by)},
                     {"resolved", mismatch.resolved ? "true" : "false"}});

                if (!mismatch.resolved) continue;

                // ИСПРАВЛЕНИЕ H6: исполняем действия по позиционным расхождениям
                switch (mismatch.resolved_by) {
                    case reconciliation::ResolutionAction::SyncFromExchange: {
                        // Позиция/баланс на бирже отличается — принимаем данные биржи
                        reconciliation_needs_resync_ = true;
                        logger_->info("reconciliation",
                            "Запланирована позиционная ресинхронизация с биржей",
                            {{"symbol", symbol_.get()},
                             {"type", reconciliation::to_string(mismatch.type)}});
                        break;
                    }
                    case reconciliation::ResolutionAction::UpdateLocalState: {
                        // Позиция в системе, но нет на бирже — закрываем локально
                        if (mismatch.type == reconciliation::MismatchType::PositionExistsOnlyLocally) {
                            // A2 fix: используем leg-aware close если known side
                            if (mismatch.position_side.has_value()) {
                                portfolio_->close_position(symbol_, *mismatch.position_side, Price(0.0), 0.0);
                                logger_->warn("reconciliation",
                                    "Локальная позиция закрыта leg-aware (нет на бирже)",
                                    {{"symbol", symbol_.get()},
                                     {"side", *mismatch.position_side == PositionSide::Long ? "Long" : "Short"}});
                            } else {
                                portfolio_->close_position(symbol_, Price(0.0), 0.0);
                                logger_->warn("reconciliation",
                                    "Локальная позиция закрыта (нет на бирже)",
                                    {{"symbol", symbol_.get()}});
                            }
                            if (hedge_active_) reset_hedge_state();
                            reset_trailing_state();
                        }
                        break;
                    }
                    case reconciliation::ResolutionAction::CloseOnExchange: {
                        // Orphan-позиция — ресинхронизируем чтобы захватить и управлять
                        reconciliation_needs_resync_ = true;
                        logger_->warn("reconciliation",
                            "Orphan-позиция на бирже — запуск ресинхронизации",
                            {{"symbol", symbol_.get()}});
                        break;
                    }
                    default:
                        break;
                }
            }
        } else if (tick_count_ % 500 == 0) {
            logger_->debug("reconciliation", "Позиции/баланс reconciliation OK",
                {{"positions", std::to_string(snap.positions.size())},
                 {"cash", std::to_string(local_cash)}});
        }
    }
}

// ============================================================
// Rolling Trade Statistics
// ============================================================

void TradingPipeline::record_trade_for_stats(double pnl_pct) {
    trade_history_.push_back(TradeOutcome{
        .pnl_pct = pnl_pct,
        .won     = (pnl_pct > 0.0)
    });
    // Поддерживаем скользящее окно фиксированного размера
    while (trade_history_.size() > kTradeStatsWindowSize) {
        trade_history_.pop_front();
    }
}

double TradingPipeline::rolling_win_rate() const {
    if (trade_history_.empty()) {
        // Консервативная оценка по умолчанию (до накопления статистики)
        return 0.45;
    }
    int wins = 0;
    for (const auto& t : trade_history_) {
        if (t.won) ++wins;
    }
    return static_cast<double>(wins) / static_cast<double>(trade_history_.size());
}

double TradingPipeline::rolling_win_loss_ratio() const {
    if (trade_history_.empty()) {
        // Консервативная оценка по умолчанию
        return 1.2;
    }
    double total_wins = 0.0;
    double total_losses = 0.0;
    int win_count = 0;
    int loss_count = 0;
    for (const auto& t : trade_history_) {
        if (t.won) {
            total_wins += t.pnl_pct;
            ++win_count;
        } else {
            total_losses += std::abs(t.pnl_pct);
            ++loss_count;
        }
    }
    // Средний выигрыш / средний проигрыш
    double avg_win = (win_count > 0) ? (total_wins / win_count) : 0.0;
    double avg_loss = (loss_count > 0) ? (total_losses / loss_count) : 1.0;
    if (avg_loss < 1e-9) {
        // Все сделки прибыльные — возвращаем высокий, но конечный ratio
        return 5.0;
    }
    return avg_win / avg_loss;
}

// ==================== Correlation Monitor: Reference Price Feed ====================

void TradingPipeline::update_reference_prices() {
    if (!correlation_monitor_ || !rest_client_) return;

    bool expected = false;
    if (!reference_price_fetch_in_flight_ ||
        !reference_price_fetch_in_flight_->compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) {
        return;
    }

    auto rest_client = rest_client_;
    auto correlation_monitor = correlation_monitor_;
    auto logger = logger_;
    auto in_flight = reference_price_fetch_in_flight_;
    const std::string symbol = symbol_.get();

    std::thread([rest_client = std::move(rest_client),
                 correlation_monitor = std::move(correlation_monitor),
                 logger = std::move(logger),
                 in_flight = std::move(in_flight),
                 symbol]() {
        static const std::vector<std::string> kReferenceAssets = {"BTCUSDT", "ETHUSDT"};

        for (const auto& asset : kReferenceAssets) {
            if (symbol == asset) continue;

            try {
                std::string endpoint = "/api/v2/mix/market/ticker";
                std::string params = "productType=USDT-FUTURES&symbol=" + asset;

                auto resp = rest_client->get(endpoint, params);
                if (!resp.success) continue;

                auto doc = boost::json::parse(resp.body);
                auto& obj = doc.as_object();
                if (obj.at("code").as_string() != "00000") continue;

                auto& data = obj.at("data").as_array();
                if (data.empty()) continue;

                auto& ticker = data[0].as_object();
                auto last_price_it = ticker.find("lastPr");
                if (last_price_it == ticker.end()) continue;

                double price = std::stod(std::string(last_price_it->value().as_string()));
                if (price > 0.0) {
                    correlation_monitor->on_reference_tick(asset, price);
                }
            } catch (const std::exception& e) {
                if (logger) {
                    logger->debug("pipeline", "Reference price fetch failed for " + asset,
                        {{"error", e.what()}});
                }
            }
        }

        in_flight->store(false, std::memory_order_release);
    }).detach();
}

} // namespace tb::pipeline
