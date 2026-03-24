#include "bitget_signing.hpp"
#include <openssl/hmac.h>
#include <openssl/evp.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <chrono>
#include <sstream>
#include <stdexcept>
#include <memory>

namespace tb::exchange::bitget {

std::string hmac_sha256_base64(std::string_view key, std::string_view message) {
    // Вычисляем HMAC-SHA256
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;

    const unsigned char* k = reinterpret_cast<const unsigned char*>(key.data());
    const unsigned char* m = reinterpret_cast<const unsigned char*>(message.data());

    unsigned char* result = HMAC(
        EVP_sha256(),
        k, static_cast<int>(key.size()),
        m, static_cast<int>(message.size()),
        digest, &digest_len
    );

    if (!result) {
        throw std::runtime_error("HMAC вычисление не удалось");
    }

    // Кодируем в base64 без символов переноса строки
    std::unique_ptr<BIO, decltype(&BIO_free_all)> b64(BIO_new(BIO_f_base64()), BIO_free_all);
    BIO* mem_bio = BIO_new(BIO_s_mem());
    // Цепочка: b64 -> mem
    BIO_push(b64.get(), mem_bio);
    // Без переносов строк
    BIO_set_flags(b64.get(), BIO_FLAGS_BASE64_NO_NL);
    BIO_write(b64.get(), digest, static_cast<int>(digest_len));
    BIO_flush(b64.get());

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
