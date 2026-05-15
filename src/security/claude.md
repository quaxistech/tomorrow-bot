# `src/security` — Безопасность

## Назначение

Управление секретами (API ключи) и production-guard (защита от случайного боевого запуска). Изоляция чувствительных данных от config/log/слепка памяти.

## Границы ответственности

* `ISecretProvider` — абстракция получения секретов.
* `EnvSecretProvider` — из переменных окружения.
* `FileSecretProvider` — из `.env` (формат `KEY=VALUE`).
* `ProductionGuard` — preflight перед `Production`-режимом.
* `redaction.hpp` — маскирование чувствительных строк в логах.

## Входы / выходы

* Вход: env vars / файл `.env` / config_hash / API base URL / mode.
* Выход: `Result<std::string>` секрета, либо `ProductionGuardResult.allowed`.

## Публичные интерфейсы

* `class ISecretProvider`:
  * `[[nodiscard]] Result<std::string> get_secret(SecretRef) const`.
* `class EnvSecretProvider`, `class FileSecretProvider`.
* Фабрики `create_env_secret_provider()`, `create_file_secret_provider(path)`.
* `class ProductionGuard`:
  * `ProductionGuardResult validate(mode, api_key, api_secret, api_passphrase, base_url, config_hash)`.
  * `static bool check_env_confirmation()`.
  * `static bool is_production_api(base_url)`.
* `redaction.hpp` — `std::string redact(string, len_keep_head, len_keep_tail)`.
* `credential_types.hpp` — `SecretRef`, `Credential`.

## Внутренние компоненты

* `secret_provider.hpp/cpp` — интерфейс, env-impl, file-impl.
* `production_guard.hpp/cpp` — guard.
* `redaction.hpp/cpp` — маскирование.
* `credential_types.hpp` — типы.

## Зависимости

* `common/result.hpp`, `common/types.hpp`.
* `logging/ILogger` (для guard).

## Потоки данных

* Bootstrap: `AppBootstrap::initialize` ищет `.env` в [`./.env`, `./configs/.env`, `/etc/tomorrow-bot/.env`], загружает, либо fallback на env vars.
* Production: `ProductionGuard::validate` запускается ПОСЛЕ загрузки секретов.
* После использования: в `app_bootstrap.cpp` локальные копии `api_key`/`api_secret`/`api_passphrase` обнуляются (`std::fill(.., '\0')`) и `clear()` — снижает risk попадания в core dump.

## Race conditions

* `FileSecretProvider` загружает в map в конструкторе; чтение из const-метода thread-safe.
* `EnvSecretProvider::get_secret` использует `std::getenv` — thread-safe в POSIX.

## Ошибки проектирования

* **D-sec-1 (HIGH).** Секреты в `FileSecretProvider::secrets_` хранятся как обычные `std::string` в heap до конца процесса — попадают в core dump. Mitigation: хранить в защищённом контейнере (`mlock`, `secure_string` с `volatile`-fill at destroy).
* **D-sec-2 (MEDIUM).** `ProductionGuard::is_production_api` сравнивает по простому substring? — необходимо проверить реализацию. Если допускается частичное совпадение, atak-surface расширен.
* **D-sec-3 (LOW).** Файл `.env` может иметь неправильные права (mode 0644). Bootstrap не проверяет permissions.
* **D-sec-4 (INFO).** redaction применяется вручную в каждом log-вызове; нет автоматического scrubbing полей по списку имен.

## Контракты

### `ISecretProvider::get_secret(SecretRef ref)`

* **Pre.** `ref.name() != ""`.
* **Post.** Успех: возвращена строка с secret. Неудача: `TbError::SecretNotFound` или `SecretProviderUnavailable`.
* **Invariant.** Возвращённое значение — копия; провайдер сохраняет владение mast-копией. Caller обязан обнулить после использования.

### `ProductionGuard::validate(...)`

* **Pre.** `mode == Production` (проверки не релевантны для других режимов в текущей версии).
* **Post.**
  * `allowed = true` ⇒ все 5 условий выполнены (API URL whitelisted, env_confirmation, complete keys, config_hash непустой, NDEBUG-сборка).
  * `allowed = false` ⇒ `reason` содержит человекочитаемую причину.
* **Invariant.** Result структура содержит достаточную диагностику для аудита.

## Производственные риски

* **R-sec-1.** Утечка секретов через core dump: реализовать `madvise(MADV_DONTDUMP)` и/или `mlock` для `secrets_` map в `FileSecretProvider`.
* **R-sec-2.** Подмена `.env` при недостаточных правах файла.
* **R-sec-3.** Логи могут случайно содержать секрет, если разработчик передаст его в context. Mitigation: компилируемый whitelist допустимых полей или автоматический scrubbing.

## Рекомендации

1. Secure storage для секретов (mlock + zero-on-destruct).
2. Проверка прав файла `.env` (≤ 0600) в `FileSecretProvider`.
3. Whitelist endpoint URLs (точное совпадение, не substring).
4. Auto-redaction в `JsonFormatter` для известных ключей (`api_key`, `secret`, `passphrase`, `Authorization`).
5. Тесты на ProductionGuard для всех сценариев отказа.
