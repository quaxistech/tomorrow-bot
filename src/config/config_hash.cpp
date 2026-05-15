/**
 * @file config_hash.cpp
 * @brief Реализация вычисления SHA-256 хеша конфигурации через OpenSSL
 */
#include "config_hash.hpp"
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <sstream>
#include <iomanip>
#include <stdexcept>
#include <array>
#include <memory>

namespace tb::config {

[[nodiscard]] std::string compute_config_hash(std::string_view raw_yaml) {
    // Создаём контекст EVP для SHA-256
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx{
        EVP_MD_CTX_new(),
        EVP_MD_CTX_free
    };

    if (!ctx) {
        throw std::runtime_error("Не удалось создать контекст EVP для SHA-256");
    }

    if (EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr) != 1) {
        throw std::runtime_error("Ошибка инициализации SHA-256");
    }

    if (EVP_DigestUpdate(ctx.get(), raw_yaml.data(), raw_yaml.size()) != 1) {
        throw std::runtime_error("Ошибка обновления SHA-256 дайджеста");
    }

    // Получение результата
    std::array<unsigned char, EVP_MAX_MD_SIZE> hash{};
    unsigned int hash_len = 0;

    if (EVP_DigestFinal_ex(ctx.get(), hash.data(), &hash_len) != 1) {
        throw std::runtime_error("Ошибка финализации SHA-256");
    }

    // Конвертируем в hex-строку
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (unsigned int i = 0; i < hash_len; ++i) {
        oss << std::setw(2) << static_cast<unsigned>(hash[i]);
    }

    return oss.str();
}

} // namespace tb::config
