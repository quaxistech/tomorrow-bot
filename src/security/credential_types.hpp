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

#include <string>

namespace tb::security {

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
