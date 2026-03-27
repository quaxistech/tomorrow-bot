#pragma once
/// @file numeric_utils.hpp
/// @brief Production-grade numeric safety utilities for trading systems
///
/// Provides unified NaN/Inf guards, safe division, clamped arithmetic,
/// and validation helpers used across indicators and ML modules.

#include <cmath>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace tb::numeric {

// ─── Constants ───────────────────────────────────────────────────────────────

inline constexpr double kEpsilon = 1e-9;
inline constexpr double kMinValidPrice = 1e-10;
inline constexpr double kMinVariance = 1e-12;
inline constexpr double kMaxReasonablePrice = 1e12;
inline constexpr double kMaxReasonableVolume = 1e15;

// ─── Finite checks ──────────────────────────────────────────────────────────

/// Returns true only if value is finite (not NaN, not Inf)
[[nodiscard]] inline bool is_finite(double v) noexcept {
    return std::isfinite(v);
}

/// Returns true if value is NaN or Inf
[[nodiscard]] inline bool is_bad(double v) noexcept {
    return !std::isfinite(v);
}

/// Sanitize: return value if finite, else fallback
[[nodiscard]] inline double sanitize(double v, double fallback = 0.0) noexcept {
    return std::isfinite(v) ? v : fallback;
}

// ─── Safe arithmetic ─────────────────────────────────────────────────────────

/// Safe division: returns fallback if divisor is near-zero or result is non-finite
[[nodiscard]] inline double safe_div(double num, double den, double fallback = 0.0) noexcept {
    if (std::abs(den) < kEpsilon) return fallback;
    double result = num / den;
    return std::isfinite(result) ? result : fallback;
}

/// Clamped value in [lo, hi], returns fallback if input is NaN/Inf
[[nodiscard]] inline double safe_clamp(double v, double lo, double hi, double fallback = 0.0) noexcept {
    if (!std::isfinite(v)) return fallback;
    return std::clamp(v, lo, hi);
}

/// Safe sqrt: returns 0 for negative inputs, NaN guard
[[nodiscard]] inline double safe_sqrt(double v) noexcept {
    if (!std::isfinite(v) || v < 0.0) return 0.0;
    return std::sqrt(v);
}

/// Safe log: returns fallback for non-positive inputs
[[nodiscard]] inline double safe_log(double v, double fallback = 0.0) noexcept {
    if (!std::isfinite(v) || v <= 0.0) return fallback;
    return std::log(v);
}

// ─── Validation ──────────────────────────────────────────────────────────────

/// Validate a price value: finite, positive, within reasonable bounds
[[nodiscard]] inline bool is_valid_price(double p) noexcept {
    return std::isfinite(p) && p > kMinValidPrice && p < kMaxReasonablePrice;
}

/// Validate volume: finite, non-negative
[[nodiscard]] inline bool is_valid_volume(double v) noexcept {
    return std::isfinite(v) && v >= 0.0 && v < kMaxReasonableVolume;
}

/// Validate a spread in basis points: finite, non-negative
[[nodiscard]] inline bool is_valid_spread_bps(double s) noexcept {
    return std::isfinite(s) && s >= 0.0;
}

/// Validate a probability value: finite, in [0, 1]
[[nodiscard]] inline bool is_valid_probability(double p) noexcept {
    return std::isfinite(p) && p >= 0.0 && p <= 1.0;
}

/// Validate a correlation value: finite, in [-1, 1]
[[nodiscard]] inline bool is_valid_correlation(double c) noexcept {
    return std::isfinite(c) && c >= -1.0 && c <= 1.0;
}

/// Check if a vector of doubles contains any NaN/Inf
[[nodiscard]] inline bool has_bad_values(const double* data, std::size_t n) noexcept {
    for (std::size_t i = 0; i < n; ++i) {
        if (!std::isfinite(data[i])) return true;
    }
    return false;
}

/// Validate an entire price series
template<typename Container>
[[nodiscard]] bool validate_price_series(const Container& prices) {
    if (prices.empty()) return false;
    for (const auto& p : prices) {
        if (!is_valid_price(p)) return false;
    }
    return true;
}

/// Validate that multiple series have equal length and minimum size
template<typename... Containers>
[[nodiscard]] bool validate_series_alignment(std::size_t min_size, const Containers&... series) {
    std::size_t sizes[] = { series.size()... };
    std::size_t expected = sizes[0];
    if (expected < min_size) return false;
    for (std::size_t s : sizes) {
        if (s != expected) return false;
    }
    return true;
}

// ─── Staleness ───────────────────────────────────────────────────────────────

/// Default stale threshold: 5 seconds in nanoseconds
inline constexpr std::int64_t kDefaultStalenessNs = 5'000'000'000LL;

/// Check if a timestamp is stale relative to now
[[nodiscard]] inline bool is_stale(std::int64_t computed_at_ns, std::int64_t now_ns,
                                    std::int64_t threshold_ns = kDefaultStalenessNs) noexcept {
    if (computed_at_ns <= 0 || now_ns <= 0) return true;
    return (now_ns - computed_at_ns) > threshold_ns;
}

} // namespace tb::numeric
