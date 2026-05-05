#include "scanner_engine.hpp"

#include <boost/json.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <map>
#include <sstream>
#include <unordered_map>

namespace tb::scanner {

static constexpr char kComp[] = "ScannerEngine";

// Bitget API endpoints (USDT-M Futures)
static constexpr char kContractsPath[] = "/api/v2/mix/market/contracts";
static constexpr char kTickersPath[]   = "/api/v2/mix/market/tickers";
static constexpr char kDepthPath[]     = "/api/v2/mix/market/merge-depth";
static constexpr char kCandlesPath[]   = "/api/v2/mix/market/candles";

// ═══════════════════════════════════════════════════════════════════════════════
// Constructor / Destructor
// ═══════════════════════════════════════════════════════════════════════════════

ScannerEngine::ScannerEngine(ScannerConfig config,
                             std::shared_ptr<exchange::bitget::BitgetRestClient> rest_client,
                             std::shared_ptr<logging::ILogger> logger,
                             std::shared_ptr<metrics::IMetricsRegistry> metrics)
    : config_(std::move(config))
    , rest_client_(std::move(rest_client))
    , logger_(std::move(logger))
    , metrics_(std::move(metrics))
    , feature_calc_(config_)
    , trap_aggregator_(config_)
    , pair_filter_(config_)
    , pair_ranker_(config_)
    , bias_detector_(config_)
{
}

ScannerEngine::~ScannerEngine() {
    stop_rotation();
}

// ═══════════════════════════════════════════════════════════════════════════════
// Main Scan
// ═══════════════════════════════════════════════════════════════════════════════

ScannerResult ScannerEngine::scan() {
    auto scan_start = std::chrono::system_clock::now();
    auto start_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        scan_start.time_since_epoch()).count();
    auto deadline_ms = start_ms + config_.scan_timeout_ms;

    // Circuit breaker check
    if (is_circuit_breaker_open()) {
        logger_->warn(kComp, "Scan skipped — circuit breaker OPEN",
            {{"failures", std::to_string(consecutive_api_failures_)},
             {"reset_ms", std::to_string(config_.circuit_breaker_reset_ms)}});
        ScannerResult result;
        result.timestamp_ms = start_ms;
        result.errors.push_back("circuit_breaker_open");
        return result;
    }

    logger_->info(kComp, "Starting market scan...",
        {{"top_n", std::to_string(config_.top_n)},
         {"timeout_ms", std::to_string(config_.scan_timeout_ms)},
         {"diversification", config_.enable_diversification ? "on" : "off"}});

    ScannerResult result;
    result.timestamp_ms = start_ms;

    // ── Step 1: Fetch instrument list ──
    auto symbols = fetch_symbols();
    result.total_universe_size = static_cast<int>(symbols.size());
    logger_->info(kComp, "Loaded " + std::to_string(symbols.size()) + " contracts");

    if (symbols.empty()) {
        result.errors.push_back("failed_to_fetch_symbols");
        logger_->error(kComp, "Failed to fetch contract list");
        record_api_failure();
        return result;
    }
    record_api_success();

    // Build symbol info map
    std::unordered_map<std::string, SymbolInfo> sym_info_map;
    for (const auto& si : symbols) {
        sym_info_map[si.symbol] = si;
    }

    // ── Step 2: Fetch all tickers (bulk) ──
    auto snapshots = fetch_tickers();
    logger_->info(kComp, "Loaded " + std::to_string(snapshots.size()) + " tickers");

    if (snapshots.empty()) {
        result.errors.push_back("failed_to_fetch_tickers");
        logger_->error(kComp, "Failed to fetch tickers");
        record_api_failure();
        return result;
    }
    record_api_success();

    // Merge instrument info into snapshots
    for (auto& snap : snapshots) {
        auto it = sym_info_map.find(snap.symbol);
        if (it != sym_info_map.end()) {
            snap.quantity_precision = it->second.quantity_precision;
            snap.price_precision = it->second.price_precision;
            snap.min_trade_usdt = it->second.min_trade_usdt;
            snap.min_quantity = it->second.min_quantity;
            snap.status = it->second.status;
        }
    }

