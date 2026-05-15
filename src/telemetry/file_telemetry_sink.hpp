#pragma once
/**
 * @file file_telemetry_sink.hpp
 * @brief Production telemetry sink — write JSON-lines to rotating files
 *
 * Outputs one TelemetryEnvelope per line in JSON format.
 * Rotates files when hitting the max size threshold.
 * Thread-safe via internal mutex.
 */
#include "telemetry/telemetry_sink.hpp"
#include <string>
#include <fstream>
#include <mutex>
#include <cstdint>

namespace tb::telemetry {

struct FileSinkConfig {
    std::string directory{"./telemetry"};
    std::string file_prefix{"decision_trace"};
    size_t max_file_bytes{50 * 1024 * 1024};  // 50 MB per file
    int max_files{20};                         // Keep last 20 files (~1 GB total)
};

class FileTelemetrySink final : public ITelemetrySink {
public:
    explicit FileTelemetrySink(FileSinkConfig config);
    ~FileTelemetrySink() override;

    VoidResult emit(const TelemetryEnvelope& envelope) override;
    VoidResult flush() override;

private:
    void rotate_if_needed();
    void open_new_file();
    std::string serialize_envelope(const TelemetryEnvelope& envelope) const;

    FileSinkConfig config_;
    std::ofstream file_;
    size_t current_file_bytes_{0};
    int file_index_{0};
    std::mutex mutex_;
};

} // namespace tb::telemetry
