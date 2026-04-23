#pragma once

/// @file trading_pipeline.hpp
/// @brief Торговый pipeline — связывает все подсистемы в единый цикл обработки
///
/// Поток данных:
/// MarketDataGateway → FeatureSnapshot → WorldModel → Regime → Uncertainty
/// → Strategies → StrategyAllocator → Decision → PortfolioAllocator → Risk → Execution

#include "market_data/market_data_gateway.hpp"
#include "features/feature_engine.hpp"
#include "common/exchange_rules.hpp"
#include "features/advanced_features.hpp"
#include "order_book/order_book.hpp"
#include "indicators/indicator_engine.hpp"
#include "world_model/world_model_engine.hpp"
#include "regime/regime_engine.hpp"
#include "uncertainty/uncertainty_engine.hpp"
#include "strategy/strategy_registry.hpp"
#include "strategy/strategy_engine.hpp"
#include "strategy_allocator/strategy_allocator.hpp"
#include "decision/decision_aggregation_engine.hpp"
#include "portfolio_allocator/portfolio_allocator.hpp"
#include "portfolio/portfolio_engine.hpp"
#include "execution_alpha/execution_alpha_engine.hpp"
#include "opportunity_cost/opportunity_cost_engine.hpp"
#include "risk/risk_engine.hpp"
#include "execution/execution_engine.hpp"
#include "execution/twap_executor.hpp"
#include "config/config_types.hpp"
#include "security/secret_provider.hpp"
#include "logging/logger.hpp"
#include "clock/clock.hpp"
#include "metrics/metrics_registry.hpp"
#include "exchange/bitget/bitget_rest_client.hpp"
#include "exchange/bitget/bitget_futures_order_submitter.hpp"
#include "exchange/bitget/bitget_private_ws_client.hpp"
#include "ml/bayesian_adapter.hpp"
#include "ml/entropy_filter.hpp"
#include "ml/microstructure_fingerprint.hpp"
#include "ml/liquidation_cascade.hpp"
#include "ml/correlation_monitor.hpp"
#include "ml/thompson_sampler.hpp"
#include "ml/ml_signal_types.hpp"
#include "pipeline/pipeline_tick_context.hpp"
#include "pipeline/pipeline_stage_result.hpp"
#include "pipeline/pipeline_latency_tracker.hpp"
#include "pipeline/order_watchdog.hpp"
#include "pipeline/exit_orchestrator.hpp"
#include "pipeline/hedge_pair_manager.hpp"
#include "pipeline/market_reaction_engine.hpp"
#include "pipeline/dual_leg_manager.hpp"
#include "reconciliation/reconciliation_engine.hpp"
#include "leverage/leverage_engine.hpp"
#include "exchange/bitget/bitget_futures_query_adapter.hpp"
#include "persistence/persistence_layer.hpp"
#include "telemetry/research_telemetry.hpp"
#include "telemetry/file_telemetry_sink.hpp"
#include "telemetry/incident_detector.hpp"
#include "telemetry/observability_panels.hpp"
#include <memory>
#include <atomic>
#include <mutex>
#include <thread>
#include <optional>
#include <deque>
#include <unordered_map>
#include <string>
#include <limits>

namespace tb::pipeline {

class TradingPipeline {
public:
    /// @param symbol Торговый символ (например "BTCUSDT").
    ///        Если пустой — используется "BTCUSDT" по умолчанию.
    /// @param shared_portfolio Общий движок портфеля (для account-level state).
    ///        Если nullptr — создаётся локальный (backward compat).
    TradingPipeline(
        const config::AppConfig& config,
        std::shared_ptr<security::ISecretProvider> secret_provider,
        std::shared_ptr<logging::ILogger> logger,
        std::shared_ptr<clock::IClock> clock,
        std::shared_ptr<metrics::IMetricsRegistry> metrics,
        const std::string& symbol = "",
        std::shared_ptr<portfolio::IPortfolioEngine> shared_portfolio = nullptr
    );

    ~TradingPipeline();

    /// Запустить все подсистемы (вызывается supervisor)
    bool start();

