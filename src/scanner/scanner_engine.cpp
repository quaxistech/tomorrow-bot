#include "scanner_engine.hpp"

#include <boost/json.hpp>
#include <algorithm>
#include <chrono>
#include <iomanip>
#include <sstream>

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

    logger_->info(kComp, "Starting market scan...",
        {{"top_n", std::to_string(config_.top_n)}});

    ScannerResult result;
    result.timestamp_ms = start_ms;

    // ── Step 1: Fetch instrument list ──
    auto symbols = fetch_symbols();
    result.total_universe_size = static_cast<int>(symbols.size());
    logger_->info(kComp, "Loaded " + std::to_string(symbols.size()) + " contracts");

    if (symbols.empty()) {
        result.errors.push_back("failed_to_fetch_symbols");
        logger_->error(kComp, "Failed to fetch contract list");
        return result;
    }

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
        return result;
    }

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
    for (auto& snap : candidates) {
        // Fetch orderbook
        snap.orderbook = fetch_orderbook(snap.symbol);

        // Fetch candles
        snap.candles = fetch_candles(snap.symbol);
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

    // ── Step 7: Select top-N (skip DoNotTrade pairs) ──
    for (auto& a : analyzed) {
        if (static_cast<int>(result.top_pairs.size()) >= config_.top_n) {
            result.rejected_pairs.push_back(std::move(a));
            continue;
        }
        if (a.trade_state == TradeState::DoNotTrade) {
            result.rejected_pairs.push_back(std::move(a));
            continue;
        }
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

    // Log rejected pairs if configured
    if (config_.log_rejected_pairs) {
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

} // namespace tb::scanner
