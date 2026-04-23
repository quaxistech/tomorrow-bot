/**
 * @file file_telemetry_sink.cpp
 * @brief Production telemetry sink — JSON-lines with file rotation
 */
#include "telemetry/file_telemetry_sink.hpp"
#include <boost/json.hpp>
#include <filesystem>
#include <algorithm>
#include <vector>
#include <regex>

namespace tb::telemetry {

FileTelemetrySink::FileTelemetrySink(FileSinkConfig config)
    : config_(std::move(config)) {
    std::filesystem::create_directories(config_.directory);
    open_new_file();
}

FileTelemetrySink::~FileTelemetrySink() {
    std::lock_guard lock(mutex_);
    if (file_.is_open()) {
        file_.flush();
        file_.close();
    }
}

VoidResult FileTelemetrySink::emit(const TelemetryEnvelope& envelope) {
    std::lock_guard lock(mutex_);
    rotate_if_needed();
    if (!file_.is_open()) {
        return ErrVoid(TbError::PersistenceError);
    }
    std::string line = serialize_envelope(envelope);
    line += '\n';
    file_.write(line.data(), static_cast<std::streamsize>(line.size()));
    current_file_bytes_ += line.size();
    return OkVoid();
}

VoidResult FileTelemetrySink::flush() {
    std::lock_guard lock(mutex_);
    if (file_.is_open()) {
        file_.flush();
    }
    return OkVoid();
}

void FileTelemetrySink::rotate_if_needed() {
    if (current_file_bytes_ < config_.max_file_bytes) return;
    file_.flush();
    file_.close();

    // Garbage-collect oldest files beyond max_files
    namespace fs = std::filesystem;
    std::vector<fs::path> existing;
    for (auto& entry : fs::directory_iterator(config_.directory)) {
        if (entry.is_regular_file() &&
            entry.path().filename().string().starts_with(config_.file_prefix)) {
            existing.push_back(entry.path());
        }
    }
    if (static_cast<int>(existing.size()) >= config_.max_files) {
        std::sort(existing.begin(), existing.end());
        int to_remove = static_cast<int>(existing.size()) - config_.max_files + 1;
        for (int i = 0; i < to_remove; ++i) {
            std::error_code ec;
            fs::remove(existing[static_cast<size_t>(i)], ec);
        }
    }

    open_new_file();
}

void FileTelemetrySink::open_new_file() {
    ++file_index_;
    auto now = std::chrono::system_clock::now();
    auto epoch_s = std::chrono::duration_cast<std::chrono::seconds>(
        now.time_since_epoch()).count();
    std::string filename = config_.file_prefix + "_" +
        std::to_string(epoch_s) + "_" +
        std::to_string(file_index_) + ".jsonl";
    std::string path = config_.directory + "/" + filename;
    file_.open(path, std::ios::out | std::ios::trunc);
    current_file_bytes_ = 0;
}

std::string FileTelemetrySink::serialize_envelope(const TelemetryEnvelope& envelope) const {
    boost::json::object obj;

    // Identity
    obj["seq"] = envelope.sequence_id;
    obj["corr_id"] = envelope.correlation_id.get();
    obj["ts"] = envelope.captured_at.get();
    obj["symbol"] = envelope.symbol.get();
    obj["strategy"] = envelope.strategy_id.get();
    obj["strategy_ver"] = envelope.strategy_version.get();
    obj["config_hash"] = envelope.config_hash.get();

    // Market data
    obj["last_px"] = envelope.last_price;
    obj["mid_px"] = envelope.mid_price;
    obj["spread_bps"] = envelope.spread_bps;

    // World state
    obj["world_state"] = static_cast<int>(envelope.world_state);
    obj["regime"] = static_cast<int>(envelope.regime_label);
    obj["regime_conf"] = envelope.regime_confidence;
    obj["uncertainty"] = static_cast<int>(envelope.uncertainty_level);
    obj["uncertainty_score"] = envelope.uncertainty_score;

    // Decisions
    obj["approved"] = envelope.trade_approved;
    obj["conviction"] = envelope.final_conviction;
    obj["risk_verdict"] = envelope.risk_verdict;
    obj["exec_style"] = envelope.execution_style;
    obj["exec_urgency"] = envelope.execution_urgency;
    obj["exec_cost_bps"] = envelope.execution_cost_bps;

    // Portfolio
    obj["exposure_pct"] = envelope.portfolio_exposure_pct;
    obj["daily_pnl"] = envelope.daily_pnl;
    obj["drawdown_pct"] = envelope.drawdown_pct;
    obj["open_positions"] = envelope.open_positions;

    // Latency
    obj["pipeline_ns"] = envelope.total_pipeline_ns;
    if (!envelope.latency_traces.empty()) {
        boost::json::array traces;
        for (const auto& t : envelope.latency_traces) {
            boost::json::object tr;
            tr["stage"] = t.stage;
            tr["dur_ns"] = t.duration_ns;
            traces.push_back(std::move(tr));
        }
        obj["latency"] = std::move(traces);
    }

    // Post-execution
    if (envelope.realized_pnl) obj["realized_pnl"] = *envelope.realized_pnl;
    if (envelope.slippage_bps) obj["slippage_bps"] = *envelope.slippage_bps;

    // Compact JSON strategy proposals and decisions
    if (!envelope.strategy_proposals_json.empty())
        obj["proposals"] = envelope.strategy_proposals_json;
    if (!envelope.decision_json.empty())
        obj["decision"] = envelope.decision_json;
    if (!envelope.risk_reasons_json.empty())
        obj["risk_reasons"] = envelope.risk_reasons_json;

    return boost::json::serialize(obj);
}

} // namespace tb::telemetry