    /// Получить символ, которым торгует этот pipeline
    const Symbol& symbol() const { return symbol_; }

    /// Установить точность ордеров для символа (из PairScanner exchange info)
    void set_symbol_precision(int quantity_precision, int price_precision);

    /// Установить правила инструмента (из PairScanner exchange info)
    void set_exchange_rules(const ExchangeSymbolRules& rules);

    /// Установить количество параллельных pipeline (для корректного деления капитала)
    void set_num_pipelines(int n) { num_pipelines_ = std::max(1, n); }

    /// Остановить все подсистемы (вызывается supervisor)
    void stop();

    /// Подключён ли WebSocket
    bool is_connected() const;

    /// Есть ли открытая позиция (нельзя убивать pipeline с позицией)
    bool has_open_position() const;

    /// Pipeline простаивает (нет сделок и нет сигналов дольше threshold_ns)
    bool is_idle(int64_t threshold_ns) const;

    /// Время последней успешной торговой активности (ns epoch)
    int64_t last_activity_time_ns() const { return last_activity_ns_.load(std::memory_order_relaxed); }

private:
    /// Callback от MarketDataGateway при готовом FeatureSnapshot
    void on_feature_snapshot(features::FeatureSnapshot snapshot);

    /// Вывести статус в консоль (человекочитаемый)
    void print_status(const features::FeatureSnapshot& snap,
                      const world_model::WorldModelSnapshot& world,
                      const regime::RegimeSnapshot& regime);

    // Конфигурация
    config::AppConfig config_;
    Symbol symbol_;
    int num_pipelines_{1};  ///< Кол-во параллельных pipeline (для деления капитала)

    // Инфраструктура
    std::shared_ptr<security::ISecretProvider> secret_provider_;
    std::shared_ptr<logging::ILogger> logger_;
    std::shared_ptr<clock::IClock> clock_;
    std::shared_ptr<metrics::IMetricsRegistry> metrics_;

    // Рыночные данные
    std::shared_ptr<indicators::IndicatorEngine> indicator_engine_;
    std::shared_ptr<features::FeatureEngine> feature_engine_;
    std::shared_ptr<order_book::LocalOrderBook> order_book_;
    std::shared_ptr<market_data::MarketDataGateway> gateway_;

    // Аналитический слой
    std::shared_ptr<world_model::IWorldModelEngine> world_model_;
    std::shared_ptr<regime::IRegimeEngine> regime_engine_;
    std::shared_ptr<uncertainty::IUncertaintyEngine> uncertainty_engine_;

    // Стратегии
    std::shared_ptr<strategy::StrategyRegistry> strategy_registry_;
    std::shared_ptr<strategy_allocator::IStrategyAllocator> strategy_allocator_;
    std::shared_ptr<decision::IDecisionAggregationEngine> decision_engine_;

    // Исполнение
    std::shared_ptr<portfolio::IPortfolioEngine> portfolio_;
    std::shared_ptr<portfolio_allocator::IPortfolioAllocator> portfolio_allocator_;
    std::shared_ptr<execution_alpha::IExecutionAlphaEngine> execution_alpha_;
    std::shared_ptr<opportunity_cost::IOpportunityCostEngine> opportunity_cost_engine_;
    std::shared_ptr<risk::IRiskEngine> risk_engine_;

    /// Адаптивный движок управления кредитным плечом (USDT-M фьючерсы)
    std::shared_ptr<leverage::LeverageEngine> leverage_engine_;

    std::shared_ptr<execution::ExecutionEngine> execution_engine_;

    /// Smart TWAP — разбиение крупных ордеров на адаптивные слайсы
    std::shared_ptr<execution::SmartTwapExecutor> twap_executor_;

    /// Продвинутый движок features (CUSUM, VPIN, Volume Profile, Time-of-Day)
    std::shared_ptr<features::AdvancedFeatureEngine> advanced_features_;