    // ── Step 3: Pre-filter by volume and spread to limit API calls ──
    // Sort by turnover descending
    std::sort(snapshots.begin(), snapshots.end(),
        [](const MarketSnapshot& a, const MarketSnapshot& b) {
            return a.turnover_24h > b.turnover_24h;
        });

    std::vector<MarketSnapshot> candidates;
    for (const auto& snap : snapshots) {
        if (snap.status != "online" && snap.status != "normal") continue;
        if (snap.turnover_24h < config_.prefilter_min_volume_usdt) continue;

        // Quick spread check from ticker
        double mid = (snap.best_bid + snap.best_ask) / 2.0;
        double spread_bps = (mid > 0.0) ? (snap.best_ask - snap.best_bid) / mid * 10000.0 : 999.0;
        if (spread_bps > config_.prefilter_max_spread_bps) continue;

        // Check blacklist
        bool blacklisted = false;
        for (const auto& bl : config_.blacklist) {
            if (snap.symbol == bl) { blacklisted = true; break; }
        }
        if (blacklisted) continue;

        candidates.push_back(snap);
        if (static_cast<int>(candidates.size()) >= config_.max_candidates_detailed) break;
    }

    logger_->info(kComp, "Pre-filtered to " + std::to_string(candidates.size()) + " candidates");

    if (candidates.empty()) {
        result.errors.push_back("no_candidates_after_prefilter");
        logger_->warn(kComp, "No candidates passed pre-filter");
        return result;
    }

    // ── Step 4: Fetch detailed data (orderbook + candles) per candidate ──
    // Respect scan timeout: stop fetching if we're running out of time
    for (auto& snap : candidates) {
        auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        if (now_ms >= deadline_ms) {
            logger_->warn(kComp, "Scan timeout reached during data fetch",
                {{"fetched", std::to_string(&snap - &candidates[0])},
                 {"total", std::to_string(candidates.size())},
                 {"timeout_ms", std::to_string(config_.scan_timeout_ms)}});
            result.errors.push_back("scan_timeout_during_fetch");
            break;
        }

        // Circuit breaker guard
        if (is_circuit_breaker_open()) {
            logger_->warn(kComp, "Circuit breaker tripped during data fetch");
            result.errors.push_back("circuit_breaker_during_fetch");
            break;
        }

        // Fetch orderbook
        snap.orderbook = fetch_orderbook(snap.symbol);
        now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        if (now_ms >= deadline_ms) {
            result.errors.push_back("scan_timeout_after_orderbook_fetch");
            break;
        }
        if (snap.orderbook.timestamp_ms > 0 &&
            now_ms - snap.orderbook.timestamp_ms > 10'000) {
            logger_->warn(kComp, "Skipping stale orderbook",
                {{"symbol", snap.symbol},
                 {"age_ms", std::to_string(now_ms - snap.orderbook.timestamp_ms)}});
            snap.orderbook = {};
        }
        if (snap.orderbook.bids.empty() && snap.orderbook.asks.empty()) {
            record_api_failure();
        } else {
            record_api_success();
        }

        // Fetch candles
        snap.candles = fetch_candles(snap.symbol);
        now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        if (now_ms >= deadline_ms) {
            result.errors.push_back("scan_timeout_after_candle_fetch");
            break;
        }
        if (snap.candles.empty()) {
            record_api_failure();
        } else {
            record_api_success();
        }
    }

    // ── Step 5: Full analysis pipeline for each candidate ──
    std::vector<SymbolAnalysis> analyzed;
    analyzed.reserve(candidates.size());

