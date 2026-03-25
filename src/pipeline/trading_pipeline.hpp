#pragma once

/// @file trading_pipeline.hpp
/// @brief Торговый pipeline — связывает все подсистемы в единый цикл обработки
///
/// Поток данных:
/// MarketDataGateway → FeatureSnapshot → WorldModel → Regime → Uncertainty
/// → Strategies → StrategyAllocator → Decision → PortfolioAllocator → Risk → Execution

#include "market_data/market_data_gateway.hpp"
#include "features/feature_engine.hpp"
#include "features/advanced_features.hpp"
#include "order_book/order_book.hpp"
#include "indicators/indicator_engine.hpp"
#include "world_model/world_model_engine.hpp"
#include "regime/regime_engine.hpp"
#include "uncertainty/uncertainty_engine.hpp"
#include "strategy/strategy_registry.hpp"
#include "strategy/momentum/momentum_strategy.hpp"
#include "strategy/mean_reversion/mean_reversion_strategy.hpp"
#include "strategy/breakout/breakout_strategy.hpp"
#include "strategy/vol_expansion/vol_expansion_strategy.hpp"
#include "strategy/microstructure_scalp/microstructure_scalp_strategy.hpp"
#include "strategy_allocator/strategy_allocator.hpp"
#include "decision/decision_aggregation_engine.hpp"
#include "portfolio_allocator/portfolio_allocator.hpp"
#include "portfolio/portfolio_engine.hpp"
#include "execution_alpha/execution_alpha_engine.hpp"
#include "risk/risk_engine.hpp"
#include "execution/execution_engine.hpp"
#include "execution/twap_executor.hpp"
#include "config/config_types.hpp"
#include "security/secret_provider.hpp"
#include "logging/logger.hpp"
#include "clock/clock.hpp"
#include "metrics/metrics_registry.hpp"
#include "health/health_service.hpp"
#include "exchange/bitget/bitget_rest_client.hpp"
#include "exchange/bitget/bitget_order_submitter.hpp"
#include "alpha_decay/alpha_decay_monitor.hpp"
#include "ml/bayesian_adapter.hpp"
#include "ml/entropy_filter.hpp"
#include "ml/microstructure_fingerprint.hpp"
#include "ml/liquidation_cascade.hpp"
#include "ml/correlation_monitor.hpp"
#include "ml/thompson_sampler.hpp"
#include <memory>
#include <atomic>
#include <mutex>
#include <optional>

namespace tb::pipeline {

class TradingPipeline {
public:
    /// @param symbol Торговый символ (например "BTCUSDT").
    ///        Если пустой — используется "BTCUSDT" по умолчанию.
    TradingPipeline(
        const config::AppConfig& config,
        std::shared_ptr<security::ISecretProvider> secret_provider,
        std::shared_ptr<logging::ILogger> logger,
        std::shared_ptr<clock::IClock> clock,
        std::shared_ptr<metrics::IMetricsRegistry> metrics,
        std::shared_ptr<health::IHealthService> health,
        const std::string& symbol = ""
    );

    ~TradingPipeline();

    /// Запустить все подсистемы (вызывается supervisor)
    bool start();

    /// Получить символ, которым торгует этот pipeline
    const Symbol& symbol() const { return symbol_; }

    /// Установить точность ордеров для символа (из PairScanner exchange info)
    void set_symbol_precision(int quantity_precision, int price_precision);

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
    std::shared_ptr<health::IHealthService> health_;

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
    std::shared_ptr<risk::IRiskEngine> risk_engine_;
    std::shared_ptr<execution::ExecutionEngine> execution_engine_;

    /// Smart TWAP — разбиение крупных ордеров на адаптивные слайсы
    std::shared_ptr<execution::SmartTwapExecutor> twap_executor_;

    /// Мониторинг деградации альфа-сигнала стратегий
    std::shared_ptr<alpha_decay::AlphaDecayMonitor> alpha_decay_monitor_;

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

    /// Ожидающий вход (Thompson Sampling решил ждать)
    struct PendingEntry {
        strategy::TradeIntent intent;          ///< Исходный торговый сигнал
        int wait_periods_remaining{0};         ///< Оставшиеся периоды ожидания
        ml::EntryAction action{ml::EntryAction::EnterNow}; ///< Выбранное действие
    };
    std::optional<PendingEntry> pending_entry_;

    std::atomic<bool> running_{false};
    std::mutex pipeline_mutex_;
    uint64_t tick_count_{0};

    /// Время последней торговой активности (ордер, non-veto сигнал)
    std::atomic<int64_t> last_activity_ns_{0};