    /// Байесовский адаптер параметров стратегий
    std::shared_ptr<ml::BayesianAdapter> bayesian_adapter_;
    /// Фильтр энтропии сигналов
    std::shared_ptr<ml::EntropyFilter> entropy_filter_;
    /// Microstructure fingerprinting
    std::shared_ptr<ml::MicrostructureFingerprinter> fingerprinter_;
    /// Последний fingerprint при входе в позицию (для записи результата)
    std::optional<ml::MicroFingerprint> last_entry_fingerprint_;

    /// Детектор ликвидационных каскадов
    std::shared_ptr<ml::LiquidationCascadeDetector> cascade_detector_;
    /// Монитор мульти-активных корреляций
    std::shared_ptr<ml::CorrelationMonitor> correlation_monitor_;
    /// Thompson Sampling для оптимизации момента входа
    std::shared_ptr<ml::ThompsonSampler> thompson_sampler_;
    ml::MlSignalSnapshot ml_snapshot_;

    /// Ожидающий вход (Thompson Sampling решил ждать)
    struct PendingEntry {
        strategy::TradeIntent intent;          ///< Исходный торговый сигнал
        int wait_periods_remaining{0};         ///< Оставшиеся периоды ожидания
        ml::EntryAction action{ml::EntryAction::EnterNow}; ///< Выбранное действие
    };
    std::optional<PendingEntry> pending_entry_;

    /// Consecutive Wait1 actions from Thompson Sampling (triggers rotation when too many)
    int consecutive_wait1_count_{0};
    /// Threshold: if Thompson selects Wait1 this many times, force pipeline idle for rotation
    static constexpr int kMaxConsecutiveWait1 = 20;

    std::atomic<bool> running_{false};
    std::mutex pipeline_mutex_;
    uint64_t tick_count_{0};

    /// Время последней торговой активности (ордер, non-veto сигнал)
    std::atomic<int64_t> last_activity_ns_{0};

    /// Время последнего принятого тика (для backlog detection)
    int64_t last_tick_ingress_ns_{0};

    /// Время последнего отправленного ордера (для cooldown между сделками)
    int64_t last_order_time_ns_{0};

    /// Счётчик последовательных отклонений ордеров (для экспоненциального backoff)
    int consecutive_rejections_{0};

    /// Последнее установленное плечо на бирже (для debounce)
    int last_applied_leverage_long_{0};
    int last_applied_leverage_short_{0};

    /// Диагностика: счётчики блокировок для каждого gate (логируем первые N)
    int diag_decision_block_{0};
    int diag_thompson_block_{0};
    int diag_htf_block_{0};
    int diag_cooldown_block_{0};
    int diag_opp_cost_block_{0};
    int diag_sizing_block_{0};
    int diag_risk_block_{0};
    int diag_pnl_gate_block_{0};
    int diag_leverage_block_{0};
    int diag_slow_tick_breakdown_{0};
    int diag_fingerprint_block_{0};
    int diag_conviction_block_{0};
    int diag_rr_block_{0};
    int diag_rsi_extreme_block_{0};
    static constexpr int kDiagLogLimit = 10; ///< Логируем первые N блокировок каждого gate
    /// Максимальный backoff между ордерами: 2 минуты (120 секунд)
    static constexpr int64_t kMaxRejectionBackoffNs = 120'000'000'000LL;

    /// Время последнего стоп-лосс ордера (отдельный cooldown от обычных сделок).
    /// Стоп-лосс — экстренный механизм, его нельзя блокировать обычным cooldown'ом,
    /// иначе позиция остаётся открытой и убыток растёт.
    int64_t last_stop_loss_time_ns_{0};
    /// Cooldown между попытками стоп-лосса: 5 секунд.
    /// Достаточно для предотвращения спама на биржу, но не блокирует экстренное закрытие.
    static constexpr int64_t kStopLossCooldownNs = 5'000'000'000LL;

    /// Timestamp of the most recent position-open fill (ns since epoch).
    /// Used to suppress phantom-position cleanup for a grace period after fill,
    /// giving the exchange API time to reflect the new position.
    int64_t last_position_fill_ns_{0};
    /// Grace period for phantom detection after a fill: 5 seconds.
    static constexpr int64_t kPhantomGracePeriodNs = 5'000'000'000LL;