    for (const auto& snap : candidates) {
        SymbolAnalysis analysis;
        analysis.symbol = snap.symbol;
        analysis.quantity_precision = snap.quantity_precision;
        analysis.price_precision = snap.price_precision;
        analysis.min_trade_usdt = snap.min_trade_usdt;
        analysis.min_quantity = snap.min_quantity;
        analysis.last_price = snap.last_price;

        // 5a. Compute features (§5.3)
        analysis.features = feature_calc_.compute(snap);

        // 5b. Run trap detectors (§6)
        analysis.traps = trap_aggregator_.evaluate(snap, analysis.features);

        // 5c. Apply filters (§7)
        analysis.filter = pair_filter_.evaluate(snap, analysis.features, analysis.traps);

        if (!analysis.filter.passed()) {
            analysis.trade_state = TradeState::DoNotTrade;
            analysis.reasons.push_back(to_string(analysis.filter.reason));
            if (!analysis.filter.details.empty())
                analysis.reasons.push_back(analysis.filter.details);
            // Diagnostic: log first rejections
            if (result.rejected_pairs.size() < 5) {
                logger_->debug(kComp, "Candidate rejected",
                    {{"symbol", analysis.symbol},
                     {"reason", to_string(analysis.filter.reason)},
                     {"details", analysis.filter.details}});
            }
            result.rejected_pairs.push_back(std::move(analysis));
            continue;
        }

        // 5d. Compute score (§8)
        analysis.score = pair_ranker_.compute(analysis.features, analysis.traps, snap);

        // 5e. Detect bias (§9)
        auto bias_result = bias_detector_.detect(snap, analysis.features, analysis.traps);
        analysis.bias = bias_result.direction;
        analysis.bias_confidence = bias_result.confidence;
        analysis.reasons = std::move(bias_result.reasons);

        // 5f. Determine trade state (using configurable thresholds)
        if (analysis.traps.total_risk > config_.trade_state_max_trap_risk) {
            analysis.trade_state = TradeState::DoNotTrade;
            analysis.reasons.push_back("elevated_trap_risk");
        } else if (analysis.score.total > config_.trade_state_min_score &&
                   analysis.score.confidence > config_.trade_state_min_confidence) {
            analysis.trade_state = TradeState::TradeAllowed;
        } else {
            analysis.trade_state = TradeState::Neutral;
        }

        // Add score reasons
        for (const auto& r : analysis.score.bonus_reasons)
            analysis.reasons.push_back(r);
        for (const auto& r : analysis.score.penalty_reasons)
            analysis.reasons.push_back(r);

        analyzed.push_back(std::move(analysis));
    }

    // ── Step 6: Sort by score descending ──
    std::sort(analyzed.begin(), analyzed.end(),
        [](const SymbolAnalysis& a, const SymbolAnalysis& b) {
            return a.score.total > b.score.total;
        });

    result.after_filter_count = static_cast<int>(analyzed.size());

    // ── Step 6b: Diversification constraints ──
    if (config_.enable_diversification) {
        size_t before = analyzed.size();
        diversify_basket(analyzed, candidates);
        logger_->info(kComp, "Diversification: " + std::to_string(before)
            + " → " + std::to_string(analyzed.size()) + " pairs",
            {{"max_correlation", std::to_string(config_.max_correlation_in_basket)},
             {"max_per_sector", std::to_string(config_.max_pairs_per_sector)}});
    }

    // ── Step 7: Select top-N (skip DoNotTrade pairs) with traceability ──
    for (auto& a : analyzed) {
        if (static_cast<int>(result.top_pairs.size()) >= config_.top_n) {
            logger_->debug(kComp, "TRACE: пара отсеяна — корзина полна",
                {{"symbol", a.symbol}, {"score", std::to_string(a.score.total)}});
            result.rejected_pairs.push_back(std::move(a));
            continue;
        }
        if (a.trade_state == TradeState::DoNotTrade) {
            std::string reasons_str;
            for (const auto& r : a.reasons) {
                if (!reasons_str.empty()) reasons_str += "; ";
                reasons_str += r;
            }
            logger_->debug(kComp, "TRACE: пара отсеяна — DoNotTrade",
                {{"symbol", a.symbol}, {"reasons", reasons_str}});
            result.rejected_pairs.push_back(std::move(a));
            continue;
        }
        logger_->info(kComp, "TRACE: пара ВЫБРАНА в корзину",
            {{"symbol", a.symbol},
             {"score", std::to_string(a.score.total)},
             {"bias", to_string(a.bias)},
             {"state", to_string(a.trade_state)},
             {"confidence", std::to_string(a.score.confidence)}});
        result.top_pairs.push_back(std::move(a));
    }

