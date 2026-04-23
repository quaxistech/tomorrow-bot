/**
 * @file test_security.cpp
 * @brief Phase 4 тесты: secret_provider, redaction, SecureString, ProductionGuard allowlist
 */
#include <catch2/catch_test_macros.hpp>
#include "test_mocks.hpp"
#include "security/production_guard.hpp"
#include "security/secret_provider.hpp"
#include "security/redaction.hpp"
#include "security/credential_types.hpp"

#include <cstdlib>
#include <fstream>
#include <filesystem>

using namespace tb;
using namespace tb::test;
using namespace tb::security;

// ============================================================
// ProductionGuard: Host Allowlist
// ============================================================

TEST_CASE("ProductionGuard: allowlist — api.bitget.com пропускает", "[security]") {
    REQUIRE(ProductionGuard::is_production_api("https://api.bitget.com"));
    REQUIRE(ProductionGuard::is_production_api("https://api.bitget.com/api/v2/mix"));
    REQUIRE(ProductionGuard::is_production_api("https://API.BITGET.COM/api/v2"));
}

TEST_CASE("ProductionGuard: allowlist — capi.bitget.com пропускает", "[security]") {
    REQUIRE(ProductionGuard::is_production_api("https://capi.bitget.com"));
    REQUIRE(ProductionGuard::is_production_api("https://capi.bitget.com/api/v2"));
}

TEST_CASE("ProductionGuard: allowlist — testnet блокируется", "[security]") {
    REQUIRE_FALSE(ProductionGuard::is_production_api("https://testnet.bitget.com"));
    REQUIRE_FALSE(ProductionGuard::is_production_api("https://testnet.bitget.com/api/v2"));
}

TEST_CASE("ProductionGuard: allowlist — localhost блокируется", "[security]") {
    REQUIRE_FALSE(ProductionGuard::is_production_api("http://localhost:8080"));
    REQUIRE_FALSE(ProductionGuard::is_production_api("http://127.0.0.1:8080"));
    REQUIRE_FALSE(ProductionGuard::is_production_api("http://localhost"));
}

TEST_CASE("ProductionGuard: allowlist — произвольные хосты блокируются", "[security]") {
    REQUIRE_FALSE(ProductionGuard::is_production_api("https://evil-proxy.com/api"));
    REQUIRE_FALSE(ProductionGuard::is_production_api("https://api.binance.com"));
    REQUIRE_FALSE(ProductionGuard::is_production_api("https://fake-bitget.com"));
    REQUIRE_FALSE(ProductionGuard::is_production_api("https://api.bitget.com.evil.com"));
}

TEST_CASE("ProductionGuard: allowlist — пустой URL блокируется", "[security]") {
    REQUIRE_FALSE(ProductionGuard::is_production_api(""));
}

// ============================================================
// ProductionGuard: Debug Build Guard
// ============================================================

TEST_CASE("ProductionGuard: debug-сборка блокирует production", "[security]") {
    auto logger = std::make_shared<TestLogger>();
    ProductionGuard guard(logger);

    ::setenv("TOMORROW_BOT_PRODUCTION_CONFIRM", "I_UNDERSTAND_LIVE_TRADING", 1);

    auto result = guard.validate(
        TradingMode::Production,
        "key", "secret", "pass",
        "https://api.bitget.com",
        "hash-123");

    // В debug-сборке (тесты всегда debug) — должен быть запрет
#ifdef NDEBUG
    // Release build — разрешено
    REQUIRE(result.allowed);
#else
    // Debug build — запрещено
    REQUIRE_FALSE(result.allowed);
    REQUIRE(result.reason.find("debug") != std::string::npos);
#endif

    ::unsetenv("TOMORROW_BOT_PRODUCTION_CONFIRM");
}

// ============================================================
// Redaction: JSON fields
// ============================================================

TEST_CASE("Redaction: JSON api_key маскируется", "[security]") {
    auto input = R"({"api_key": "secret123", "data": "ok"})";
    auto result = redact_secrets(input);
    REQUIRE(result.find("secret123") == std::string::npos);
    REQUIRE(result.find("[REDACTED]") != std::string::npos);
    REQUIRE(result.find("\"data\": \"ok\"") != std::string::npos);
}