    /// REST клиент для запроса баланса
    std::shared_ptr<exchange::bitget::BitgetRestClient> rest_client_;

    /// Authenticated private WebSocket client for event-driven fills
    std::unique_ptr<exchange::bitget::BitgetPrivateWsClient> private_ws_client_;

    /// Handle incoming private WS messages (order fills, position updates)
    void on_private_ws_message(const std::string& channel, const boost::json::value& data);

    /// Ссылка на BitgetFuturesOrderSubmitter (для USDT-M фьючерсов)
    std::shared_ptr<exchange::bitget::BitgetFuturesOrderSubmitter> futures_submitter_;

    /// Правила инструмента для текущего символа (из exchange info)
    ExchangeSymbolRules exchange_rules_;

    // ==================== Phase 1: Latency SLA ============================
    /// Трекер латентности по стадиям pipeline (P50/P95/P99)
    std::unique_ptr<PipelineLatencyTracker> latency_tracker_;
    /// Timestamp последнего экспорта latency-метрик
    int64_t last_latency_emit_ns_{0};

    // ==================== Phase 2: Order Watchdog =========================
    /// Непрерывный монитор жизненного цикла ордеров
    std::unique_ptr<OrderWatchdog> order_watchdog_;

    // ==================== Phase 2: Exit Orchestrator =====================
    /// Единый владелец всех exit-решений
    std::unique_ptr<PositionExitOrchestrator> exit_orchestrator_;

    /// Cached regime snapshot (updated every tick, used by build_exit_context)
    regime::RegimeSnapshot cached_regime_snapshot_{};
    /// Cached uncertainty snapshot (updated every tick, used by build_exit_context)
    uncertainty::UncertaintySnapshot cached_uncertainty_snapshot_{};
    /// Cached market state vector (Phase 4, updated every tick)
    MarketStateVector cached_market_state_{};
    /// Market reaction engine (Phase 4)
    std::unique_ptr<MarketReactionEngine> market_reaction_engine_;
    /// Timestamp последней проверки watchdog
    int64_t last_watchdog_ns_{0};
    /// Интервал проверки watchdog: 10 секунд
    static constexpr int64_t kWatchdogIntervalNs = 10'000'000'000LL;

    // ==================== Phase 4: Continuous Reconciliation ==============
    /// Движок reconciliation для непрерывной проверки состояния ордеров/позиций
    std::shared_ptr<reconciliation::ReconciliationEngine> reconciliation_engine_;
    /// Адаптер Bitget REST для фьючерсов
    std::shared_ptr<exchange::bitget::BitgetFuturesQueryAdapter> futures_query_adapter_;

    /// Timestamp последней reconciliation в runtime
    int64_t last_reconciliation_ns_{0};
    /// Timestamp последней позиционной/балансовой reconciliation
    int64_t last_pos_balance_reconciliation_ns_{0};
    /// Интервал runtime reconciliation: 60 секунд
    static constexpr int64_t kReconciliationIntervalNs = 60'000'000'000LL;
    /// Флаг: reconciliation обнаружила расхождение, нужна ресинхронизация с биржей
    bool reconciliation_needs_resync_{false};

    /// Timestamp последней периодической синхронизации баланса
    int64_t last_balance_sync_ns_{0};
    /// Интервал синхронизации баланса с биржей: 5 минут
    static constexpr int64_t kBalanceSyncIntervalNs = 300'000'000'000LL;

    /// Запросить баланс USDT с биржи и обновить капитал
    void sync_balance_from_exchange();

    /// Загрузить точность ордеров (quantity/price) для текущего символа с биржи
    void fetch_symbol_precision();

    /// Загрузить исторические свечи через REST API для прогрева индикаторов.
    /// Загружает 168 часовых свечей (7 дней) + 200 минутных свечей.
    /// Без достаточной истории индикаторы дают ложные сигналы.
    void bootstrap_historical_candles();

