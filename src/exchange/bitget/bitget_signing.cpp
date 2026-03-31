#include "bitget_signing.hpp"
#include <openssl/evp.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <chrono>
#include <sstream>
#include <stdexcept>
#include <memory>
#include <vector>

namespace tb::exchange::bitget {

std::string hmac_sha256_base64(std::string_view key, std::string_view message) {
    // Вычисляем HMAC-SHA256 через EVP_MAC (OpenSSL 3.0+)
    std::vector<unsigned char> digest(EVP_MAX_MD_SIZE);
    size_t digest_len = 0;

    std::unique_ptr<EVP_MAC, decltype(&EVP_MAC_free)> mac(
        EVP_MAC_fetch(nullptr, "HMAC", nullptr), EVP_MAC_free);
    if (!mac) {
        throw std::runtime_error("EVP_MAC_fetch(HMAC) не удалось");
    }

    std::unique_ptr<EVP_MAC_CTX, decltype(&EVP_MAC_CTX_free)> ctx(
        EVP_MAC_CTX_new(mac.get()), EVP_MAC_CTX_free);
    if (!ctx) {
        throw std::runtime_error("EVP_MAC_CTX_new не удалось");
    }

    // Параметры: алгоритм дайджеста = SHA256
    OSSL_PARAM params[2];
    params[0] = OSSL_PARAM_construct_utf8_string(
        "digest", const_cast<char*>("SHA256"), 0);
    params[1] = OSSL_PARAM_construct_end();

    if (!EVP_MAC_init(ctx.get(),
                      reinterpret_cast<const unsigned char*>(key.data()),
                      key.size(), params)) {
        throw std::runtime_error("EVP_MAC_init не удалось");
    }

    if (!EVP_MAC_update(ctx.get(),
                        reinterpret_cast<const unsigned char*>(message.data()),
                        message.size())) {
        throw std::runtime_error("EVP_MAC_update не удалось");
    }

    if (!EVP_MAC_final(ctx.get(), digest.data(), &digest_len, digest.size())) {
        throw std::runtime_error("EVP_MAC_final не удалось");
    }

    // Кодируем в base64 без символов переноса строки
    std::unique_ptr<BIO, decltype(&BIO_free_all)> b64(BIO_new(BIO_f_base64()), BIO_free_all);
    BIO* mem_bio = BIO_new(BIO_s_mem());
    // Цепочка: b64 -> mem
    BIO_push(b64.get(), mem_bio);
    // Без переносов строк
    BIO_set_flags(b64.get(), BIO_FLAGS_BASE64_NO_NL);
    BIO_write(b64.get(), digest.data(), static_cast<int>(digest_len));
    if (BIO_flush(b64.get()) != 1) {
        throw std::runtime_error("BIO_flush не удалось");
    }

    BUF_MEM* buf_ptr = nullptr;
    BIO_get_mem_ptr(mem_bio, &buf_ptr);

    return std::string(buf_ptr->data, buf_ptr->length);
}

SignedHeaders make_auth_headers(
    std::string_view api_key,
    std::string_view api_secret,
    std::string_view passphrase,
    std::string_view method,
    std::string_view path,
    std::string_view body
) {
    // Временная метка в миллисекундах (Bitget v2 API требует мс)
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()
    ).count();

    std::string ts = std::to_string(ms);

    // Строка для подписи: timestamp + METHOD + path + body
    std::string pre_sign;
    pre_sign.reserve(ts.size() + method.size() + path.size() + body.size());
    pre_sign += ts;
    pre_sign += method;
    pre_sign += path;
    pre_sign += body;

    std::string sig = hmac_sha256_base64(api_secret, pre_sign);

    return SignedHeaders{
        .access_key  = std::string(api_key),
        .timestamp   = std::move(ts),
        .signature   = std::move(sig),
        .passphrase  = std::string(passphrase),
    };
}

} // namespace tb::exchange::bitget