    // ── Step 8: Timing & logging ──
    auto scan_end = std::chrono::system_clock::now();
    result.scan_duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        scan_end - scan_start).count();

    // Log results (§15: explainability)
    logger_->info(kComp, "=== SCANNER RESULTS ===");
    for (int i = 0; i < static_cast<int>(result.top_pairs.size()); ++i) {
        const auto& p = result.top_pairs[i];
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2);
        oss << "#" << (i + 1) << " " << p.symbol
            << " | Score: " << p.score.total
            << " (Liq:" << p.score.liquidity_score
            << " Spr:" << p.score.spread_score
            << " Vol:" << p.score.volatility_score
            << " OB:" << p.score.orderbook_score
            << " TQ:" << p.score.trend_quality_score
            << " -Trap:" << p.score.trap_risk_penalty
            << ")"
            << " | Bias: " << to_string(p.bias)
            << " | State: " << to_string(p.trade_state)
            << " | Conf: " << p.score.confidence;

        logger_->info(kComp, oss.str(),
            {{"rank", std::to_string(i + 1)},
             {"symbol", p.symbol},
             {"score", std::to_string(p.score.total)},
             {"bias", to_string(p.bias)},
             {"state", to_string(p.trade_state)}});
    }

    logger_->info(kComp, "Scan complete",
        {{"duration_ms", std::to_string(result.scan_duration_ms)},
         {"universe", std::to_string(result.total_universe_size)},
         {"candidates", std::to_string(candidates.size())},
         {"passed_filters", std::to_string(result.after_filter_count)},
         {"selected", std::to_string(result.top_pairs.size())},
         {"rejected", std::to_string(result.rejected_pairs.size())}});

    // Log rejection reasons summary at INFO level when all candidates fail
    if (result.top_pairs.empty() && !result.rejected_pairs.empty()) {
        std::map<std::string, int> reason_counts;
        for (const auto& r : result.rejected_pairs) {
            reason_counts[to_string(r.filter.reason)]++;
        }
        std::string summary;
        for (const auto& [reason, count] : reason_counts) {
            if (!summary.empty()) summary += ", ";
            summary += reason + "=" + std::to_string(count);
        }
        logger_->info(kComp, "ALL candidates rejected. Breakdown: " + summary);

        // Log first 10 rejections with details
        int logged_rejected = 0;
        for (const auto& r : result.rejected_pairs) {
            if (logged_rejected >= 10) break;
            logger_->info(kComp, "Rejected: " + r.symbol,
                {{"filter", to_string(r.filter.reason)},
                 {"details", r.filter.details}});
            logged_rejected++;
        }
    } else if (config_.log_rejected_pairs) {
        int logged_rejected = 0;
        for (const auto& r : result.rejected_pairs) {
            if (logged_rejected >= 5) break;
            std::string reasons_str;
            for (const auto& reason : r.reasons) {
                if (!reasons_str.empty()) reasons_str += ", ";
                reasons_str += reason;
            }
            logger_->debug(kComp, "Rejected: " + r.symbol,
                {{"reasons", reasons_str},
                 {"filter", to_string(r.filter.reason)}});
            logged_rejected++;
        }
    }

    // Store result
    {
        std::lock_guard lock(mutex_);
        last_result_ = result;
    }

    return result;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Rotation
// ═══════════════════════════════════════════════════════════════════════════════

void ScannerEngine::start_rotation(ScannerCallback callback) {
    stop_rotation();

    rotation_callback_ = std::move(callback);
    rotation_running_.store(true);

    int interval_sec = config_.rotation_interval_hours * 3600;

    rotation_thread_ = std::thread([this, interval_sec]() {
        while (rotation_running_.load()) {
            // Sleep in 10s increments for responsive shutdown
            for (int elapsed = 0; elapsed < interval_sec && rotation_running_.load();
                 elapsed += 10) {
                std::this_thread::sleep_for(std::chrono::seconds(10));
            }
            if (!rotation_running_.load()) break;

            try {
                logger_->info(kComp, "Scheduled rotation scan starting...");
                auto result = scan();
                if (rotation_callback_) {
                    rotation_callback_(result);
                }
            } catch (const std::exception& e) {
                logger_->error(kComp, "Rotation scan failed",
                    {{"error", e.what()}});
            }
        }
    });
}