    /// Загрузить часовые свечи для HTF (High-TimeFrame) анализа.
    /// Используется для определения глобального тренда на старшем таймфрейме.
    void bootstrap_htf_candles();

    /// Проверить открытые позиции на стоп-лосс.
    bool check_position_stop_loss(const features::FeatureSnapshot& snapshot);

    /// Проверка готовности рынка к торговле.
    /// Блокирует торговлю пока HTF-анализ не подтвердит безопасные условия.
    /// @return true если рынок готов, false — ждём лучших условий
    bool check_market_readiness(const features::FeatureSnapshot& snapshot);

    /// Минимальное количество тиков перед началом торговли.
    /// 200 тиков ≈ 3-5 минут live данных — индикаторы стабилизируются.
    static constexpr uint64_t kMinWarmupTicks = 200;

    /// Флаг завершения прогрева индикаторов (исторические свечи загружены)
    bool indicators_warmed_up_{false};

    // ==================== HTF Trend Filter ====================
    // Данные старшего таймфрейма (1h) для определения глобального направления.
    // Блокирует BUY в сильном даунтренде и SELL в сильном аптренде.

    /// HTF EMA (20 часовых свечей)
    double htf_ema_20_{0.0};
    /// HTF EMA (50 часовых свечей)
    double htf_ema_50_{0.0};
    /// HTF RSI (14 часовых свечей)
    double htf_rsi_14_{50.0};
    /// HTF ADX (14 часовых свечей)
    double htf_adx_{0.0};
    /// HTF MACD histogram (часовой)
    double htf_macd_histogram_{0.0};
    /// Последняя часовая цена закрытия
    double htf_last_close_{0.0};
    /// HTF данные валидны (достаточно истории)
    bool htf_valid_{false};

    /// Направление HTF тренда: +1=аптренд, -1=даунтренд, 0=боковик
    int htf_trend_direction_{0};
    /// Сила HTF тренда (0.0-1.0): используется для блокировки контр-трендовых сделок
    double htf_trend_strength_{0.0};

    /// Вычислить HTF-тренд из загруженных часовых свечей
    void compute_htf_trend(const std::vector<double>& closes);

    // ==================== HTF Real-Time Update ====================
    /// Буфер часовых свечей (closes) для инкрементального обновления HTF
    std::vector<double> htf_closes_buffer_;
    /// Буфер highs для ATR/ADX пересчёта
    std::vector<double> htf_highs_buffer_;
    /// Буфер lows
    std::vector<double> htf_lows_buffer_;
    /// Максимальный размер буфера (200 часовых свечей)
    static constexpr size_t kHtfBufferMaxSize = 200;
    /// Timestamp последнего HTF обновления
    int64_t last_htf_update_ns_{0};
    /// Интервал принудительного обновления HTF через REST (каждые 60 минут)
    static constexpr int64_t kHtfUpdateIntervalNs = 3600'000'000'000LL;
    /// Интервал экстренного обновления (если цена сильно сдвинулась — 3×ATR)
    bool htf_urgent_update_needed_{false};

    /// Обновить HTF данные через REST если прошёл час или urgent
    void maybe_update_htf(const features::FeatureSnapshot& snapshot);

    /// Флаг готовности рынка — бот не торгует, пока не найдёт хорошую точку входа
    bool market_ready_{false};
    /// Счётчик тиков с момента определения готовности (антиспам)
    uint64_t market_ready_since_tick_{0};

    // ==================== Адаптивный стоп-лосс ====================
    // Chandelier Exit: trailing stop подтягивается за ценой, никогда не откатывается

    /// Максимальная цена с момента входа в позицию (для trailing stop BUY)
    double highest_price_since_entry_{0.0};
    /// Минимальная цена с момента входа (для trailing stop SELL)
    double lowest_price_since_entry_{std::numeric_limits<double>::max()};
    /// Текущий уровень стоп-лосса (динамически обновляется)
    double current_stop_level_{0.0};
    /// Стоп был перенесён в breakeven (вход + комиссия)
    bool breakeven_activated_{false};
    /// Первый partial take-profit выполнен
    bool partial_tp_taken_{false};
    /// Закрытие позиции ожидает исполнения — блокирует новые входы
    bool close_order_pending_{false};
    /// Начальный размер позиции (для расчёта partial close)
    double initial_position_size_{0.0};
    /// Текущий ATR-множитель для trailing stop (адаптивный)
    double current_trail_mult_{2.0};
    /// Время входа в позицию (nanoseconds) — used for telemetry and hedge duration tracking
    int64_t position_entry_time_ns_{0};

