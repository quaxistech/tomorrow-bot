/**
 * @file redaction.cpp
 * @brief Реализация маскирования секретных данных
 */
#include "redaction.hpp"
#include <regex>
#include <array>

namespace tb::security {

std::string redact_secrets(std::string input) {
    // ── 1. JSON-поля: "field": "value" ──────────────────────────
    static const std::array<std::regex, 10> json_patterns{
        std::regex{R"("api_key"\s*:\s*"[^"]*")",       std::regex::icase},
        std::regex{R"("api_secret"\s*:\s*"[^"]*")",    std::regex::icase},
        std::regex{R"("apiKey"\s*:\s*"[^"]*")"},
        std::regex{R"("apiSecret"\s*:\s*"[^"]*")"},
        std::regex{R"("password"\s*:\s*"[^"]*")",      std::regex::icase},
        std::regex{R"("passphrase"\s*:\s*"[^"]*")",    std::regex::icase},
        std::regex{R"("token"\s*:\s*"[^"]*")",         std::regex::icase},
        std::regex{R"("access_token"\s*:\s*"[^"]*")",  std::regex::icase},
        std::regex{R"("secret"\s*:\s*"[^"]*")",        std::regex::icase},
        std::regex{R"("authorization"\s*:\s*"[^"]*")", std::regex::icase},
    };

    static const std::array<std::string, 10> json_replacements{
        R"("api_key": "[REDACTED]")",
        R"("api_secret": "[REDACTED]")",
        R"("apiKey": "[REDACTED]")",
        R"("apiSecret": "[REDACTED]")",
        R"("password": "[REDACTED]")",
        R"("passphrase": "[REDACTED]")",
        R"("token": "[REDACTED]")",
        R"("access_token": "[REDACTED]")",
        R"("secret": "[REDACTED]")",
        R"("authorization": "[REDACTED]")",
    };

    for (std::size_t i = 0; i < json_patterns.size(); ++i) {
        input = std::regex_replace(input, json_patterns[i], json_replacements[i]);
    }

    // ── 2. Env-style: KEY=VALUE (для логов с env-переменными) ──
    // Ловит: BITGET_API_KEY=abc123, BITGET_API_SECRET=xyz, BITGET_PASSPHRASE=pass
    static const std::regex env_pattern{
        R"((BITGET_API_KEY|BITGET_API_SECRET|BITGET_PASSPHRASE|API_KEY|API_SECRET|SECRET_KEY)\s*=\s*\S+)",
        std::regex::icase
    };
    input = std::regex_replace(input, env_pattern, "$1=[REDACTED]");

    // ── 3. Bearer/Token-like строки в заголовках ──
    // "Authorization: Bearer <token>" или "Bearer <token>"
    static const std::regex bearer_pattern{
        R"((Authorization:\s*Bearer\s+|Bearer\s+)\S+)",
        std::regex::icase
    };
    input = std::regex_replace(input, bearer_pattern, "$1[REDACTED]");

    // ── 4. token=..., access_token=... в query strings / payloads ──
    static const std::regex token_param_pattern{
        R"((\b(?:access_token|token|api_key|apiKey|apiSecret)\s*=)\S+)",
        std::regex::icase
    };
    input = std::regex_replace(input, token_param_pattern, "$1[REDACTED]");

    return input;
}

} // namespace tb::security