void ScannerEngine::stop_rotation() {
    rotation_running_.store(false);
    if (rotation_thread_.joinable()) {
        rotation_thread_.join();
    }
}

ScannerResult ScannerEngine::last_result() const {
    std::lock_guard lock(mutex_);
    return last_result_;
}

std::vector<std::string> ScannerEngine::selected_symbols() const {
    std::lock_guard lock(mutex_);
    return last_result_.selected_symbols();
}

// ═══════════════════════════════════════════════════════════════════════════════
// Data Collection
// ═══════════════════════════════════════════════════════════════════════════════

double ScannerEngine::parse_double(const std::string& s) {
    if (s.empty()) return 0.0;
    try { return std::stod(s); }
    catch (...) { return 0.0; }
}

std::vector<ScannerEngine::SymbolInfo> ScannerEngine::fetch_symbols() {
    std::vector<SymbolInfo> result;

    std::string query = "productType=" + config_.product_type;
    auto resp = rest_client_->get(std::string(kContractsPath), query);

    if (!resp.success || resp.status_code != 200) {
        logger_->error(kComp, "Failed to fetch contracts",
            {{"status", std::to_string(resp.status_code)},
             {"error", resp.error_message}});
        return result;
    }

    try {
        auto doc = boost::json::parse(resp.body);
        auto& root = doc.as_object();
        if (root.at("code").as_string() != "00000") return result;

        auto& data = root.at("data").as_array();
        result.reserve(data.size());

        for (const auto& item : data) {
            auto& obj = item.as_object();
            SymbolInfo si;
            si.symbol = std::string(obj.at("symbol").as_string());
            si.base_coin = std::string(obj.at("baseCoin").as_string());

            if (obj.contains("symbolStatus"))
                si.status = std::string(obj.at("symbolStatus").as_string());
            else if (obj.contains("status"))
                si.status = std::string(obj.at("status").as_string());

            if (obj.contains("pricePlace"))
                si.price_precision = static_cast<int>(parse_double(
                    std::string(obj.at("pricePlace").as_string())));
            if (obj.contains("volumePlace"))
                si.quantity_precision = static_cast<int>(parse_double(
                    std::string(obj.at("volumePlace").as_string())));
            if (obj.contains("minTradeUSDT"))
                si.min_trade_usdt = parse_double(
                    std::string(obj.at("minTradeUSDT").as_string()));
            if (obj.contains("minTradeNum"))
                si.min_quantity = parse_double(
                    std::string(obj.at("minTradeNum").as_string()));

            // Only include USDT-quoted pairs
            if (obj.contains("quoteCoin")) {
                std::string quote = std::string(obj.at("quoteCoin").as_string());
                if (quote != "USDT") continue;
            }

            result.push_back(std::move(si));
        }
    } catch (const std::exception& e) {
        logger_->error(kComp, "Failed to parse contracts response",
            {{"error", e.what()}});
    }

    return result;
}

