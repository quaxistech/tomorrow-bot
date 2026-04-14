/**
 * @file json_formatter.cpp
 * @brief Реализация JSON форматтера событий лога
 */
#include "json_formatter.hpp"
#include <sstream>
#include <iomanip>
#include <chrono>
#include <ctime>

namespace tb::logging {

std::string json_escape(std::string_view s) {
    std::string result;
    result.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':  result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\n': result += "\\n";  break;
            case '\r': result += "\\r";  break;
            case '\t': result += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    // Управляющие символы
                    std::ostringstream oss;
                    oss << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                        << static_cast<int>(static_cast<unsigned char>(c));
                    result += oss.str();
                } else {
                    result += c;
                }
        }
    }
    return result;
}

std::string timestamp_to_iso8601(Timestamp ts) {
    // Конвертируем наносекунды в time_t и миллисекунды
    int64_t ns  = ts.get();
    int64_t sec = ns / 1'000'000'000LL;
    int64_t ms  = (ns % 1'000'000'000LL) / 1'000'000LL;

    std::time_t t = static_cast<std::time_t>(sec);
    std::tm  tm_utc{};

#if defined(_WIN32)
    gmtime_s(&tm_utc, &t);
#else
    gmtime_r(&t, &tm_utc);
#endif

    std::ostringstream oss;
    oss << std::put_time(&tm_utc, "%Y-%m-%dT%H:%M:%S");
    oss << "." << std::setw(3) << std::setfill('0') << ms << "Z";
    return oss.str();
}

std::string format_as_json(const LogEvent& event) {
    std::ostringstream oss;
    oss << "{";
    oss << "\"timestamp\":\"" << timestamp_to_iso8601(event.timestamp) << "\"";
    oss << ",\"level\":\"" << json_escape(to_string(event.level)) << "\"";
    oss << ",\"component\":\"" << json_escape(event.component) << "\"";
    oss << ",\"message\":\"" << json_escape(event.message) << "\"";

    if (!event.correlation_id.empty()) {
        oss << ",\"correlation_id\":\"" << json_escape(event.correlation_id) << "\"";
    }

    if (!event.fields.empty()) {
        oss << ",\"fields\":{";
        bool first = true;
        for (const auto& [k, v] : event.fields) {
            if (k == "correlation_id") continue;
            if (!first) oss << ",";
            oss << "\"" << json_escape(k) << "\":\"" << json_escape(v) << "\"";
            first = false;
        }
        oss << "}";
    }

    oss << "}";
    return oss.str();
}

} // namespace tb::logging