TEST_CASE("Redaction: JSON token маскируется", "[security]") {
    auto input = R"({"token": "abc-xyz-123"})";
    auto result = redact_secrets(input);
    REQUIRE(result.find("abc-xyz-123") == std::string::npos);
    REQUIRE(result.find("[REDACTED]") != std::string::npos);
}

TEST_CASE("Redaction: JSON access_token маскируется", "[security]") {
    auto input = R"({"access_token": "eyJhbGciOiJIUzI1NiJ9"})";
    auto result = redact_secrets(input);
    REQUIRE(result.find("eyJhbGciOiJIUzI1NiJ9") == std::string::npos);
}

TEST_CASE("Redaction: JSON authorization маскируется", "[security]") {
    auto input = R"({"authorization": "Bearer xyz123"})";
    auto result = redact_secrets(input);
    REQUIRE(result.find("xyz123") == std::string::npos);
}

TEST_CASE("Redaction: JSON secret маскируется", "[security]") {
    auto input = R"({"secret": "mysecretvalue"})";
    auto result = redact_secrets(input);
    REQUIRE(result.find("mysecretvalue") == std::string::npos);
}

TEST_CASE("Redaction: JSON passphrase маскируется (case insensitive)", "[security]") {
    auto input = R"({"Passphrase": "MyPhrase123"})";
    auto result = redact_secrets(input);
    REQUIRE(result.find("MyPhrase123") == std::string::npos);
}

// ============================================================
// Redaction: env-style KEY=VALUE
// ============================================================

TEST_CASE("Redaction: env BITGET_API_KEY=value маскируется", "[security]") {
    auto result = redact_secrets("BITGET_API_KEY=abc123xyz");
    REQUIRE(result.find("abc123xyz") == std::string::npos);
    REQUIRE(result.find("BITGET_API_KEY=[REDACTED]") != std::string::npos);
}

TEST_CASE("Redaction: env BITGET_API_SECRET=value маскируется", "[security]") {
    auto result = redact_secrets("Got secret: BITGET_API_SECRET=supersecret other stuff");
    REQUIRE(result.find("supersecret") == std::string::npos);
    REQUIRE(result.find("BITGET_API_SECRET=[REDACTED]") != std::string::npos);
}

TEST_CASE("Redaction: env BITGET_PASSPHRASE=value маскируется", "[security]") {
    auto result = redact_secrets("BITGET_PASSPHRASE=pass123");
    REQUIRE(result.find("pass123") == std::string::npos);
}

// ============================================================
// Redaction: Bearer tokens
// ============================================================

TEST_CASE("Redaction: Authorization Bearer header маскируется", "[security]") {
    auto result = redact_secrets("Authorization: Bearer eyJhbGciOiJIUzI1NiJ9.payload.sig");
    REQUIRE(result.find("eyJhbGciOiJIUzI1NiJ9") == std::string::npos);
    REQUIRE(result.find("[REDACTED]") != std::string::npos);
}

TEST_CASE("Redaction: Bearer token без Authorization: маскируется", "[security]") {
    auto result = redact_secrets("Token is Bearer abcdef123456 ok");
    REQUIRE(result.find("abcdef123456") == std::string::npos);
}

// ============================================================
// Redaction: query string parameters
// ============================================================

TEST_CASE("Redaction: access_token=value в query string маскируется", "[security]") {
    auto result = redact_secrets("url?access_token=secret123&other=ok");
    REQUIRE(result.find("secret123") == std::string::npos);
    REQUIRE(result.find("[REDACTED]") != std::string::npos);
}

TEST_CASE("Redaction: api_key=value в query string маскируется", "[security]") {
    auto result = redact_secrets("api_key=mysecretkey&foo=bar");
    REQUIRE(result.find("mysecretkey") == std::string::npos);
}

// ============================================================
// Redaction: безопасные данные не затрагиваются
// ============================================================

TEST_CASE("Redaction: обычный текст не затрагивается", "[security]") {
    std::string input = "BTC price is 50000 USDT, volume=1234567";
    auto result = redact_secrets(input);
    REQUIRE(result == input);
}

TEST_CASE("Redaction: пустая строка", "[security]") {
    REQUIRE(redact_secrets("").empty());
}

// ============================================================
// SecretProvider: EnvSecretProvider
// ============================================================

TEST_CASE("EnvSecretProvider: читает существующую переменную", "[security]") {
    ::setenv("TEST_SECRET_KEY_12345", "test_value_abc", 1);
    EnvSecretProvider provider;
    auto result = provider.get_secret(SecretRef{"TEST_SECRET_KEY_12345"});
    REQUIRE(result.has_value());
    REQUIRE(*result == "test_value_abc");
    ::unsetenv("TEST_SECRET_KEY_12345");
}

