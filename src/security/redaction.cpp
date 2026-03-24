/**
 * @file redaction.cpp
 * @brief Реализация маскирования секретных данных
 */
#include "redaction.hpp"
#include <regex>
#include <array>

namespace tb::security {

std::string redact_secrets(std::string input) {
    // Паттерны полей требующих маскировки
    // Формат JSON: "field_name": "value"
    static const std::array<std::regex, 6> patterns{
        std::regex{R"("api_key"\s*:\s*"[^"]*")",    std::regex::icase},
        std::regex{R"("api_secret"\s*:\s*"[^"]*")", std::regex::icase},
        std::regex{R"("apiKey"\s*:\s*"[^"]*")"},
        std::regex{R"("apiSecret"\s*:\s*"[^"]*")"},
        std::regex{R"("password"\s*:\s*"[^"]*")",   std::regex::icase},
        std::regex{R"("passphrase"\s*:\s*"[^"]*")", std::regex::icase},
    };

    static const std::array<std::string, 6> replacements{
        R"("api_key": "[REDACTED]")",
        R"("api_secret": "[REDACTED]")",
        R"("apiKey": "[REDACTED]")",
        R"("apiSecret": "[REDACTED]")",
        R"("password": "[REDACTED]")",
        R"("passphrase": "[REDACTED]")",
    };

    for (std::size_t i = 0; i < patterns.size(); ++i) {
        input = std::regex_replace(input, patterns[i], replacements[i]);
    }

    return input;
}

} // namespace tb::security
