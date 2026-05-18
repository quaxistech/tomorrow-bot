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
    last_trade_received_ns_[key] = trade.envelope.received_ts.get();

    // run94: update Anchored VWAP and CVD per trade.
    double price = trade.price.get();
    double size = trade.size.get();
    int64_t ts_ns = trade.envelope.exchange_ts.get();
    if (price > 0.0 && size > 0.0) {
        // Volume в quote currency (USDT) = price × size.
        double volume_quote = price * size;
        // Anchored VWAP — initialise on first trade per symbol.
        auto av_it = avwap_trackers_.find(key);
        if (av_it == avwap_trackers_.end()) {
            av_it = avwap_trackers_.emplace(key, indicators::AnchoredVwap()).first;
        }
        av_it->second.on_trade(price, volume_quote, ts_ns);

        // CVD — taker_buy = (side == Buy && !is_aggressive). Bitget: is_aggressive
        // означает sell-side taker (продавец бьёт). Тогда taker_buy = !is_aggressive
        // когда side == Buy. Конкретная семантика: side = taker side direction.
        // Buy taker → buy aggression. Используем side как primary.
        bool taker_buy = (trade.side == tb::Side::Buy);
        auto cvd_it = cvd_trackers_.find(key);
        if (cvd_it == cvd_trackers_.end()) {
            cvd_it = cvd_trackers_.emplace(key, indicators::CvdTracker(120)).first;
        }
        cvd_it->second.on_trade(price, volume_quote, taker_buy, ts_ns);
    }
}

void FeatureEngine::update_open_interest(const tb::Symbol& symbol, double oi_usdt, int64_t ts_ns) {
    std::lock_guard lock(mutex_);
    const std::string key = symbol.get();
    auto it = oi_trackers_.find(key);
    if (it == oi_trackers_.end()) {
        it = oi_trackers_.emplace(key, indicators::OiTracker(20)).first;
    }
    auto last_ticker = last_tickers_.find(key);
    double price = (last_ticker != last_tickers_.end())
        ? (last_ticker->second.bid.get() + last_ticker->second.ask.get()) * 0.5
        : 0.0;
    it->second.on_oi_update(oi_usdt, price, ts_ns);
}

void FeatureEngine::update_funding_rate(const tb::Symbol& symbol, double rate_8h) {
    std::lock_guard lock(mutex_);
    funding_rates_[symbol.get()] = rate_8h;
}

