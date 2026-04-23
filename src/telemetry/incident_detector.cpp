/**
 * @file incident_detector.cpp
 * @brief Production incident detection and automated playbook triggers
 */
#include "telemetry/incident_detector.hpp"
#include <algorithm>
#include <unordered_set>

namespace tb::telemetry {

IncidentDetector::IncidentDetector(
    IncidentDetectorConfig config,
    std::shared_ptr<logging::ILogger> logger,
    std::shared_ptr<metrics::IMetricsRegistry> metrics)
    : config_(std::move(config))
    , logger_(std::move(logger))
    , metrics_(std::move(metrics)) {
    if (metrics_) {
        incident_counter_ = metrics_->counter("tb_incidents_total");
    }
}

// --- Event ingestion ---

void IncidentDetector::on_order_sent(int64_t now_ns) {
    std::lock_guard lock(mutex_);
    order_sent_times_.push_back(now_ns);
}

void IncidentDetector::on_fill_received(int64_t now_ns) {
    std::lock_guard lock(mutex_);
    fill_received_times_.push_back(now_ns);
}

void IncidentDetector::on_ws_sequence(int64_t seq_id, int64_t now_ns) {
    std::lock_guard lock(mutex_);
    ws_sequences_.emplace_back(seq_id, now_ns);
}

void IncidentDetector::on_partial_fill(const std::string& order_id, int64_t now_ns) {
    std::lock_guard lock(mutex_);
    // Avoid duplicates
    for (const auto& pf : partial_fills_) {
        if (pf.order_id == order_id) return;
    }
    partial_fills_.push_back({order_id, now_ns});
}

void IncidentDetector::on_partial_fill_completed(const std::string& order_id) {
    std::lock_guard lock(mutex_);
    partial_fills_.erase(
        std::remove_if(partial_fills_.begin(), partial_fills_.end(),
            [&](const auto& e) { return e.order_id == order_id; }),
        partial_fills_.end());
}

void IncidentDetector::on_rest_latency(double latency_ms, int64_t now_ns) {
    std::lock_guard lock(mutex_);
    rest_latencies_.emplace_back(latency_ms, now_ns);
}

void IncidentDetector::on_order_rejected(int64_t now_ns) {
    std::lock_guard lock(mutex_);
    rejection_times_.push_back(now_ns);
}

void IncidentDetector::on_hedge_leg_opened(const std::string& pair_id, int64_t now_ns) {
    std::lock_guard lock(mutex_);
    for (const auto& h : pending_hedges_) {
        if (h.pair_id == pair_id) return;
    }
    pending_hedges_.push_back({pair_id, now_ns});
}

void IncidentDetector::on_hedge_leg_filled(const std::string& pair_id) {
    std::lock_guard lock(mutex_);
    pending_hedges_.erase(
        std::remove_if(pending_hedges_.begin(), pending_hedges_.end(),
            [&](const auto& e) { return e.pair_id == pair_id; }),
        pending_hedges_.end());
}

void IncidentDetector::set_config_hash(const std::string& hash) {
    std::lock_guard lock(mutex_);
    config_hash_ = hash;
    if (initial_config_hash_.empty()) {
        initial_config_hash_ = hash;
    }
}

// --- Periodic check ---

std::vector<IncidentReport> IncidentDetector::check(int64_t now_ns) {
    std::lock_guard lock(mutex_);
    std::vector<IncidentReport> incidents;

    // 1. Fill drift detection
    {
        // Prune old entries
        while (!order_sent_times_.empty() &&
               (now_ns - order_sent_times_.front()) > config_.fill_drift_window_ns) {
            order_sent_times_.pop_front();
        }
        while (!fill_received_times_.empty() &&
               (now_ns - fill_received_times_.front()) > config_.fill_drift_window_ns) {
            fill_received_times_.pop_front();
        }

        size_t sent = order_sent_times_.size();
        size_t filled = fill_received_times_.size();
        if (sent >= 3) {  // Need minimum orders for meaningful ratio
            double ratio = static_cast<double>(filled) / static_cast<double>(sent);
            if (ratio < config_.fill_drift_min_ratio) {
                incidents.push_back({
                    IncidentType::FillDrift,
                    IncidentSeverity::Warning,
                    now_ns,
                    "Fill drift detected: " + std::to_string(filled) + "/" +
                        std::to_string(sent) + " fills in window (ratio=" +
                        std::to_string(ratio) + ")",
                    "PLAYBOOK: 1) Check WS fill channel connectivity. "
                    "2) Verify REST /fills endpoint returns expected fills. "
                    "3) If fills missing, force reconciliation via REST. "
                    "4) If persistent, halt trading and investigate order routing."
                });
            }
        }
    }

    // 2. WS snapshot replay storm
    {
        while (!ws_sequences_.empty() &&
               (now_ns - ws_sequences_.front().second) > config_.ws_replay_window_ns) {
            ws_sequences_.pop_front();
        }

        // Count duplicate sequence IDs
        std::unordered_set<int64_t> seen;
        int duplicates = 0;
        for (const auto& [seq, ts] : ws_sequences_) {
            if (!seen.insert(seq).second) {
                ++duplicates;
            }
        }

        if (duplicates > config_.ws_replay_max_duplicates) {
            incidents.push_back({
                IncidentType::WsReplayStorm,
                IncidentSeverity::Critical,
                now_ns,
                "WS replay storm: " + std::to_string(duplicates) +
                    " duplicate sequences in " +
                    std::to_string(config_.ws_replay_window_ns / 1'000'000'000LL) + "s window",
                "PLAYBOOK: 1) Disconnect and reconnect WS immediately. "
                "2) Re-subscribe to fill and order channels. "
                "3) Force full state reconciliation via REST. "
                "4) If persistent after reconnect, switch to REST-only mode."
            });
        }
    }

    // 3. Partial fill deadlock
    {
        for (const auto& pf : partial_fills_) {
            int64_t age_ns = now_ns - pf.started_ns;
            if (age_ns > config_.partial_fill_max_age_ns) {
                incidents.push_back({
                    IncidentType::PartialFillDeadlock,
                    IncidentSeverity::Warning,
                    now_ns,
                    "Partial fill deadlock: order " + pf.order_id +
                        " stuck for " + std::to_string(age_ns / 1'000'000'000LL) + "s",
                    "PLAYBOOK: 1) Cancel remaining quantity via REST. "
                    "2) If cancel fails, query order status from REST. "
                    "3) If order is already fully filled, reconcile portfolio. "
                    "4) Log orphan quantity for reconciliation."
                });
            }
        }
    }

    // 4. Venue degradation
    {
        while (!rest_latencies_.empty() &&
               (now_ns - rest_latencies_.front().second) > config_.venue_window_ns) {
            rest_latencies_.pop_front();
        }
        while (!rejection_times_.empty() &&
               (now_ns - rejection_times_.front()) > config_.venue_window_ns) {
            rejection_times_.pop_front();
        }

        if (rest_latencies_.size() >= 3) {
            double sum = 0.0;
            for (const auto& [lat, ts] : rest_latencies_) {
                sum += lat;
            }
            double avg_ms = sum / static_cast<double>(rest_latencies_.size());

            if (avg_ms > config_.venue_max_latency_ms) {
                incidents.push_back({
                    IncidentType::VenueDegradation,
                    IncidentSeverity::Warning,
                    now_ns,
                    "Venue degradation: avg REST latency " +
                        std::to_string(avg_ms) + "ms (threshold: " +
                        std::to_string(config_.venue_max_latency_ms) + "ms)",
                    "PLAYBOOK: 1) Widen spread tolerance for passive orders. "
                    "2) Reduce order frequency to avoid rate limits. "
                    "3) Switch to aggressive fills to avoid stale limits. "
                    "4) If latency > 10s, halt new entries and protect existing positions."
                });
            }
        }

        // Rejection rate check
        size_t total_actions = order_sent_times_.size() + rejection_times_.size();
        if (total_actions >= 5) {
            double rej_rate = static_cast<double>(rejection_times_.size()) /
                              static_cast<double>(total_actions);
            if (rej_rate > config_.venue_max_rejection_rate) {
                incidents.push_back({
                    IncidentType::VenueDegradation,
                    IncidentSeverity::Critical,
                    now_ns,
                    "High rejection rate: " +
                        std::to_string(static_cast<int>(rej_rate * 100)) +
                        "% (threshold: " +
                        std::to_string(static_cast<int>(config_.venue_max_rejection_rate * 100)) +
                        "%)",
                    "PLAYBOOK: 1) Check account margin and available balance. "
                    "2) Verify symbol still tradeable (not in maintenance). "
                    "3) Check for IP ban or rate limiting from exchange. "
                    "4) Halt trading if rejection rate > 50%."
                });
            }
        }
    }

    // 5. Orphan hedge
    {
        for (const auto& h : pending_hedges_) {
            int64_t age_ns = now_ns - h.opened_ns;
            if (age_ns > config_.orphan_hedge_max_age_ns) {
                incidents.push_back({
                    IncidentType::OrphanHedge,
                    IncidentSeverity::Critical,
                    now_ns,
                    "Orphan hedge: pair " + h.pair_id +
                        " has unmatched leg for " +
                        std::to_string(age_ns / 1'000'000'000LL) + "s",
                    "PLAYBOOK: 1) Check if second leg order is pending on exchange. "
                    "2) If order exists but unfilled, cancel and close orphan at market. "
                    "3) If order was rejected, immediately close orphan directional leg. "
                    "4) Record orphan unwind cost in pair economics."
                });
            }
        }
    }

    // 6. Config drift
    {
        if ((now_ns - last_config_check_ns_) > config_.config_check_interval_ns) {
            last_config_check_ns_ = now_ns;
            if (!initial_config_hash_.empty() && !config_hash_.empty() &&
                config_hash_ != initial_config_hash_) {
                incidents.push_back({
                    IncidentType::ConfigDrift,
                    IncidentSeverity::Critical,
                    now_ns,
                    "Config drift: hash changed from " +
                        initial_config_hash_.substr(0, 12) + " to " +
                        config_hash_.substr(0, 12),
                    "PLAYBOOK: 1) Verify config change was intentional. "
                    "2) Log full diff between old and new config. "
                    "3) If unintentional, restore original config and restart. "
                    "4) If intentional, update initial_config_hash and continue."
                });
            }
        }
    }

    // Emit all detected incidents
    for (auto& inc : incidents) {
        emit_incident(inc);
    }

    return incidents;
}

void IncidentDetector::emit_incident(IncidentReport report) {
    if (incident_counter_) {
        incident_counter_->increment();
    }

    // Log with severity
    std::string severity_str;
    switch (report.severity) {
        case IncidentSeverity::Info:     severity_str = "INFO"; break;
        case IncidentSeverity::Warning:  severity_str = "WARN"; break;
        case IncidentSeverity::Critical: severity_str = "CRIT"; break;
    }

    auto log_fn = (report.severity == IncidentSeverity::Critical)
        ? &logging::ILogger::error : &logging::ILogger::warn;

    (logger_.get()->*log_fn)("incident", report.description,
        {{"type", std::to_string(static_cast<int>(report.type))},
         {"severity", severity_str},
         {"mitigation", report.mitigation}});

    // Store in recent ring buffer
    if (recent_.size() >= kMaxRecentIncidents) {
        recent_.erase(recent_.begin());
    }
    recent_.push_back(std::move(report));
}

} // namespace tb::telemetry
