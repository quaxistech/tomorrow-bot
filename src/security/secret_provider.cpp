/**
 * @file secret_provider.cpp
 * @brief Реализация провайдеров секретов
 */
#include "secret_provider.hpp"
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <algorithm>
#include <cctype>

namespace tb::security {

// ============================================================
// EnvSecretProvider
// ============================================================

Result<std::string> EnvSecretProvider::get_secret(const SecretRef& ref) const {
    const char* value = std::getenv(ref.key_name.c_str());
    if (value == nullptr || value[0] == '\0') {
        return Err<std::string>(TbError::SecretNotFound);
    }
    return Ok(std::string(value));
}

// ============================================================
// FileSecretProvider
// ============================================================

FileSecretProvider::FileSecretProvider(std::string file_path) {
    load_secrets(file_path);
}

void FileSecretProvider::load_secrets(const std::string& path) {
    std::ifstream file{path};
    if (!file.is_open()) {
        throw std::runtime_error(
            "Не удалось открыть файл секретов: " + path
        );
    }

    std::string line;
    while (std::getline(file, line)) {
        // Пропускаем комментарии и пустые строки
        if (line.empty() || line[0] == '#') continue;

        auto eq_pos = line.find('=');
        if (eq_pos == std::string::npos) continue;

        std::string key   = line.substr(0, eq_pos);
        std::string value = line.substr(eq_pos + 1);

        // Обрезаем пробелы
        auto trim = [](std::string s) {
            s.erase(s.begin(), std::find_if(s.begin(), s.end(),
                [](unsigned char c){ return !std::isspace(c); }));
            s.erase(std::find_if(s.rbegin(), s.rend(),
                [](unsigned char c){ return !std::isspace(c); }).base(), s.end());
            return s;
        };

        key   = trim(key);
        value = trim(value);

        // Удаляем кавычки если есть
        if (value.size() >= 2 &&
            ((value.front() == '"' && value.back() == '"') ||
             (value.front() == '\'' && value.back() == '\''))) {
            value = value.substr(1, value.size() - 2);
        }

        if (!key.empty()) {
            secrets_[key] = std::move(value);
        }
    }
}

Result<std::string> FileSecretProvider::get_secret(const SecretRef& ref) const {
    auto it = secrets_.find(ref.key_name);
    if (it == secrets_.end()) {
        return Err<std::string>(TbError::SecretNotFound);
    }
    return Ok(std::string{it->second});
}

// ============================================================
// Фабричные функции
// ============================================================

std::unique_ptr<ISecretProvider> create_env_secret_provider() {
    return std::make_unique<EnvSecretProvider>();
}

std::unique_ptr<ISecretProvider> create_file_secret_provider(std::string path) {
    return std::make_unique<FileSecretProvider>(std::move(path));
}

} // namespace tb::security
