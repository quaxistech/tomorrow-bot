/**
 * @file drift_monitor.cpp
 * @brief Реализация DriftMonitor — PSI, KS, Page-Hinkley, ADWIN
 */
#include "drift/drift_monitor.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <stdexcept>

namespace tb::drift {

// ============================================================
// Конструктор
// ============================================================

DriftMonitor::DriftMonitor(DriftConfig config)
    : config_(std::move(config)) {}

// ============================================================
// Регистрация
// ============================================================

void DriftMonitor::register_stream(const std::string& name, DriftType type) {
    std::lock_guard lock(mutex_);
    if (streams_.contains(name)) return;
    StreamState state;
    state.name = name;
    state.type = type;
    streams_.emplace(name, std::move(state));
}

// ============================================================
// Подача данных
// ============================================================

void DriftMonitor::push(const std::string& stream_name, double value) {
    std::lock_guard lock(mutex_);
    auto it = streams_.find(stream_name);
    if (it == streams_.end()) return;

    auto& s = it->second;

    // Заполняем reference до нужного размера, затем test
    if (s.reference.size() < config_.reference_window) {
        s.reference.push_back(value);
    } else {
        s.test.push_back(value);
        if (s.test.size() > config_.test_window) {
            s.test.pop_front();
        }
    }

    // Обновляем Page-Hinkley
    update_page_hinkley(s, value);

    // ADWIN
    s.adwin_window.push_back(value);
    s.adwin_total += value;
    s.adwin_count++;
    // Ограничиваем окно ADWIN
    const auto max_adwin = config_.reference_window + config_.test_window;
    while (s.adwin_window.size() > max_adwin) {
        s.adwin_total -= s.adwin_window.front();
        s.adwin_window.pop_front();
        if (s.adwin_count > 0) s.adwin_count--;
    }
}

void DriftMonitor::push_batch(const std::string& stream_name,
                               const std::vector<double>& values) {
    for (const auto v : values) {
        push(stream_name, v);
    }
}

// ============================================================
// Детекция
// ============================================================

DriftDetectionResult DriftMonitor::check(const std::string& stream_name) const {
    std::lock_guard lock(mutex_);
    auto it = streams_.find(stream_name);
    if (it == streams_.end()) {
        return DriftDetectionResult{.stream_name = stream_name};
    }

    const auto& s = it->second;
    DriftDetectionResult result;
    result.stream_name = s.name;
    result.drift_type = s.type;
    result.reference_count = s.reference.size();
    result.test_count = s.test.size();

    // Нужны данные в обоих окнах
    if (s.reference.size() < config_.psi_bins * 2 || s.test.size() < config_.psi_bins) {
        return result;
    }

    result.psi_value = compute_psi(s.reference, s.test);
    auto [ks_stat, ks_p] = compute_ks(s.reference, s.test);
    result.ks_statistic = ks_stat;
    result.ks_p_value = ks_p;

    result.page_hinkley_alarm = (s.ph_sum - s.ph_min) > config_.page_hinkley_threshold;

    // Для ADWIN нужен mutable — используем const_cast (state не меняется логически
    // при check, но внутренний метод может обновить маркер). Вместо этого вычисляем inline.
    result.adwin_change = false;
    if (s.adwin_window.size() >= 2 * config_.psi_bins) {
        const auto mid = s.adwin_window.size() / 2;
        double sum1 = 0, sum2 = 0;
        for (std::size_t i = 0; i < mid; ++i) sum1 += s.adwin_window[i];
        for (std::size_t i = mid; i < s.adwin_window.size(); ++i) sum2 += s.adwin_window[i];
        const double mean1 = sum1 / static_cast<double>(mid);
        const double mean2 = sum2 / static_cast<double>(s.adwin_window.size() - mid);
        const double n = static_cast<double>(s.adwin_window.size());
        const double eps_cut = std::sqrt(0.5 * (1.0 / static_cast<double>(mid) +
                                1.0 / static_cast<double>(s.adwin_window.size() - mid)) *
                                std::log(2.0 / config_.adwin_delta));
        result.adwin_change = std::abs(mean1 - mean2) >= eps_cut;
    }

    result.severity = classify_severity(result.psi_value, result.ks_p_value,
                                         result.page_hinkley_alarm, result.adwin_change);
    return result;
}

DriftSnapshot DriftMonitor::check_all() const {
    std::lock_guard lock(mutex_);
    DriftSnapshot snap;

    for (const auto& [name, _] : streams_) {
        // Вызываем без рекурсивного лока — перенесём логику inline
        // Для простоты unlock + relock не нужен, т.к. всё под одним lock
    }

    // Разблокируем и вызываем check() который возьмёт свой лок
    // Нет — это deadlock. Скопируем имена и вызовем без блокировки.
    std::vector<std::string> names;
    names.reserve(streams_.size());
    for (const auto& [name, _] : streams_) {
        names.push_back(name);
    }

    // Отпускаем лок, затем проходим по каждому
    // Но мы уже под lock_guard. Придётся дублировать логику check() без лока.
    for (const auto& [name, s] : streams_) {
        DriftDetectionResult result;
        result.stream_name = s.name;
        result.drift_type = s.type;
        result.reference_count = s.reference.size();
        result.test_count = s.test.size();

        if (s.reference.size() >= config_.psi_bins * 2 && s.test.size() >= config_.psi_bins) {
            result.psi_value = compute_psi(s.reference, s.test);
            auto [ks_stat, ks_p] = compute_ks(s.reference, s.test);
            result.ks_statistic = ks_stat;
            result.ks_p_value = ks_p;
            result.page_hinkley_alarm = (s.ph_sum - s.ph_min) > config_.page_hinkley_threshold;

            if (s.adwin_window.size() >= 2 * config_.psi_bins) {
                const auto mid = s.adwin_window.size() / 2;
                double sum1 = 0, sum2 = 0;
                for (std::size_t i = 0; i < mid; ++i) sum1 += s.adwin_window[i];
                for (std::size_t i = mid; i < s.adwin_window.size(); ++i) sum2 += s.adwin_window[i];
                const double mean1 = sum1 / static_cast<double>(mid);
                const double mean2 = sum2 / static_cast<double>(s.adwin_window.size() - mid);
                const double eps_cut = std::sqrt(0.5 * (1.0 / static_cast<double>(mid) +
                                        1.0 / static_cast<double>(s.adwin_window.size() - mid)) *
                                        std::log(2.0 / config_.adwin_delta));
                result.adwin_change = std::abs(mean1 - mean2) >= eps_cut;
            }

            result.severity = classify_severity(result.psi_value, result.ks_p_value,
                                                 result.page_hinkley_alarm, result.adwin_change);
        }

        if (result.severity == DriftSeverity::Warning) snap.warning_count++;
        if (result.severity == DriftSeverity::Critical) {
            snap.critical_count++;
            snap.any_critical = true;
        }
        snap.results.push_back(std::move(result));
    }

    return snap;
}

// ============================================================
// Управление
// ============================================================

void DriftMonitor::reset_reference(const std::string& stream_name) {
    std::lock_guard lock(mutex_);
    auto it = streams_.find(stream_name);
    if (it == streams_.end()) return;
    auto& s = it->second;
    // Текущее test-окно становится новым reference
    s.reference = s.test;
    s.test.clear();
    // Сброс Page-Hinkley
    s.ph_sum = 0;
    s.ph_min = 0;
    s.ph_mean = 0;
    s.ph_count = 0;
}

std::vector<std::string> DriftMonitor::stream_names() const {
    std::lock_guard lock(mutex_);
    std::vector<std::string> names;
    names.reserve(streams_.size());
    for (const auto& [name, _] : streams_) {
        names.push_back(name);
    }
    return names;
}

std::size_t DriftMonitor::stream_count() const {
    std::lock_guard lock(mutex_);
    return streams_.size();
}

// ============================================================
// PSI — Population Stability Index
// ============================================================

double DriftMonitor::compute_psi(const std::deque<double>& reference,
                                  const std::deque<double>& test) const {
    if (reference.empty() || test.empty()) return 0.0;
    if (config_.psi_bins == 0) return 0.0;

    // Определяем границы бинов по reference
    auto ref_sorted = std::vector<double>(reference.begin(), reference.end());
    std::sort(ref_sorted.begin(), ref_sorted.end());

    const auto bins = config_.psi_bins;
    std::vector<double> edges(bins + 1);
    edges[0] = ref_sorted.front() - 1.0;
    edges[bins] = ref_sorted.back() + 1.0;
    for (std::size_t i = 1; i < bins; ++i) {
        const auto idx = static_cast<std::size_t>(
            static_cast<double>(i) / static_cast<double>(bins) *
            static_cast<double>(ref_sorted.size()));
        edges[i] = ref_sorted[std::min(idx, ref_sorted.size() - 1)];
    }

    // Считаем пропорции в каждом бине
    const double ref_n = static_cast<double>(reference.size());
    const double test_n = static_cast<double>(test.size());
    const double epsilon = 1e-6;

    double psi = 0.0;
    for (std::size_t b = 0; b < bins; ++b) {
        double ref_count = 0, test_count = 0;
        for (const auto v : reference) {
            if (v > edges[b] && v <= edges[b + 1]) ref_count++;
        }
        for (const auto v : test) {
            if (v > edges[b] && v <= edges[b + 1]) test_count++;
        }
        const double ref_pct = std::max(ref_count / ref_n, epsilon);
        const double test_pct = std::max(test_count / test_n, epsilon);
        psi += (test_pct - ref_pct) * std::log(test_pct / ref_pct);
    }

    return psi;
}

// ============================================================
// KS — Kolmogorov-Smirnov Test
// ============================================================

std::pair<double, double> DriftMonitor::compute_ks(
        const std::deque<double>& reference,
        const std::deque<double>& test) const {
    if (reference.empty() || test.empty()) return {0.0, 1.0};

    auto ref_sorted = std::vector<double>(reference.begin(), reference.end());
    auto test_sorted = std::vector<double>(test.begin(), test.end());
    std::sort(ref_sorted.begin(), ref_sorted.end());
    std::sort(test_sorted.begin(), test_sorted.end());

    double d_max = 0.0;
    std::size_t i = 0, j = 0;
    const double n1 = static_cast<double>(ref_sorted.size());
    const double n2 = static_cast<double>(test_sorted.size());

    while (i < ref_sorted.size() && j < test_sorted.size()) {
        const double cdf1 = static_cast<double>(i + 1) / n1;
        const double cdf2 = static_cast<double>(j + 1) / n2;

        if (ref_sorted[i] <= test_sorted[j]) {
            d_max = std::max(d_max, std::abs(cdf1 - static_cast<double>(j) / n2));
            ++i;
        } else {
            d_max = std::max(d_max, std::abs(static_cast<double>(i) / n1 - cdf2));
            ++j;
        }
    }
    // Оставшиеся
    while (i < ref_sorted.size()) {
        d_max = std::max(d_max, std::abs(static_cast<double>(i + 1) / n1 - 1.0));
        ++i;
    }
    while (j < test_sorted.size()) {
        d_max = std::max(d_max, std::abs(1.0 - static_cast<double>(j + 1) / n2));
        ++j;
    }

    // Approx p-value (asymptotic formula)
    const double en = std::sqrt(n1 * n2 / (n1 + n2));
    const double lambda = (en + 0.12 + 0.11 / en) * d_max;
    // Kolmogorov distribution approximation
    double p_value = 0.0;
    if (lambda > 0) {
        // Series expansion: Q(λ) = 2 * Σ_{k=1}^∞ (-1)^{k-1} * exp(-2k²λ²)
        for (int k = 1; k <= 100; ++k) {
            const double sign = (k % 2 == 1) ? 1.0 : -1.0;
            p_value += sign * std::exp(-2.0 * static_cast<double>(k * k) * lambda * lambda);
        }
        p_value *= 2.0;
    } else {
        p_value = 1.0;
    }
    p_value = std::clamp(p_value, 0.0, 1.0);

    return {d_max, p_value};
}

// ============================================================
// Page-Hinkley Change-Point Detector
// ============================================================

void DriftMonitor::update_page_hinkley(StreamState& state, double value) const {
    state.ph_count++;
    const double n = static_cast<double>(state.ph_count);
    state.ph_mean += (value - state.ph_mean) / n;  // online mean
    state.ph_sum += value - state.ph_mean - config_.page_hinkley_delta;
    state.ph_min = std::min(state.ph_min, state.ph_sum);
}

// ============================================================
// ADWIN (используется как inline check в detect methods)
// ============================================================

bool DriftMonitor::check_adwin(StreamState& state) const {
    if (state.adwin_window.size() < 2 * config_.psi_bins) return false;

    const auto mid = state.adwin_window.size() / 2;
    double sum1 = 0, sum2 = 0;
    for (std::size_t i = 0; i < mid; ++i) sum1 += state.adwin_window[i];
    for (std::size_t i = mid; i < state.adwin_window.size(); ++i) sum2 += state.adwin_window[i];
    const double mean1 = sum1 / static_cast<double>(mid);
    const double mean2 = sum2 / static_cast<double>(state.adwin_window.size() - mid);
    const double eps_cut = std::sqrt(0.5 * (1.0 / static_cast<double>(mid) +
                            1.0 / static_cast<double>(state.adwin_window.size() - mid)) *
                            std::log(2.0 / config_.adwin_delta));
    return std::abs(mean1 - mean2) >= eps_cut;
}

// ============================================================
// Классификация severity
// ============================================================

DriftSeverity DriftMonitor::classify_severity(double psi, double ks_p,
                                                bool ph_alarm, bool adwin_change) const {
    int critical_votes = 0;
    int warning_votes = 0;

    // PSI
    if (psi >= config_.psi_critical) critical_votes++;
    else if (psi >= config_.psi_warn) warning_votes++;

    // KS
    if (ks_p <= config_.ks_critical) critical_votes++;
    else if (ks_p <= config_.ks_warn) warning_votes++;

    // Page-Hinkley
    if (ph_alarm) critical_votes++;

    // ADWIN
    if (adwin_change) warning_votes++;

    // Majority voting
    if (critical_votes >= 2) return DriftSeverity::Critical;
    if (critical_votes >= 1 || warning_votes >= 2) return DriftSeverity::Warning;
    return DriftSeverity::None;
}

} // namespace tb::drift