TEST_CASE("EnvSecretProvider: отсутствующая переменная → ошибка", "[security]") {
    ::unsetenv("NONEXISTENT_SECRET_KEY_XYZ");
    EnvSecretProvider provider;
    auto result = provider.get_secret(SecretRef{"NONEXISTENT_SECRET_KEY_XYZ"});
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("EnvSecretProvider: пустая переменная → ошибка", "[security]") {
    ::setenv("EMPTY_SECRET_KEY_TEST", "", 1);
    EnvSecretProvider provider;
    auto result = provider.get_secret(SecretRef{"EMPTY_SECRET_KEY_TEST"});
    REQUIRE_FALSE(result.has_value());
    ::unsetenv("EMPTY_SECRET_KEY_TEST");
}

// ============================================================
// SecretProvider: FileSecretProvider
// ============================================================

TEST_CASE("FileSecretProvider: читает .env файл", "[security]") {
    // Создаём временный .env файл
    auto tmp = std::filesystem::temp_directory_path() / "test_secrets.env";
    {
        std::ofstream f(tmp);
        f << "# Comment line\n"
          << "TEST_KEY=hello_world\n"
          << "TEST_SECRET=\"quoted value\"\n"
          << "  SPACED_KEY  =  spaced_value  \n"
          << "\n"
          << "SINGLE_QUOTED='value123'\n";
    }

    FileSecretProvider provider(tmp.string());

    auto r1 = provider.get_secret(SecretRef{"TEST_KEY"});
    REQUIRE(r1.has_value());
    REQUIRE(*r1 == "hello_world");

    auto r2 = provider.get_secret(SecretRef{"TEST_SECRET"});
    REQUIRE(r2.has_value());
    REQUIRE(*r2 == "quoted value");

    auto r3 = provider.get_secret(SecretRef{"SPACED_KEY"});
    REQUIRE(r3.has_value());
    REQUIRE(*r3 == "spaced_value");

    auto r4 = provider.get_secret(SecretRef{"SINGLE_QUOTED"});
    REQUIRE(r4.has_value());
    REQUIRE(*r4 == "value123");

    std::filesystem::remove(tmp);
}

TEST_CASE("FileSecretProvider: несуществующий ключ → ошибка", "[security]") {
    auto tmp = std::filesystem::temp_directory_path() / "test_secrets2.env";
    {
        std::ofstream f(tmp);
        f << "EXISTING_KEY=value\n";
    }

    FileSecretProvider provider(tmp.string());
    auto result = provider.get_secret(SecretRef{"MISSING_KEY"});
    REQUIRE_FALSE(result.has_value());

    std::filesystem::remove(tmp);
}

TEST_CASE("FileSecretProvider: несуществующий файл → исключение", "[security]") {
    REQUIRE_THROWS_AS(
        FileSecretProvider("/nonexistent/path/to/secrets.env"),
        std::runtime_error);
}

// ============================================================
// SecureString
// ============================================================

TEST_CASE("SecureString: хранит и возвращает значение", "[security]") {
    SecureString s("my_secret_value");
    REQUIRE(s.reveal() == "my_secret_value");
    REQUIRE(s.size() == 15);
    REQUIRE_FALSE(s.empty());
}

TEST_CASE("SecureString: пустая строка", "[security]") {
    SecureString s;
    REQUIRE(s.empty());
    REQUIRE(s.size() == 0);
}

TEST_CASE("SecureString: move зануляет исходный объект", "[security]") {
    SecureString s1("secret");
    SecureString s2(std::move(s1));
    REQUIRE(s2.reveal() == "secret");
    REQUIRE(s1.empty());
}

TEST_CASE("SecureString: move assignment зануляет исходный объект", "[security]") {
    SecureString s1("value1");
    SecureString s2("value2");
    s2 = std::move(s1);
    REQUIRE(s2.reveal() == "value1");
    REQUIRE(s1.empty());
}

TEST_CASE("SecureString: copy создаёт копию", "[security]") {
    SecureString s1("copy_me");
    SecureString s2(s1);
    REQUIRE(s2.reveal() == "copy_me");
    REQUIRE(s1.reveal() == "copy_me");
}
