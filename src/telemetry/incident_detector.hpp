#pragma once
/**
 * @file incident_detector.hpp
 * @brief Production incident detection with automated mitigation hints
 *
 * Monitors runtime health signals and detects 6 incident types:
 * 1. Fill drift — divergence between expected and actual fill rates
 * 2. WS snapshot replay storm — repeated sequence numbers from exchange
 * 3. Partial fill deadlock — order stuck in PartiallyFilled for too long
 * 4. Venue degradation — high REST latency, elevated rejection rate
 * 5. Orphan hedge — one leg filled without matching opposite
 * 6. Config drift — config hash changes at runtime
 *
 * Each incident produces a structured IncidentReport with severity,
 * description, and recommended mitigation action.
 */
#include "common/types.hpp"
#include "logging/logger.hpp"
#include "metrics/metrics_registry.hpp"
#include <string>
#include <vector>
#include <deque>
#include <chrono>
#include <cstdint>
#include <mutex>

namespace tb::telemetry {

enum class IncidentSeverity { Info, Warning, Critical };
enum class IncidentType {
    FillDrift,
    WsReplayStorm,
    PartialFillDeadlock,
    VenueDegradation,
    OrphanHedge,
    ConfigDrift
};

struct IncidentReport {
    IncidentType type;
    IncidentSeverity severity;
    int64_t detected_at_ns;
    std::string description;
    std::string mitigation;
};

struct IncidentDetectorConfig {
    // Fill drift: ratio of fills-received / orders-sent drops below threshold
    double fill_drift_min_ratio{0.7};
    int64_t fill_drift_window_ns{300'000'000'000LL}; // 5 min

    // WS replay storm: >N duplicate sequence IDs in window
    int ws_replay_max_duplicates{10};
    int64_t ws_replay_window_ns{60'000'000'000LL}; // 1 min

    // Partial fill deadlock: order stuck >N seconds
    int64_t partial_fill_max_age_ns{120'000'000'000LL}; // 2 min

    // Venue degradation: avg REST latency > threshold
    double venue_max_latency_ms{5000.0};
    double venue_max_rejection_rate{0.3}; // 30%
    int64_t venue_window_ns{300'000'000'000LL}; // 5 min

    // Orphan hedge: unmatched leg > threshold after opening
    int64_t orphan_hedge_max_age_ns{60'000'000'000LL}; // 1 min

    // Config drift: hash comparison interval
    int64_t config_check_interval_ns{60'000'000'000LL}; // 1 min
};

class IncidentDetector {
public:
    explicit IncidentDetector(
        IncidentDetectorConfig config,
        std::shared_ptr<logging::ILogger> logger,
        std::shared_ptr<metrics::IMetricsRegistry> metrics);

    // --- Event ingestion ---
    void on_order_sent(int64_t now_ns);
    void on_fill_received(int64_t now_ns);
    void on_ws_sequence(int64_t seq_id, int64_t now_ns);
    void on_partial_fill(const std::string& order_id, int64_t now_ns);
    void on_partial_fill_completed(const std::string& order_id);
    void on_rest_latency(double latency_ms, int64_t now_ns);
    void on_order_rejected(int64_t now_ns);
    void on_hedge_leg_opened(const std::string& pair_id, int64_t now_ns);
    void on_hedge_leg_filled(const std::string& pair_id);
    void set_config_hash(const std::string& hash);

    // --- Periodic check (call once per tick or on timer) ---
    std::vector<IncidentReport> check(int64_t now_ns);

    // --- Query ---
    [[nodiscard]] const std::vector<IncidentReport>& recent_incidents() const { return recent_; }

private:
    void emit_incident(IncidentReport report);

    IncidentDetectorConfig config_;
    std::shared_ptr<logging::ILogger> logger_;
    std::shared_ptr<metrics::IMetricsRegistry> metrics_;
    std::shared_ptr<metrics::ICounter> incident_counter_;

    // Fill drift tracking
    std::deque<int64_t> order_sent_times_;
    std::deque<int64_t> fill_received_times_;

    // WS replay tracking
    std::deque<std::pair<int64_t, int64_t>> ws_sequences_; // (seq_id, timestamp)

    // Partial fill tracking
    struct PartialFillEntry { std::string order_id; int64_t started_ns; };
    std::vector<PartialFillEntry> partial_fills_;

    // Venue health tracking
    std::deque<std::pair<double, int64_t>> rest_latencies_; // (ms, timestamp)
    std::deque<int64_t> rejection_times_;

    // Orphan hedge tracking
    struct HedgeEntry { std::string pair_id; int64_t opened_ns; };
    std::vector<HedgeEntry> pending_hedges_;

    // Config drift
    std::string config_hash_;
    std::string initial_config_hash_;
    int64_t last_config_check_ns_{0};

    // Recent incidents (ring buffer of last 100)
    std::vector<IncidentReport> recent_;
    static constexpr size_t kMaxRecentIncidents = 100;

    std::mutex mutex_;
};

} // namespace tb::telemetry
