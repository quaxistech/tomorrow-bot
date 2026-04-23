#pragma once
/**
 * @file runtime_manifest.hpp
 * @brief Immutable runtime manifest for production traceability.
 *
 * Captures build info (git SHA, version, build type) and runtime config
 * (config hash, exchange endpoints) into a single auditable record.
 * Logged at session start and included in every telemetry snapshot.
 */

#include <string>

namespace tb::app {

/// Immutable manifest — frozen at session start, never modified.
struct RuntimeManifest {
    std::string git_sha;          ///< Git commit hash (short, 12 chars)
    std::string version;          ///< Project version (semver)
    std::string build_type;       ///< CMake build type (Debug/Release/RelWithDebInfo)
    std::string config_hash;      ///< SHA-256 of production.yaml
    std::string exchange_endpoint;///< REST endpoint URL
    int64_t session_start_ns{0};  ///< Session start timestamp (monotonic ns)

    /// Build a manifest from compile-time defines and runtime config.
    static RuntimeManifest build(const std::string& config_hash,
                                  const std::string& exchange_endpoint,
                                  int64_t now_ns);
};

} // namespace tb::app