std::vector<MarketSnapshot> ScannerEngine::fetch_tickers() {
    std::vector<MarketSnapshot> result;

    std::string query = "productType=" + config_.product_type;
    auto resp = rest_client_->get(std::string(kTickersPath), query);

    if (!resp.success || resp.status_code != 200) {
        logger_->error(kComp, "Failed to fetch tickers",
            {{"status", std::to_string(resp.status_code)},
             {"error", resp.error_message}});
        return result;
    }

    try {
        auto doc = boost::json::parse(resp.body);
        auto& root = doc.as_object();
        if (root.at("code").as_string() != "00000") return result;

        auto& data = root.at("data").as_array();
        result.reserve(data.size());

        auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        for (const auto& item : data) {
            auto& obj = item.as_object();
            MarketSnapshot snap;
            snap.symbol = std::string(obj.at("symbol").as_string());

            // Parse ticker fields (Bitget returns strings)
            auto pv = [&](const char* key) -> double {
                if (!obj.contains(key)) return 0.0;
                auto& v = obj.at(key);
                if (v.is_string()) return parse_double(std::string(v.as_string()));
                if (v.is_double()) return v.as_double();
                if (v.is_int64()) return static_cast<double>(v.as_int64());
                return 0.0;
            };

            snap.last_price = pv("lastPr");
            snap.best_bid = pv("bidPr");
            snap.best_ask = pv("askPr");
            snap.high_24h = pv("high24h");
            snap.low_24h = pv("low24h");
            snap.open_24h = pv("open24h");
            snap.change_24h_pct = pv("change24h") * 100.0;  // A3 fix: Bitget возвращает ratio (0.02), нормализуем в percent (2.0)
            snap.volume_24h = pv("baseVolume");
            snap.turnover_24h = pv("quoteVolume");
            snap.funding_rate = pv("fundingRate");
            snap.open_interest = pv("holdingAmount");

            if (obj.contains("ts")) {
                auto& ts = obj.at("ts");
                if (ts.is_string()) snap.collected_at_ms = static_cast<int64_t>(
                    parse_double(std::string(ts.as_string())));
                else if (ts.is_int64()) snap.collected_at_ms = ts.as_int64();
            }
            if (snap.collected_at_ms == 0) snap.collected_at_ms = now_ms;

            // Build minimal orderbook from ticker bid/ask
            if (snap.best_bid > 0.0 && snap.best_ask > 0.0) {
                snap.orderbook.bids.push_back({snap.best_bid, 0.0});
                snap.orderbook.asks.push_back({snap.best_ask, 0.0});
            }

            result.push_back(std::move(snap));
        }
    } catch (const std::exception& e) {
        logger_->error(kComp, "Failed to parse tickers response",
            {{"error", e.what()}});
    }

    return result;
}

OrderBookSnapshot ScannerEngine::fetch_orderbook(const std::string& symbol) {
    OrderBookSnapshot ob;

    std::string query = "symbol=" + symbol +
                        "&productType=" + config_.product_type +
                        "&limit=" + std::to_string(config_.orderbook_depth);

    auto resp = rest_client_->get(std::string(kDepthPath), query);

    if (!resp.success || resp.status_code != 200) {
        logger_->debug(kComp, "Failed to fetch orderbook for " + symbol);
        return ob;
    }

    try {
        auto doc = boost::json::parse(resp.body);
        auto& root = doc.as_object();
        if (root.at("code").as_string() != "00000") return ob;

        auto& data = root.at("data").as_object();

        // Helper: parse value that may be string or numeric
        auto to_double = [](const boost::json::value& v) -> double {
            if (v.is_string()) return parse_double(std::string(v.as_string()));
            if (v.is_double()) return v.as_double();
            if (v.is_int64()) return static_cast<double>(v.as_int64());
            if (v.is_uint64()) return static_cast<double>(v.as_uint64());
            return 0.0;
        };

        // Bids: [[price, size], ...]
        if (data.contains("bids")) {
            for (const auto& level : data.at("bids").as_array()) {
                auto& arr = level.as_array();
                if (arr.size() >= 2) {
                    double price = to_double(arr[0]);
                    double qty = to_double(arr[1]);
                    ob.bids.push_back({price, qty});
                }
            }
        }

        // Asks: [[price, size], ...]
        if (data.contains("asks")) {
            for (const auto& level : data.at("asks").as_array()) {
                auto& arr = level.as_array();
                if (arr.size() >= 2) {
                    double price = to_double(arr[0]);
                    double qty = to_double(arr[1]);
                    ob.asks.push_back({price, qty});
                }
            }
        }

        if (data.contains("ts")) {
            ob.timestamp_ms = static_cast<int64_t>(to_double(data.at("ts")));
        }
    } catch (const std::exception& e) {
        logger_->debug(kComp, "Failed to parse orderbook for " + symbol,
            {{"error", e.what()}});
    }

    return ob;
}

