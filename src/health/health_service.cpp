/**
 * @file health_service.cpp
 * @brief Реализация сервиса мониторинга здоровья подсистем
 */
#include "health_service.hpp"
#include "clock/timestamp_utils.hpp"
#include <sstream>
#include <mutex>

namespace tb::health {

namespace {

/// Экранирование строк для JSON
std::string json_escape(std::string_view s) {
    std::string result;
    for (char c : s) {
        switch (c) {
            case '"':  result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\n': result += "\\n";  break;
            default:   result += c;      break;
        }
    }
    return result;
}

} // anonymous namespace

void HealthService::register_subsystem(std::string name) {
    std::lock_guard lock{mutex_};
    if (subsystems_.find(name) == subsystems_.end()) {
        subsystems_.emplace(name, SubsystemHealth{
            .name         = name,
            .state        = SubsystemState::Unknown,
            .message      = "Инициализация",
            .last_updated = Timestamp{clock::steady_now_ns()}
        });
    }
}

void HealthService::update_subsystem(std::string name,
                                      SubsystemState state,
                                      std::string message) {
    std::lock_guard lock{mutex_};
    auto it = subsystems_.find(name);
    if (it == subsystems_.end()) {
        // Если подсистема не зарегистрирована — регистрируем автоматически
        subsystems_.emplace(name, SubsystemHealth{
            .name         = name,
            .state        = state,
            .message      = std::move(message),
            .last_updated = Timestamp{clock::steady_now_ns()}
        });
    } else {
        it->second.state        = state;
        it->second.message      = std::move(message);
        it->second.last_updated = Timestamp{clock::steady_now_ns()};
    }
}

ReadinessState HealthService::overall_readiness() const {
    std::lock_guard lock{mutex_};

    if (subsystems_.empty()) {
        return ReadinessState::NotReady;
    }

    bool any_degraded = false;

    for (const auto& [_, h] : subsystems_) {
        switch (h.state) {
            case SubsystemState::Failed:
            case SubsystemState::Unknown:
            case SubsystemState::Starting:
                return ReadinessState::NotReady;  // Немедленно NotReady
            case SubsystemState::Degraded:
                any_degraded = true;
                break;
            case SubsystemState::Healthy:
                break;
        }
    }

    return any_degraded ? ReadinessState::Degraded : ReadinessState::Ready;
}

std::vector<SubsystemHealth> HealthService::get_all() const {
    std::lock_guard lock{mutex_};
    std::vector<SubsystemHealth> result;
    result.reserve(subsystems_.size());
    for (const auto& [_, h] : subsystems_) {
        result.push_back(h);
    }
    return result;
}

std::string HealthService::export_json() const {
    std::lock_guard lock{mutex_};
    std::ostringstream oss;

    // Вычисляем общее состояние (без блокировки — mutex уже захвачен)
    bool any_failed   = false;
    bool any_starting = false;
    bool any_degraded = false;

    for (const auto& [_, h] : subsystems_) {
        if (h.state == SubsystemState::Failed)  any_failed   = true;
        if (h.state == SubsystemState::Starting ||
            h.state == SubsystemState::Unknown) any_starting = true;
        if (h.state == SubsystemState::Degraded) any_degraded = true;
    }

    ReadinessState overall = ReadinessState::Ready;
    if (any_failed || any_starting)   overall = ReadinessState::NotReady;
    else if (any_degraded)            overall = ReadinessState::Degraded;

    oss << "{";
    oss << "\"status\":\"" << to_string(overall) << "\"";
    oss << ",\"subsystems\":[";

    bool first = true;
    for (const auto& [_, h] : subsystems_) {
        if (!first) oss << ",";
        oss << "{";
        oss << "\"name\":\"" << json_escape(h.name) << "\"";
        oss << ",\"state\":\"" << to_string(h.state) << "\"";
        oss << ",\"message\":\"" << json_escape(h.message) << "\"";
        oss << "}";
        first = false;
    }

    oss << "]}";
    return oss.str();
}

std::shared_ptr<IHealthService> create_health_service() {
    return std::make_shared<HealthService>();
}

} // namespace tb::health
