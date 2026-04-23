#include "feature_engine.hpp"
#include <cmath>
#include <algorithm>
#include <numeric>

namespace tb::features {

FeatureEngine::FeatureEngine(Config config,
                             std::shared_ptr<indicators::IndicatorEngine> indicators,
                             std::shared_ptr<tb::clock::IClock> clock,
                             std::shared_ptr<tb::logging::ILogger> logger,
                             std::shared_ptr<tb::metrics::IMetricsRegistry> metrics)
    : config_(std::move(config))
    , indicators_(std::move(indicators))
    , clock_(std::move(clock))
    , logger_(std::move(logger))
    , metrics_(std::move(metrics))
{}

void FeatureEngine::on_candle(const normalizer::NormalizedCandle& candle) {
    std::lock_guard lock(mutex_);
    // Key by symbol:interval to prevent mixing timeframes (1m/5m/1h)
    const std::string key = candle.envelope.symbol.get() + ":" + candle.interval;
    auto& buf = candle_buffers_[key];
    if (candle.is_closed) {
        buf.push(candle);
    } else {
        buf.update_last(candle);
    }
}

void FeatureEngine::on_trade(const normalizer::NormalizedTrade& trade) {
    std::lock_guard lock(mutex_);
    const std::string key = trade.envelope.symbol.get();
    trade_buffers_[key].push(trade);
}

void FeatureEngine::on_ticker(const normalizer::NormalizedTicker& ticker) {
    std::lock_guard lock(mutex_);
    last_tickers_[ticker.envelope.symbol.get()] = ticker;
}

bool FeatureEngine::is_ready(const tb::Symbol& symbol) const {
    std::lock_guard lock(mutex_);
    const std::string key = symbol.get() + ":" + config_.primary_interval;
    auto it = candle_buffers_.find(key);
    if (it == candle_buffers_.end()) return false;
    // Нужно достаточно свечей для ВСЕХ индикаторов + warm-up
    const std::size_t required = static_cast<std::size_t>(
        std::max({config_.sma_period, config_.ema_slow_period, config_.macd_slow,
                  config_.bb_period, config_.atr_period, config_.adx_period}) + 1
    );
    return it->second.has_enough(required);
}

std::optional<FeatureSnapshot> FeatureEngine::compute_snapshot(
    const tb::Symbol& symbol,
    const order_book::LocalOrderBook& book) const
{
    std::lock_guard lock(mutex_);

    // Проверяем готовность без повторной блокировки
    {
        const std::string candle_key = symbol.get() + ":" + config_.primary_interval;
        auto it = candle_buffers_.find(candle_key);
        const std::size_t required = static_cast<std::size_t>(
            std::max({config_.sma_period, config_.ema_slow_period, config_.macd_slow,
                      config_.bb_period, config_.atr_period, config_.adx_period}) + 1
        );
        if (it == candle_buffers_.end() || !it->second.has_enough(required)) {
            return std::nullopt;
        }
    }

    auto ticker_it = last_tickers_.find(symbol.get());
    if (ticker_it == last_tickers_.end()) {
        return std::nullopt;
    }
    const auto& ticker = ticker_it->second;

    FeatureSnapshot snap;
    snap.symbol = symbol;
    snap.computed_at = clock_->now();
    snap.last_price = ticker.last_price;
    snap.mid_price = tb::Price{(ticker.bid.get() + ticker.ask.get()) * 0.5};
    snap.book_quality = book.quality();

    // Возраст данных: разница между текущим временем и временем последнего тикера
    snap.market_data_age_ns = tb::Timestamp{
        snap.computed_at.get() - ticker.envelope.received_ts.get()
    };

    snap.technical = compute_technical(symbol);
    snap.microstructure = compute_microstructure(ticker, book);
    snap.execution_context = compute_execution_context(ticker, book);

    return snap;
}

TechnicalFeatures FeatureEngine::compute_technical(const tb::Symbol& symbol) const {
    TechnicalFeatures tf;

    const std::string key = symbol.get() + ":" + config_.primary_interval;
    auto buf_it = candle_buffers_.find(key);
    if (buf_it == candle_buffers_.end()) return tf;
    const auto& buf = buf_it->second;

    const auto close  = buf.close_prices();
    const auto high   = buf.high_prices();
    const auto low    = buf.low_prices();
    const auto volume = buf.volumes();

    if (close.empty()) return tf;

    // SMA / EMA
    {
        auto sma = indicators_->sma(close, config_.sma_period);
        tf.sma_valid = sma.valid;
        tf.sma_20 = sma.value;

        auto ema_ultra_fast = indicators_->ema(close, config_.ema_ultra_fast_period);
        auto ema_fast = indicators_->ema(close, config_.ema_fast_period);
        auto ema_slow = indicators_->ema(close, config_.ema_slow_period);
        tf.ema_valid = ema_fast.valid && ema_slow.valid;
        tf.ema_8 = ema_ultra_fast.valid ? ema_ultra_fast.value : 0.0;
        tf.ema_20 = ema_fast.value;
        tf.ema_50 = ema_slow.value;
    }

    // RSI
    {
        auto rsi = indicators_->rsi(close, config_.rsi_period);
        tf.rsi_valid = rsi.valid;
        tf.rsi_14 = rsi.value;
    }

    // MACD
    {
        auto macd = indicators_->macd(close, config_.macd_fast, config_.macd_slow, config_.macd_signal);
        tf.macd_valid = macd.valid;
        tf.macd_line = macd.macd;
        tf.macd_signal = macd.signal;
        tf.macd_histogram = macd.histogram;
    }

    // Bollinger Bands
    {
        auto bb = indicators_->bollinger(close, config_.bb_period, config_.bb_stddev);
        tf.bb_valid = bb.valid;
        tf.bb_upper = bb.upper;
        tf.bb_middle = bb.middle;
        tf.bb_lower = bb.lower;
        tf.bb_bandwidth = bb.bandwidth;
        tf.bb_percent_b = bb.percent_b;
    }

    // ATR
    {
        auto atr = indicators_->atr(high, low, close, config_.atr_period);
        tf.atr_valid = atr.valid;
        tf.atr_14 = atr.value;
        // Нормализация ATR относительно последней цены закрытия
        const double last_close = close.back();
        tf.atr_14_normalized = (atr.valid && last_close > 0.0)
            ? atr.value / last_close
            : 0.0;
    }

    // ADX
    {
        auto adx = indicators_->adx(high, low, close, config_.adx_period);
        tf.adx_valid = adx.valid;
        tf.adx = adx.adx;
        tf.plus_di = adx.plus_di;
        tf.minus_di = adx.minus_di;
    }

    // OBV
    {
        auto obv = indicators_->obv(close, volume);
        tf.obv_valid = obv.valid;
        tf.obv = obv.value;
        // ИСПРАВЛЕНИЕ H12: нормализация OBV через slope, а не деление кумулятивного значения.
        // Проблема старой нормализации: cumulative_obv / mean_vol зависит от длины истории.
        // Решение: OBV slope (дельта OBV за окно) / (mean_vol × window).
        // Это даёт стационарный сигнал ∈ [-1, +1], измеряющий текущее согласие
        // объёма с направлением цены — независимо от времени работы бота.
        constexpr std::size_t norm_window = 20;
        if (close.size() >= norm_window + 1 && volume.size() >= norm_window + 1) {
            // Вычисляем "локальный OBV" за последние norm_window баров
            double obv_recent = 0.0;
            const auto start_idx = close.size() - norm_window;
            for (std::size_t i = start_idx; i < close.size(); ++i) {
                if (close[i] > close[i - 1])      obv_recent += volume[i];
                else if (close[i] < close[i - 1]) obv_recent -= volume[i];
            }
            // Нормализуем по средней абс. объём × window
            const auto vol_begin = volume.end() - static_cast<std::ptrdiff_t>(norm_window);
            double sum_vol = 0.0;
            for (auto it = vol_begin; it != volume.end(); ++it) {
                sum_vol += std::abs(*it);
            }
            const double mean_vol = sum_vol / static_cast<double>(norm_window);
            const double denominator = mean_vol * static_cast<double>(norm_window);
            tf.directional_volume_proxy = (denominator > 0.0)
                ? std::clamp(obv_recent / denominator, -1.0, 1.0)
                : 0.0;
        }
    }

    // Волатильность: std-dev логарифмических доходностей (IndicatorEngine)
    {
        auto vol5 = indicators_->volatility(close, 5);
        auto vol20 = indicators_->volatility(close, 20);
        tf.volatility_5 = vol5.valid ? vol5.value : 0.0;
        tf.volatility_20 = vol20.valid ? vol20.value : 0.0;
        tf.volatility_valid = vol5.valid && vol20.valid;
    }

    // Моментум: (close_now − close_n_ago) / close_n_ago (IndicatorEngine)
    {
        auto mom5 = indicators_->momentum(close, 5);
        auto mom20 = indicators_->momentum(close, 20);
        tf.momentum_5 = mom5.valid ? mom5.value : 0.0;
        tf.momentum_20 = mom20.valid ? mom20.value : 0.0;
        tf.momentum_valid = mom5.valid && mom20.valid;
    }

    return tf;
}

MicrostructureFeatures FeatureEngine::compute_microstructure(
    const normalizer::NormalizedTicker& ticker,
    const order_book::LocalOrderBook& book) const
{
    MicrostructureFeatures mf;

    // Спред из нормализованного тикера
    mf.spread = ticker.spread;
    mf.spread_bps = ticker.spread_bps;
    mf.spread_valid = (ticker.ask.get() > 0.0 && ticker.bid.get() > 0.0);

    // Средняя цена
    mf.mid_price = (ticker.bid.get() + ticker.ask.get()) * 0.5;

    // Данные из стакана
    if (auto tob = book.top_of_book()) {
        mf.weighted_mid_price = tob->mid_price;
    } else {
        mf.weighted_mid_price = mf.mid_price;
    }

    // Дисбаланс стакана из DepthSummary
    if (auto depth = book.depth_summary(config_.book_depth_levels)) {
        mf.book_imbalance_5 = depth->imbalance_5;
        mf.book_imbalance_10 = depth->imbalance_10;
        mf.book_imbalance_valid = true;
        mf.weighted_mid_price = depth->weighted_mid;

        // Ликвидность: нотиональный объём на 5 уровнях
        mf.bid_depth_5_notional = depth->bid_depth_5.get() * mf.mid_price;
        mf.ask_depth_5_notional = depth->ask_depth_5.get() * mf.mid_price;
        mf.liquidity_valid = true;
        const double total = mf.bid_depth_5_notional + mf.ask_depth_5_notional;
        mf.liquidity_ratio = (total > 0.0)
            ? std::min(mf.bid_depth_5_notional, mf.ask_depth_5_notional) / (total * 0.5)
            : 1.0;
    }

    // Поток сделок из буфера
    const std::string key = ticker.envelope.symbol.get();
    auto trade_it = trade_buffers_.find(key);
    if (trade_it != trade_buffers_.end() && !trade_it->second.empty()) {
        const auto& tbuf = trade_it->second;
        const std::size_t window = static_cast<std::size_t>(config_.trade_flow_window);
        mf.buy_sell_ratio = tbuf.buy_sell_ratio(window);
        mf.aggressive_flow = tbuf.aggressive_flow(window);
        mf.trade_vwap = tbuf.vwap(window);
        mf.trade_flow_valid = true;
    }

    // Нестабильность стакана — composite метрика из дисбаланса и спреда.
    // Cont, Kukanov & Stoikov (2014), "The Price Impact of Order Book Events":
    //   order imbalance объясняет ~65–70% краткосрочных ценовых движений.
    // Weights: 0.7 — imbalance (основной предиктор), 0.3 — spread (вторичный).
    if (mf.book_imbalance_valid && mf.spread_valid) {
        const double imbalance_component = std::abs(mf.book_imbalance_5) * 0.7;
        const double spread_component = std::min(mf.spread_bps / 100.0, 1.0) * 0.3;
        mf.book_instability = std::clamp(imbalance_component + spread_component, 0.0, 1.0);
        mf.instability_valid = true;
    } else {
        mf.book_instability = 0.0;
        mf.instability_valid = false;
    }

    return mf;
}

ExecutionContextFeatures FeatureEngine::compute_execution_context(
    const normalizer::NormalizedTicker& ticker,
    const order_book::LocalOrderBook& book) const
{
    ExecutionContextFeatures ec;

    ec.spread_cost_bps = ticker.spread_bps;

    // Немедленная ликвидность: минимальный размер лучшей котировки
    if (auto tob = book.top_of_book()) {
        ec.immediate_liquidity = std::min(tob->bid_size.get(), tob->ask_size.get());
    }

    // M-11 fix: depth-aware slippage model
    // Base: half-spread + depth impact estimator
    double base_slippage = ticker.spread_bps * 0.5;
    double depth_impact = 0.0;
    if (ec.immediate_liquidity > 0.0) {
        // Thin book → higher slippage. Uses inverse-depth scaling.
        // At 1000 USD depth: ~0 extra impact. At 100 USD: +2-3 bps.
        double mid_px = (ticker.bid.get() + ticker.ask.get()) * 0.5;
        if (mid_px > 0.0) {
            double depth_usd = ec.immediate_liquidity * mid_px;
            if (depth_usd < 1000.0 && depth_usd > 0.0) {
                depth_impact = std::min(5.0, (1000.0 / depth_usd - 1.0) * 1.5);
            }
        }
    }
    ec.estimated_slippage_bps = base_slippage + depth_impact;
    ec.slippage_valid = (ticker.spread_bps > 0.0);

    // Криптовалютные рынки работают 24/7 — всегда открыты
    ec.is_market_open = true;

    // Свежесть данных: время последнего тикера vs текущее время
    const int64_t now_ns = clock_->now().get();
    const int64_t received_ns = ticker.envelope.received_ts.get();
    ec.is_feed_fresh = (now_ns - received_ns) < config_.feed_freshness_ns;

    return ec;
}

} // namespace tb::features
