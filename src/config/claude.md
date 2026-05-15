# `src/config` — Конфигурация

## Назначение

Загрузка YAML-конфигурации, валидация, расчёт SHA-256 хэша для аудита. Единственный путь поступления параметров в систему (никаких хардкодов в business-логике).

## Границы ответственности

* `IConfigLoader::load(path) → Result<AppConfig>`.
* Самописный «плоский» YAML-парсер (`section.key` → string).
* `ConfigValidator` — проверка интервалов и обязательных полей (TBD: проверить `config_validator.cpp`).
* `ConfigHash` — SHA-256 от исходного содержимого файла.
* `AppConfig` — комплексная структура: trading, exchange, futures, pair_selection, risk, leverage, regime, uncertainty, strategy, decision, ml, execution, persistence, reconciliation, logging, metrics.

## Входы / выходы

* Вход: путь к YAML-файлу.
* Выход: `Result<AppConfig>` или `TbError::ConfigLoadFailed/ConfigValidationFailed`.

## Публичные интерфейсы

* `class IConfigLoader` — abstract.
* `class YamlConfigLoader : IConfigLoader`.
* `std::unique_ptr<IConfigLoader> create_config_loader()` — фабрика.
* `class ConfigValidator` — методы валидации диапазонов.
* `ConfigHash compute(const std::string& content)`.

## Внутренние компоненты

* `config_loader.hpp/cpp` — загрузчик.
* `config_types.hpp` — все вложенные конфиги (десятки структур).
* `config_validator.hpp/cpp` — валидация диапазонов.
* `config_hash.hpp/cpp` — SHA-256.

## Зависимости

* `common/result.hpp`, `common/types.hpp`.
* OpenSSL (для SHA-256).
* STL filesystem.

## Потоки данных

`load(path)` → читает файл → `parse_yaml_flat` → `unordered_map<string,string>` (плоский) → `get_value(map, "section.key", default)` для каждого поля → `AppConfig`.

`compute_hash` → SHA-256 от содержимого файла → hex-string. Сохраняется в `AppConfig::config_hash` и логируется в runtime manifest.

## Race conditions

Конфиг загружается один раз при старте. После — read-only. Hot-reload **не поддерживается** (см. `LeverageEngine::update_config` — единственный пример partial hot-reload).

## Ошибки проектирования

* **D-cfg-1 (HIGH).** Самописный flat-парсер не понимает YAML-списки и nested mapping глубже 2 уровней. Все списки (`manual_symbols`, `blacklist`, `intervals`, `top_pairs`) обрабатываются хаками: либо стрингификация в одну строку через запятую, либо специальная многострочная логика. Это хрупко: `[a, b, c]` vs `- a\n- b` не парсятся одинаково.
* **D-cfg-2 (MEDIUM).** Дефолты разбросаны: часть в `config_types.hpp` (структуры), часть в `config_loader.cpp` (`get_value(default)`), часть в `app_bootstrap.cpp`/`main.cpp` (override). Сейчас уже есть упоминание Session 4 fix про `min_rr` где hardcoded "1.5" в config_loader перебивал config_types 1.0.
* **D-cfg-3 (MEDIUM).** Валидация неполная: проверяются интервалы части полей, но cross-field constraints (например, `min_volume_usdt < min_volume_for_dynamic_top`) — не проверяются.
* **D-cfg-4 (LOW).** SHA-256 считается от файла, не от итогового `AppConfig` после применения дефолтов. Изменение дефолтов в коде не меняет hash.

## Контракты

### `IConfigLoader::load(path)`

* **Pre.** `path` существует и читается.
* **Post.**
  * Успех: возвращён `AppConfig` с заполнённым `config_hash`.
  * Неудача: `Err(TbError::ConfigLoadFailed)` или `ConfigValidationFailed`.
* **Invariant.** `config_hash` непустой при успехе.

### `ConfigValidator::validate(AppConfig)`

* **Pre.** Никаких.
* **Post.**
  * Успех: все диапазоны корректны.
  * Неудача: список ошибок (через выходной параметр или `Result`).

## Производственные риски

* **R-cfg-1.** Молчаливое игнорирование непонятных секций → оператор думает, что параметр применён, а на самом деле — нет. Yaml-парсер должен фейлиться на неизвестных ключах в strict-mode.
* **R-cfg-2.** Hot-reload отсутствует, поэтому изменение порогов риска требует рестарта (даунтайм).

## Рекомендации

1. **R-cfg-Big.** Заменить flat-парсер на `yaml-cpp`. Поддержка nested структур, списков, anchors.
2. Strict-режим парсинга: unknown keys → `TbError::ConfigValidationFailed`.
3. Unified defaults: только в `config_types.hpp`, ни в одном `cpp`-файле.
4. Cross-field валидация: список инвариантов конфига в `ConfigValidator`.
5. Hash от итогового `AppConfig` (после применения defaults).
6. Hot-reload для подсетов параметров (leverage thresholds, conviction thresholds) с notify-механизмом.