void FeatureEngine::update_leverage(const tb::Symbol& symbol, double leverage) {
    std::lock_guard lock(mutex_);
    if (leverage > 0.0 && std::isfinite(leverage)) {
        leverages_[symbol.get()] = leverage;
    }
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

    // BUG-S29-06: NaN bid or ask from normalizer would produce NaN mid_price,
    // poisoning all downstream microstructure features.
    if (!std::isfinite(ticker.bid.get()) || !std::isfinite(ticker.ask.get()) ||
        ticker.bid.get() <= 0.0 || ticker.ask.get() <= 0.0) {
        logger_->warn("FeatureEngine", "compute_snapshot: невалидные bid/ask в тикере, пропуск",
            {{"symbol", symbol.get()},
             {"bid", std::to_string(ticker.bid.get())},
             {"ask", std::to_string(ticker.ask.get())}});
        return std::nullopt;
    }

    FeatureSnapshot snap;
    snap.symbol = symbol;
    snap.computed_at = clock_->now();
    snap.last_price = ticker.last_price;
    snap.mid_price = tb::Price{(ticker.bid.get() + ticker.ask.get()) * 0.5};
    snap.book_quality = book.quality();

    // Age of exchange data: time since the last trade was received (exchange staleness).
    // Falls back to time since ticker receipt if no trades have been seen yet.
    {
        auto it = last_trade_received_ns_.find(symbol.get());
        const int64_t last_data_ns = (it != last_trade_received_ns_.end())
            ? it->second
            : ticker.envelope.received_ts.get();
        // BUG-S35-02: negative age when NTP backward jump → staleness monitoring broken.
        const auto age_raw = snap.computed_at.get() - last_data_ns;
        snap.market_data_age_ns = tb::Timestamp{age_raw > 0 ? age_raw : decltype(age_raw){0}};
    }

    snap.technical = compute_technical(symbol);
    snap.microstructure = compute_microstructure(ticker, book);
    snap.execution_context = compute_execution_context(ticker, book);

    // run94: Spoof detection — нужны данные из microstructure (depth, top of book).
    if (snap.microstructure.book_imbalance_valid && snap.microstructure.liquidity_valid) {
        double top_bid_size = 0.0;
        double top_ask_size = 0.0;
        if (auto tob = book.top_of_book()) {
            top_bid_size = tob->bid_size.get() * tob->best_bid.get();
            top_ask_size = tob->ask_size.get() * tob->best_ask.get();
        }
        auto spoof = indicators::detect_spoofing(
            snap.microstructure.bid_depth_5_notional,
            snap.microstructure.ask_depth_5_notional,
            top_bid_size,
            top_ask_size,
            snap.microstructure.cancel_burst_intensity,
            snap.microstructure.refill_asymmetry);
        if (spoof.valid) {
            snap.technical.spoof_bid = spoof.spoof_bid_detected;
            snap.technical.spoof_ask = spoof.spoof_ask_detected;
            snap.technical.spoof_intensity = spoof.spoof_intensity;
            snap.technical.spoof_valid = true;
        }
    }

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
        // BUG-S29-05: atr.valid=true doesn't guarantee isfinite(atr.value)
        tf.atr_valid = atr.valid && std::isfinite(atr.value);
        tf.atr_14 = tf.atr_valid ? atr.value : 0.0;
        // Нормализация ATR относительно последней цены закрытия
        const double last_close = close.back();
        tf.atr_14_normalized = (tf.atr_valid && last_close > 0.0)
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

    // run93: Supertrend (10, 3) — ATR trend follower hard filter.
    {
        auto st = indicators_->supertrend(high, low, close, 10, 3.0);
        if (st.valid) {
            tf.supertrend_value = st.value;
            tf.supertrend_trend = st.trend;
            tf.supertrend_flipped = st.flipped;
            tf.supertrend_valid = true;
        }
    }

    // run93: Stochastic (5, 3, 3) — fast scalping oscillator.
    {
        auto stoch = indicators_->stochastic(high, low, close, 5, 3, 3);
        if (stoch.valid) {
            tf.stoch_k = stoch.k;
            tf.stoch_d = stoch.d;
            tf.stoch_overbought = stoch.overbought;
            tf.stoch_oversold = stoch.oversold;
            tf.stoch_bull_cross = stoch.bull_cross;
            tf.stoch_bear_cross = stoch.bear_cross;
            tf.stoch_valid = true;
        }
    }

    // run93: EMA pair (9/21) — micro-trend crossover.
    {
        auto ep = indicators_->ema_pair(close, 9, 21);
        if (ep.valid) {
            tf.ema_fast_9 = ep.ema_fast;
            tf.ema_slow_21 = ep.ema_slow;
            tf.ema_pair_trend = ep.trend;
            tf.ema_pair_bull_cross = ep.bull_cross;
            tf.ema_pair_bear_cross = ep.bear_cross;
            tf.ema_pair_separation_bps = ep.separation_bps;
            tf.ema_pair_valid = true;
        }
    }

    // run94: Anchored VWAP (session/daily).
    {
        auto av_it = avwap_trackers_.find(symbol.get());
        if (av_it != avwap_trackers_.end()) {
            double cur_price = close.empty() ? 0.0 : close.back();
            auto r = av_it->second.snapshot(cur_price);
            if (r.valid) {
                tf.avwap = r.vwap;
                tf.avwap_upper_1sigma = r.upper_1sigma;
                tf.avwap_lower_1sigma = r.lower_1sigma;
                tf.avwap_upper_2sigma = r.upper_2sigma;
                tf.avwap_lower_2sigma = r.lower_2sigma;
                tf.avwap_price_vs_vwap_bps = r.price_vs_vwap_bps;
                tf.avwap_valid = true;
            }
        }
    }

    // run94: CVD + divergence detection.
    {
        auto cvd_it = cvd_trackers_.find(symbol.get());
        if (cvd_it != cvd_trackers_.end()) {
            auto r = cvd_it->second.snapshot();
            if (r.valid) {
                tf.cvd = r.cvd;
                tf.cvd_change_recent = r.cvd_change_recent;
                tf.cvd_normalized = r.cvd_normalized;
                tf.cvd_bullish_divergence = r.bullish_divergence;
                tf.cvd_bearish_divergence = r.bearish_divergence;
                tf.cvd_valid = true;
            }
        }
    }

    // run94: Open Interest tracking.
    {
        auto oi_it = oi_trackers_.find(symbol.get());
        if (oi_it != oi_trackers_.end()) {
            auto r = oi_it->second.snapshot();
            if (r.valid) {
                tf.oi_current = r.oi_current;
                tf.oi_change_recent_pct = r.oi_change_recent_pct;
                tf.oi_trend_quadrant = r.trend_quadrant;
                tf.oi_valid = true;
            }
        }
    }

    // run94: Liquidity Sweep Detector — на последних 20 1m свечах.
    if (close.size() >= 12 && high.size() == close.size() && low.size() == close.size()) {
        auto sweep = indicators::detect_liquidity_sweep(high, low, close, 10, 0.5, 0.6);
        if (sweep.valid) {
            tf.liq_sweep_high = sweep.sweep_high_detected;
            tf.liq_sweep_low = sweep.sweep_low_detected;
            tf.liq_sweep_recovery_pct = sweep.recovery_pct;
            tf.liq_sweep_valid = true;
        }
    }

    // run94: Funding bias.
    {
        auto fr_it = funding_rates_.find(symbol.get());
        if (fr_it != funding_rates_.end()) {
            auto fb = indicators::evaluate_funding_bias(fr_it->second);
            if (fb.valid) {
                tf.funding_rate_8h = fb.funding_rate;
                tf.funding_crowding_side = fb.crowding_side;
                tf.funding_crowding_intensity = fb.crowding_intensity;
                tf.funding_recommended_bias = fb.recommended_bias;
                tf.funding_valid = true;
            }
        }
    }

    // run94: Liquidation cluster proxy.
    // Bug 5.3 fix: pass actual leverage from LeverageEngine decision (via update_leverage).
    // Fallback на 10× (conservative middle ground) если leverage не передан.
    if (tf.oi_valid && tf.funding_valid && tf.momentum_valid) {
        double actual_leverage = 10.0;
        auto lev_it = leverages_.find(symbol.get());
        if (lev_it != leverages_.end()) {
            actual_leverage = lev_it->second;
        }
        auto liq = indicators::estimate_liquidation_clusters(
            tf.oi_change_recent_pct, tf.funding_rate_8h, tf.momentum_5, actual_leverage);
        if (liq.valid) {
            tf.liq_upside_cluster_pct = liq.upside_liq_cluster_pct;
            tf.liq_downside_cluster_pct = liq.downside_liq_cluster_pct;
            tf.liq_cascade_risk_score = liq.cascade_risk_score;
            tf.liq_dominant_side = liq.dominant_side;
            tf.liq_valid = true;
        }
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
        // BUG-ML-17 fix: empty book should return 0.0 (no liquidity), not 1.0 (perfect balance)
        mf.liquidity_ratio = (total > 0.0)
            ? std::min(mf.bid_depth_5_notional, mf.ask_depth_5_notional) / (total * 0.5)
            : 0.0;
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
        // B33.1/B33.2: Cont, Kukanov & Stoikov (2014) weights — 70/30 imb/spread.
        // Saturation 100 bps для spread (typical 1m crypto).
        constexpr double kImbalanceWeight = 0.7;
        constexpr double kSpreadWeight    = 0.3;
        constexpr double kSpreadSatBps    = 100.0;
        const double imbalance_component = std::abs(mf.book_imbalance_5) * kImbalanceWeight;
        const double spread_component = std::min(mf.spread_bps / kSpreadSatBps, 1.0) * kSpreadWeight;
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
        // B12.1: slippage model — inverse-depth scaling. Threshold $1000 — для
        // crypto USDT-M futures это типичный depth слой. На micro-account
        // (ордер $5-10) этот threshold даёт depth_impact=0 → можно ужесточить
        // через config если требуется.
        constexpr double kThinDepthThresholdUsd = 1000.0;
        constexpr double kSlippageImpactMaxBps  = 5.0;
        constexpr double kSlippageMultiplier    = 1.5;
        double mid_px = (ticker.bid.get() + ticker.ask.get()) * 0.5;
        if (mid_px > 0.0) {
            double depth_usd = ec.immediate_liquidity * mid_px;
            if (depth_usd < kThinDepthThresholdUsd && depth_usd > 0.0) {
                depth_impact = std::min(kSlippageImpactMaxBps,
                    (kThinDepthThresholdUsd / depth_usd - 1.0) * kSlippageMultiplier);
            }
        }
    }
    ec.estimated_slippage_bps = base_slippage + depth_impact;
    ec.slippage_valid = (ticker.spread_bps > 0.0);

    // B12.2: крипто 24/7, но Bitget maintenance windows возможны.
    // Сейчас полагаемся на data freshness (is_feed_fresh ниже) как proxy:
    // если данные приходят — market open. Maintenance детектируется через
    // feed staleness + WS reconnect (gracefully degraded).
    ec.is_market_open = true;

    // Свежесть данных: время последнего тикера vs текущее время
    const int64_t now_ns = clock_->now().get();
    const int64_t received_ns = ticker.envelope.received_ts.get();
    ec.is_feed_fresh = (now_ns - received_ns) < config_.feed_freshness_ns;

    return ec;
}

} // namespace tb::features
