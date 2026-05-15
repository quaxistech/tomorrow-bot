/**
 * @file credential_types.hpp
 * @brief Типы для работы с учётными данными API
 * 
 * ВАЖНО: ApiCredential содержит секретные данные в памяти.
 * Использовать только в защищённом контексте, не логировать,
 * не сериализовать в файл.
 * 
 * SecretRef — это только ССЫЛКА (имя переменной), не сам секрет.
 */
#pragma once

#include <cstring>
#include <string>

namespace tb::security {

/**
 * @brief Строка, безопасно зануляющая память при уничтожении.
 * 
 * Предотвращает утечку секретов через не-зануленную память после free.
 * Используйте для API ключей, секретов, passphrase.
 */
class SecureString {
public:
    SecureString() = default;
    explicit SecureString(std::string s) : data_(std::move(s)) {}
    SecureString(const char* s) : data_(s) {}

    ~SecureString() { secure_zero(); }

    // Move — исходный объект зануляется
    SecureString(SecureString&& o) noexcept : data_(std::move(o.data_)) {
        o.secure_zero();
    }
    SecureString& operator=(SecureString&& o) noexcept {
        if (this != &o) {
            secure_zero();
            data_ = std::move(o.data_);
            o.secure_zero();
        }
        return *this;
    }

    // Copy — допустимо, но нежелательно (создаёт ещё одну копию секрета)
    SecureString(const SecureString& o) : data_(o.data_) {}
    SecureString& operator=(const SecureString& o) {
        if (this != &o) {
            secure_zero();
            data_ = o.data_;
        }
        return *this;
    }

    [[nodiscard]] const std::string& reveal() const noexcept { return data_; }
    [[nodiscard]] const char* c_str() const noexcept { return data_.c_str(); }
    [[nodiscard]] bool empty() const noexcept { return data_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return data_.size(); }

private:
    void secure_zero() {
        if (!data_.empty()) {
            // volatile memset — не будет оптимизирован компилятором
            volatile char* p = data_.data();
            std::size_t n = data_.size();
            while (n--) *p++ = 0;
            data_.clear();
        }
    }
    std::string data_;
};

/**
 * @brief Учётные данные API биржи
 * 
 * Хранит фактические секреты в памяти — минимальное время жизни.
 * Нет конструктора по умолчанию — обязательно явное задание полей.
 */
struct ApiCredential {
    std::string key;        ///< API ключ (публичная часть)
    std::string secret;     ///< API секрет (приватная часть)
    std::string passphrase; ///< Кодовая фраза (требуется некоторыми биржами)

    /// Явный конструктор — предотвращает случайное создание пустых секретов
    explicit ApiCredential(std::string k, std::string s, std::string p = "")
        : key(std::move(k))
        , secret(std::move(s))
        , passphrase(std::move(p))
    {}

    // Запрет конструктора по умолчанию — нельзя создать пустые учётные данные
    ApiCredential() = delete;
};

/**
 * @brief Ссылка на секрет в хранилище (НЕ сам секрет!)
 * 
 * Используется в конфигурации для указания имени переменной окружения
 * или ключа в secrets-файле. Безопасно хранить где угодно.
 * 
 * Пример: SecretRef{"BITGET_API_KEY"} — ссылается на переменную окружения
 */
struct SecretRef {
    std::string key_name; ///< Имя ключа в хранилище секретов

    explicit SecretRef(std::string name) : key_name(std::move(name)) {}
    SecretRef() = delete;
};

} // namespace tb::security