    /// Время последнего отправленного ордера (для cooldown между сделками)
    int64_t last_order_time_ns_{0};
    /// Минимальный интервал между ордерами: 30 секунд
    static constexpr int64_t kOrderCooldownNs = 10'000'000'000LL;

    /// Счётчик последовательных отклонений ордеров (для экспоненциального backoff)
    int consecutive_rejections_{0};
    /// Максимальный backoff: 10 минут
    static constexpr int64_t kMaxRejectionBackoffNs = 120'000'000'000LL;

    /// Время последнего стоп-лосс ордера (отдельный cooldown от обычных сделок).
    /// Стоп-лосс — экстренный механизм, его нельзя блокировать обычным cooldown'ом,
    /// иначе позиция остаётся открытой и убыток растёт.
    int64_t last_stop_loss_time_ns_{0};
    /// Cooldown между попытками стоп-лосса: 5 секунд.
    /// Достаточно для предотвращения спама на биржу, но не блокирует экстренное закрытие.
    static constexpr int64_t kStopLossCooldownNs = 5'000'000'000LL;

    /// REST клиент для запроса баланса (только production/testnet)
    std::shared_ptr<exchange::bitget::BitgetRestClient> rest_client_;

    /// Ссылка на BitgetOrderSubmitter для настройки precision (nullptr в paper mode)
    std::shared_ptr<exchange::bitget::BitgetOrderSubmitter> bitget_submitter_;

    /// Запросить баланс USDT с биржи и обновить капитал
    void sync_balance_from_exchange();

    /// Загрузить точность ордеров (quantity/price) для текущего символа с биржи
    void fetch_symbol_precision();

    /// Запросить актуальный баланс конкретного ассета (BTC/USDT) перед ордером
    double query_asset_balance(const std::string& coin);

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

    /// Максимальный убыток на одну сделку (% от капитала). По умолчанию 1%.
    static constexpr double kMaxLossPerTradePct = 1.0;

    /// ATR-множитель для динамического стоп-лосса (2.0 = 2×ATR)
    static constexpr double kAtrStopMultiplier = 2.0;

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
    double lowest_price_since_entry_{0.0};
    /// Текущий уровень стоп-лосса (динамически обновляется)
    double current_stop_level_{0.0};
    /// Стоп был перенесён в breakeven (вход + комиссия)
    bool breakeven_activated_{false};
    /// Первый partial take-profit выполнен
    bool partial_tp_taken_{false};
    /// Начальный размер позиции (для расчёта partial close)
    double initial_position_size_{0.0};
    /// Текущий ATR-множитель для trailing stop (адаптивный)
    double current_trail_mult_{2.0};
    /// Время входа в позицию (nanoseconds) — для time-based exit
    int64_t position_entry_time_ns_{0};
    /// Максимальное время удержания убыточной позиции: 15 минут
    static constexpr int64_t kMaxHoldLossNs = 15LL * 60 * 1'000'000'000LL;
    /// Максимальное время удержания любой позиции: 60 минут
    static constexpr int64_t kMaxHoldAbsoluteNs = 60LL * 60 * 1'000'000'000LL;

    /// Обновить trailing stop для текущей позиции
    void update_trailing_stop(const features::FeatureSnapshot& snapshot);
    /// Сбросить все поля trailing stop при закрытии/открытии позиции
    void reset_trailing_state();

    // ==================== Alpha Decay Feedback ====================
    // Мониторинг деградации альфа-сигнала и автоматическая корректировка

    /// Стратегия, которая открыла текущую позицию (для record_trade_outcome)
    StrategyId current_position_strategy_{""};
    /// Conviction при открытии текущей позиции
    double current_position_conviction_{0.0};
    /// Thompson action при открытии текущей позиции (для корректной записи reward)
    ml::EntryAction current_entry_thompson_action_{ml::EntryAction::EnterNow};
    /// Текущий множитель размера из alpha decay (1.0 = нет корректировки)
    double alpha_decay_size_mult_{1.0};
    /// Текущая добавка к порогу conviction из alpha decay
    double alpha_decay_threshold_adj_{0.0};
    /// Timestamp последней проверки alpha decay (не проверять каждый тик)
    int64_t last_alpha_decay_check_ns_{0};
    /// Интервал проверки alpha decay (каждые 60 секунд)
    static constexpr int64_t kAlphaDecayCheckIntervalNs = 60'000'000'000LL;

    /// Минимальный порог conviction для открытия новой позиции
    static constexpr double kDefaultConvictionThreshold = 0.3;

    /// Проверить alpha decay для всех стратегий и обновить множители
    void check_alpha_decay_feedback();
    /// Записать результат закрытой сделки в alpha decay monitor
    void record_trade_for_decay(const StrategyId& strategy_id, double pnl, double conviction);
};

} // namespace tb::pipeline