std::vector<CandleData> ScannerEngine::fetch_candles(const std::string& symbol) {
    std::vector<CandleData> result;

    std::string query = "symbol=" + symbol +
                        "&productType=" + config_.product_type +
                        "&granularity=" + config_.candle_interval +
                        "&limit=" + std::to_string(config_.candle_count);

    auto resp = rest_client_->get(std::string(kCandlesPath), query);

    if (!resp.success || resp.status_code != 200) {
        logger_->debug(kComp, "Failed to fetch candles for " + symbol);
        return result;
    }

    try {
        auto doc = boost::json::parse(resp.body);
        auto& root = doc.as_object();
        if (root.at("code").as_string() != "00000") return result;

        auto& data = root.at("data").as_array();
        result.reserve(data.size());

        for (const auto& item : data) {
            auto& arr = item.as_array();
            if (arr.size() < 6) continue;

            CandleData c;
            c.timestamp_ms = static_cast<int64_t>(
                parse_double(std::string(arr[0].as_string())));
            c.open = parse_double(std::string(arr[1].as_string()));
            c.high = parse_double(std::string(arr[2].as_string()));
            c.low = parse_double(std::string(arr[3].as_string()));
            c.close = parse_double(std::string(arr[4].as_string()));
            c.volume = parse_double(std::string(arr[5].as_string()));

            result.push_back(c);
        }

        // Bitget returns candles newest first — reverse to chronological order
        std::reverse(result.begin(), result.end());

    } catch (const std::exception& e) {
        logger_->debug(kComp, "Failed to parse candles for " + symbol,
            {{"error", e.what()}});
    }

    return result;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Circuit Breaker
// ═══════════════════════════════════════════════════════════════════════════════

bool ScannerEngine::is_circuit_breaker_open() const {
    if (consecutive_api_failures_ < config_.circuit_breaker_threshold) return false;
    // BUG-S36-01: system_clock can go backward on NTP correction → elapsed negative
    // → CB permanently stuck open. Use steady_clock for monotonic elapsed measurement.
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    // Auto-reset after cooldown; clamp negative elapsed to 0
    int64_t elapsed = now_ms - circuit_breaker_tripped_at_ms_;
    if (elapsed < 0) elapsed = 0;
    return elapsed < config_.circuit_breaker_reset_ms;
}

void ScannerEngine::record_api_failure() {
    ++consecutive_api_failures_;
    if (consecutive_api_failures_ >= config_.circuit_breaker_threshold) {
        // Use steady_clock to match is_circuit_breaker_open() (BUG-S36-01 fix)
        circuit_breaker_tripped_at_ms_ = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        logger_->warn(kComp, "Circuit breaker TRIPPED — too many API failures",
            {{"failures", std::to_string(consecutive_api_failures_)},
             {"threshold", std::to_string(config_.circuit_breaker_threshold)},
             {"reset_ms", std::to_string(config_.circuit_breaker_reset_ms)}});
    }
}

void ScannerEngine::record_api_success() {
    consecutive_api_failures_ = 0;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Basket Diversification
// ═══════════════════════════════════════════════════════════════════════════════

double ScannerEngine::compute_return_correlation(const std::vector<CandleData>& a,
                                                  const std::vector<CandleData>& b) {
    // Compute Pearson correlation of log returns
    size_t n = std::min(a.size(), b.size());
    if (n < 5) return 0.0;  // Not enough data

    std::vector<double> ra, rb;
    ra.reserve(n - 1);
    rb.reserve(n - 1);

    for (size_t i = 1; i < n; ++i) {
        // BUG-S11-03: only previous close was guarded; current close could be <=0
        if (a[i-1].close <= 0.0 || b[i-1].close <= 0.0
         || a[i].close  <= 0.0 || b[i].close  <= 0.0) continue;
        ra.push_back(std::log(a[i].close / a[i-1].close));
        rb.push_back(std::log(b[i].close / b[i-1].close));
    }

    if (ra.size() < 4) return 0.0;

    double sum_a = 0.0, sum_b = 0.0, sum_ab = 0.0, sum_a2 = 0.0, sum_b2 = 0.0;
    double m = static_cast<double>(ra.size());
    for (size_t i = 0; i < ra.size(); ++i) {
        sum_a += ra[i];
        sum_b += rb[i];
        sum_ab += ra[i] * rb[i];
        sum_a2 += ra[i] * ra[i];
        sum_b2 += rb[i] * rb[i];
    }

    const double var_a = m * sum_a2 - sum_a * sum_a;
    const double var_b = m * sum_b2 - sum_b * sum_b;
    double denom = std::sqrt(std::max(0.0, var_a) * std::max(0.0, var_b));
    if (!std::isfinite(denom) || denom < 1e-9) return 0.0;
    const double corr = (m * sum_ab - sum_a * sum_b) / denom;
    return std::isfinite(corr) ? std::clamp(corr, -1.0, 1.0) : 0.0;
}

void ScannerEngine::diversify_basket(std::vector<SymbolAnalysis>& ranked,
                                      const std::vector<MarketSnapshot>& snapshots) const {
    if (!config_.enable_diversification || ranked.size() <= 1) return;

    // Build candle map for correlation computation
    std::unordered_map<std::string, const std::vector<CandleData>*> candle_map;
    for (const auto& snap : snapshots) {
        if (!snap.candles.empty()) {
            candle_map[snap.symbol] = &snap.candles;
        }
    }

    // Token sector heuristic: group by base coin suffix patterns
    // BTC/ETH → L1, SOL/AVAX/DOT → L1-alt, DOGE/SHIB/PEPE → meme, etc.
    auto get_sector = [](const std::string& symbol) -> std::string {
        // Simple heuristic: extract base coin from "BTCUSDT" → "BTC"
        std::string base = symbol;
        auto pos = base.find("USDT");
        if (pos != std::string::npos) base = base.substr(0, pos);
        // Major L1s
        if (base == "BTC" || base == "ETH") return "major";
        if (base == "SOL" || base == "AVAX" || base == "DOT" || base == "ADA" ||
            base == "NEAR" || base == "APT" || base == "SUI" || base == "SEI") return "L1";
        if (base == "DOGE" || base == "SHIB" || base == "PEPE" || base == "FLOKI" ||
            base == "WIF" || base == "BONK" || base == "MEME") return "meme";
        if (base == "ARB" || base == "OP" || base == "MATIC" || base == "BASE" ||
            base == "IMX" || base == "STRK" || base == "ZK") return "L2";
        if (base == "LINK" || base == "AAVE" || base == "UNI" || base == "MKR" ||
            base == "SNX" || base == "CRV" || base == "COMP") return "defi";
        if (base == "FIL" || base == "AR" || base == "RENDER" || base == "RNDR" ||
            base == "AI16Z" || base == "FET" || base == "AGIX") return "infra";
        return "other";
    };

    // Greedy selection: iterate ranked pairs, add if passes diversification constraints
    std::vector<SymbolAnalysis> selected;
    std::map<std::string, int> sector_counts;

    for (auto& pair : ranked) {
        // Sector concentration check
        std::string sector = get_sector(pair.symbol);
        if (sector_counts[sector] >= config_.max_pairs_per_sector) {
            logger_->info(kComp, "DIVERSIFICATION: пропуск — сектор перегружен",
                {{"symbol", pair.symbol},
                 {"sector", sector},
                 {"count", std::to_string(sector_counts[sector])},
                 {"max", std::to_string(config_.max_pairs_per_sector)}});
            continue;
        }

        // Correlation check against already-selected pairs
        bool too_correlated = false;
        auto it_new = candle_map.find(pair.symbol);
        if (it_new != candle_map.end()) {
            for (const auto& sel : selected) {
                auto it_sel = candle_map.find(sel.symbol);
                if (it_sel == candle_map.end()) continue;
                double corr = compute_return_correlation(*it_new->second, *it_sel->second);
                if (std::abs(corr) > config_.max_correlation_in_basket) {
                    logger_->info(kComp, "DIVERSIFICATION: пропуск — высокая корреляция",
                        {{"symbol", pair.symbol},
                         {"correlated_with", sel.symbol},
                         {"correlation", std::to_string(corr)},
                         {"threshold", std::to_string(config_.max_correlation_in_basket)}});
                    too_correlated = true;
                    break;
                }
            }
        }
        if (too_correlated) continue;

        sector_counts[sector]++;
        selected.push_back(std::move(pair));
    }

    ranked = std::move(selected);
}

} // namespace tb::scanner