    /// Обновить trailing stop для текущей позиции
    void update_trailing_stop(const features::FeatureSnapshot& snapshot);
    /// Сбросить все поля trailing stop при закрытии/открытии позиции
    void reset_trailing_state();

    /// Собрать ExitContext для exit orchestrator из текущего состояния pipeline
    ExitContext build_exit_context(const features::FeatureSnapshot& snapshot,
                                  const portfolio::Position& pos) const;

    // ==================== Alpha Decay Feedback ====================
    // Мониторинг деградации альфа-сигнала и автоматическая корректировка

    /// Стратегия, которая открыла текущую позицию (для record_trade_outcome)
    StrategyId current_position_strategy_{""};
    /// Сторона текущей позиции (Long/Short) — для корректного закрытия на фьючерсах
    PositionSide current_position_side_{PositionSide::Long};
    /// Conviction при открытии текущей позиции
    double current_position_conviction_{0.0};
    /// Thompson action при открытии текущей позиции (для корректной записи reward)
    ml::EntryAction current_entry_thompson_action_{ml::EntryAction::EnterNow};

    /// A4 fix: мировое состояние при открытии позиции (для feedback в world model)
    world_model::WorldState current_entry_world_state_{world_model::WorldState::Unknown};

    /// Проскальзывание при входе в текущую позицию (бп)
    double current_position_slippage_bps_{0.0};

    /// Max Adverse Excursion текущей позиции (бп, нарастающий итог)
    double current_max_adverse_excursion_bps_{0.0};

    /// Последний результат execution alpha — нужен для C/C fee estimation при закрытии позиции
    std::optional<execution_alpha::ExecutionAlphaResult> last_exec_alpha_;

    // ==================== Daily Reset ====================
    /// Последний UTC-день, в который произошёл daily reset (YYYYMMDD)
    int last_daily_reset_day_{0};

    // ==================== Futures Management ====================
    /// Текущий funding rate для активного символа (обновляется периодически)
    double current_funding_rate_{0.0};
    /// Timestamp последнего обновления funding rate
    int64_t last_funding_rate_update_ns_{0};
    /// Interval for funding rate updates: 5 minutes (300 seconds)
    static constexpr int64_t kFundingRateUpdateIntervalNs = 300'000'000'000LL;

    // ==================== Rolling Trade Statistics ====================
    // Compute live win_rate and win/loss ratio from recent trades
    // for Kelly fraction and portfolio allocator adaptation.
    static constexpr size_t kTradeStatsWindowSize = 100;

    struct TradeOutcome {
        double pnl_pct{0.0};   ///< Percentage return of the trade
        bool   won{false};     ///< Whether the trade was profitable
    };

    std::deque<TradeOutcome> trade_history_;  ///< Rolling window of recent trades

    /// Record a closed trade for rolling statistics
    void record_trade_for_stats(double pnl_pct);

    /// Compute current rolling win rate from trade_history_
    [[nodiscard]] double rolling_win_rate() const;

    /// Compute current rolling win/loss ratio from trade_history_
    [[nodiscard]] double rolling_win_loss_ratio() const;

    /// Обновить MAE для текущей позиции на текущем тике
    void update_current_mae(double current_price, bool is_long);

    /// Запустить периодические фоновые задачи (Phase 1/2/4)
    void run_periodic_tasks(int64_t now_ns);

    /// Запустить проверку watchdog ордеров (Phase 2)
    void run_order_watchdog(int64_t now_ns);

    /// Запустить непрерывную reconciliation (Phase 4)
    void run_continuous_reconciliation(int64_t now_ns);

