#pragma once
#include <string>
#include <string_view>

namespace tb::exchange::bitget {

// Заголовки аутентификации для Bitget API v2
struct SignedHeaders {
    std::string access_key;
    std::string timestamp;
    std::string signature;
    std::string passphrase;
};

// Вычисляет HMAC-SHA256 и возвращает base64-строку
std::string hmac_sha256_base64(std::string_view key, std::string_view message);

// Формирует заголовки для аутентификации Bitget API v2
// secret передаётся явно — нигде не сохраняется
SignedHeaders make_auth_headers(
    std::string_view api_key,
    std::string_view api_secret,
    std::string_view passphrase,
    std::string_view method,
    std::string_view path,
    std::string_view body = "",
    int64_t clock_offset_ms = 0
);

} // namespace tb::exchange::bitget
