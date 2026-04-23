#pragma once
/**
 * @file drift_monitor.hpp
 * @brief Движок мониторинга дрифта — PSI, KS, Page-Hinkley, ADWIN
 *
 * Отслеживает распределение фич, лейблов, calibration и execution quality.
 * Потокобезопасен. Все публичные методы защищены мьютексом.
 */
#include "drift/drift_types.hpp"
#include "common/result.hpp"

#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace tb::drift {

class DriftMonitor {
public:
    explicit DriftMonitor(DriftConfig config = {});

    // ======================== Регистрация потоков ========================

    /// Зарегистрировать поток для мониторинга
    void register_stream(const std::string& name, DriftType type);

    // ======================== Подача данных ========================

    /// Добавить наблюдение в поток
    void push(const std::string& stream_name, double value);

    /// Пакетная подача
    void push_batch(const std::string& stream_name, const std::vector<double>& values);

    // ======================== Детекция ========================

    /// Проверить дрифт для одного потока
    [[nodiscard]] DriftDetectionResult check(const std::string& stream_name) const;

    /// Проверить все потоки и вернуть snapshot
    [[nodiscard]] DriftSnapshot check_all() const;

    // ======================== Управление ========================

    /// Сбросить reference для потока (переобучение)
    void reset_reference(const std::string& stream_name);

    /// Получить зарегистрированные потоки
    [[nodiscard]] std::vector<std::string> stream_names() const;

    /// Количество потоков
    [[nodiscard]] std::size_t stream_count() const;

private:
    DriftConfig config_;
    mutable std::mutex mutex_;

    struct StreamState {
        std::string name;
        DriftType type{DriftType::Feature};
        std::deque<double> reference;
        std::deque<double> test;

        // Page-Hinkley state
        double ph_sum{0.0};
        double ph_min{0.0};
        double ph_mean{0.0};
        std::size_t ph_count{0};

        // ADWIN state (simplified bucket list)
        std::deque<double> adwin_window;
        double adwin_total{0.0};
        std::size_t adwin_count{0};
    };

    std::unordered_map<std::string, StreamState> streams_;

    // ======================== Статистические вычисления ========================

    [[nodiscard]] double compute_psi(const std::deque<double>& reference,
                                     const std::deque<double>& test) const;

    [[nodiscard]] std::pair<double, double> compute_ks(
        const std::deque<double>& reference,
        const std::deque<double>& test) const;

    void update_page_hinkley(StreamState& state, double value) const;

    [[nodiscard]] bool check_adwin(StreamState& state) const;

    [[nodiscard]] DriftSeverity classify_severity(double psi, double ks_p,
                                                   bool ph_alarm, bool adwin_change) const;
};

} // namespace tb::drift