    // ==================== Correlation Monitor Reference Feeds ==============
    /// Периодически запрашивать цены BTC/ETH для CorrelationMonitor
    void update_reference_prices();
    /// Timestamp последнего обновления reference prices
    int64_t last_reference_price_update_ns_{0};
    /// Флаг фонового запроса reference prices (не блокирует hot path)
    std::shared_ptr<std::atomic<bool>> reference_price_fetch_in_flight_{
        std::make_shared<std::atomic<bool>>(false)};
    /// Background thread for reference price fetching (joinable for clean shutdown)
    std::jthread ref_price_thread_;
    /// Интервал обновления reference prices: 30 секунд
    static constexpr int64_t kReferencePriceIntervalNs = 30'000'000'000LL;

    // ==================== Persistence ====================
    /// Персистентный слой для записи snapshot'ов и journal-событий
    std::shared_ptr<persistence::PersistenceLayer> persistence_;
    /// Timestamp последнего snapshot'а портфеля
    int64_t last_snapshot_ns_{0};
    /// Интервал записи snapshot'ов: 30 секунд
    static constexpr int64_t kSnapshotIntervalNs = 30'000'000'000LL;
    /// Записать snapshot портфеля в persistence (включая opened_at_ns)
    void persist_portfolio_snapshot();
    /// Записать событие открытия/закрытия позиции в journal
    void persist_position_event(const std::string& event_type, const Symbol& symbol,
                                PositionSide ps, double price, double pnl,
                                double size, const std::string& strategy_id);

    // ==================== Phase 8: Observability & Telemetry ==============
    /// Decision chain telemetry (captures full TelemetryEnvelope per trade decision)
    std::shared_ptr<telemetry::ResearchTelemetry> telemetry_;
    /// Incident detector (6 playbook types)
    std::unique_ptr<telemetry::IncidentDetector> incident_detector_;
    /// Observability panel metrics (7 panels)
    telemetry::ObservabilityPanels obs_panels_;
    /// Monotonic sequence ID for telemetry envelopes
    std::atomic<uint64_t> telemetry_seq_{0};
    /// Timestamp of last incident check
    int64_t last_incident_check_ns_{0};
    /// Interval: check incidents every 10 seconds
    static constexpr int64_t kIncidentCheckIntervalNs = 10'000'000'000LL;

    // ==================== Hedge Recovery ====================
    // Парная хедж-позиция управляется state machine (HedgePairManager).
    // Решения основаны на market state, не на timeout.

    /// State machine для парной хедж-позиции
    std::unique_ptr<HedgePairManager> hedge_manager_;

    /// DualLegManager — coordinated long+short pair entry, TPSL, reversal
    std::unique_ptr<DualLegManager> dual_leg_manager_;
    /// Хедж активен (есть locked position: long + short одновременно)
    bool hedge_active_{false};
    /// Сторона хедж-позиции (противоположная основной)
    PositionSide hedge_position_side_{PositionSide::Short};
    /// Цена входа хедж-позиции
    double hedge_entry_price_{0.0};
    /// Размер хедж-позиции
    double hedge_size_{0.0};
    /// Время открытия хеджа (ns)
    int64_t hedge_entry_time_ns_{0};
    /// Убыток основной позиции в момент открытия хеджа (USDT)
    double original_loss_at_hedge_{0.0};

    /// Проверить и управлять хедж-позицией (delegated to HedgePairManager).
    /// @return true если хедж активен (блокировать обычный stop-loss)
    bool check_hedge_recovery(const features::FeatureSnapshot& snapshot);

    /// Оценить рыночную ситуацию для принятия решения о выходе.
    double evaluate_exit_score(const features::FeatureSnapshot& snapshot, PositionSide for_side) const;

    /// Закрыть одну ногу хедж-позиции
    bool close_hedge_leg(const features::FeatureSnapshot& snapshot,
                         PositionSide leg_side, double qty,
                         const std::string& reason);

    /// Сбросить состояние хеджа
    void reset_hedge_state();
};


} // namespace tb::pipeline
