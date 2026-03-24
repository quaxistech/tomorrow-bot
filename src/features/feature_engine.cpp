#include "feature_engine.hpp"
#include <cmath>
#include <algorithm>
#include <numeric>

namespace tb::features {

namespace {

// Стандартное отклонение последних n значений вектора
double std_dev(const std::vector<double>& data, std::size_t n) {
    if (data.size() < n || n == 0) return 0.0;
    const std::size_t start = data.size() - n;
    double mean = 0.0;
    for (std::size_t i = start; i < data.size(); ++i) {
        mean += data[i];
    }
    mean /= static_cast<double>(n);
    double variance = 0.0;
    for (std::size_t i = start; i < data.size(); ++i) {
        const double diff = data[i] - mean;
        variance += diff * diff;
    }
    variance /= static_cast<double>(n);
    return std::sqrt(variance);
}

} // namespace

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
    const std::string key = candle.envelope.symbol.get();
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
    auto it = candle_buffers_.find(symbol.get());
    if (it == candle_buffers_.end()) return false;
    // Нужно достаточно свечей для самого медленного индикатора
    const std::size_t required = static_cast<std::size_t>(
        std::max({config_.sma_period, config_.ema_slow_period, config_.macd_slow}) + 1
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
        auto it = candle_buffers_.find(symbol.get());
        const std::size_t required = static_cast<std::size_t>(
            std::max({config_.sma_period, config_.ema_slow_period, config_.macd_slow}) + 1
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

    auto buf_it = candle_buffers_.find(symbol.get());
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

        auto ema_fast = indicators_->ema(close, config_.ema_fast_period);
        auto ema_slow = indicators_->ema(close, config_.ema_slow_period);
        tf.ema_valid = ema_fast.valid && ema_slow.valid;
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
        // Нормализация OBV: делим на среднее абсолютных значений за последние 20 периодов
        const std::size_t norm_window = 20;
        if (volume.size() >= norm_window) {
            double sum_vol = 0.0;
            const std::size_t start = volume.size() - norm_window;
            for (std::size_t i = start; i < volume.size(); ++i) {
                sum_vol += std::abs(volume[i]);
            }
            const double mean_vol = sum_vol / static_cast<double>(norm_window);
            tf.obv_normalized = (mean_vol > 0.0) ? tf.obv / mean_vol : 0.0;
        }
    }

    // Волатильность: стандартное отклонение доходностей
    if (close.size() >= 5) {
        tf.volatility_5 = std_dev(close, std::min(close.size(), std::size_t{5}));
        tf.volatility_valid = true;
    }
    if (close.size() >= 20) {
        tf.volatility_20 = std_dev(close, std::min(close.size(), std::size_t{20}));
        tf.volatility_valid = true;
    }

    // Моментум: (close[-1] - close[-n]) / close[-n]
    const double last_close = close.back();
    if (close.size() >= 6) {
        const double prev_5 = close[close.size() - 6];
        tf.momentum_5 = (prev_5 > 0.0) ? (last_close - prev_5) / prev_5 : 0.0;
        tf.momentum_valid = true;
    }
    if (close.size() >= 21) {
        const double prev_20 = close[close.size() - 21];
        tf.momentum_20 = (prev_20 > 0.0) ? (last_close - prev_20) / prev_20 : 0.0;
        tf.momentum_valid = true;
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

    // Нестабильность стакана — оценка по дисбалансу глубины и широте спреда
    if (mf.book_imbalance_valid && mf.spread_valid) {
        double imbalance_component = std::abs(mf.book_imbalance_5) * 0.6;
        double spread_component = std::min(mf.spread_bps / 100.0, 1.0) * 0.4;
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

    // Простая оценка проскальзывания: половина спреда в bps
    ec.estimated_slippage_bps = ticker.spread_bps * 0.5;
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
