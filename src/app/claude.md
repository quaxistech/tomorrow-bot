# `src/app` — Точка входа

## Назначение

Bootstrap процесса: парсинг CLI, загрузка конфигурации, инициализация компонентов, запуск scanner и pipeline, обработка signal'ов, корректное завершение. HTTP-сервер для Prometheus-метрик. Runtime manifest для трассировки сборки.

## Границы ответственности

* `main(argc, argv)` — точка входа.
* `AppBootstrap::initialize(config_path)` — все DI зависимости.
* `HttpEndpointServer` — синхронный Boost.Beast HTTP server для `/metrics`.
* `RuntimeManifest::build(...)` — фиксация git_sha + version + config_hash + endpoint.

## Публичные интерфейсы

* `class AppBootstrap` — `initialize(string_view path) → Result<AppComponents>`.
* `struct AppComponents` — `{config_loader, secret_provider, logger, metrics, clock, config}`.
* `class HttpEndpointServer` — простой sync HTTP server с handler-callback.
* `class RuntimeManifest` — `{git_sha, version, build_type, config_hash, exchange_endpoint}`.

## Внутренние компоненты

* `main.cpp` — main + scanner driver + pipeline assembly (~1200 строк, см. ниже).
* `app_bootstrap.hpp/cpp` — bootstrap.
* `http_server.hpp/cpp` — metrics + health server.
* `runtime_manifest.hpp/cpp` — manifest.
* `health_state.hpp` (production hardening 2026-05-08) — атомарный shared `HealthState` для k8s probes (`/livez`, `/readyz`, `/healthz`).

## K8s probes (production hardening 2026-05-08)

| Endpoint | Возвращает | Семантика |
|----------|------------|-----------|
| `GET /livez` | 200 пока процесс живой; 503 после SIGTERM/SIGINT | Liveness probe — k8s рестартует pod при провале |
| `GET /readyz` | 200 если ready; 503 с reason иначе | Readiness probe — k8s drain'ит из service при провале |
| `GET /healthz` | alias для `/readyz` | Backward-compat |

`is_ready()` ⇔ `subsystems_started ∧ ≥1 connected pipeline ∧ ¬kill_switch ∧ ¬degraded ∧ ¬shutdown_requested`.

Состояние обновляется:
* `early_shutdown_handler` (SIGTERM/SIGINT) → `mark_shutdown_requested`.
* `supervisor.start()` → `mark_subsystems_started`, `set_registered_pipeline_count`.
* `health_monitor_thread` (jthread, 1Hz polling) → `set_connected_pipeline_count`, `set_kill_switch_active`, `set_degraded`.
* `supervisor.stop()` → `mark_subsystems_stopped`.

## Зависимости

* `config`, `security`, `logging`, `metrics`, `clock` (через Bootstrap).
* `supervisor`, `scanner`, `pipeline`, `portfolio`, `exchange/bitget` (в main).
* Boost.Beast (HTTP server).

## Потоки данных

```
main(argc, argv):
  parse_args
  AppBootstrap::initialize(config_path) → AppComponents
  emit runtime_manifest log
  start metrics_server (если config.metrics.enabled)
  scanner_rest_client = make BitgetRestClient (public)
  market_scanner = make ScannerEngine
  scanner_result = market_scanner.scan() с retry до 30 мин
  active_symbols = result.selected_symbols ∪ open_positions
  per symbol:
     pipeline = make TradingPipeline(config, secret_provider, logger, clock, metrics, symbol, shared_portfolio)
     pipeline->set_num_pipelines(N)
     pipeline->set_symbol_precision/exchange_rules
     supervisor.register_subsystem(pipeline.name, [&]{return pipeline->start();}, [&]{pipeline->stop();})
  supervisor.start()
  supervisor.install_signal_handlers()
  supervisor.wait_for_shutdown()
  supervisor.stop()
  return 0
```

## Race conditions

`main` — single-threaded до `supervisor.start`. `HttpEndpointServer` — собственный thread.

## Ошибки проектирования

* **D-app-1 (HIGH).** `main.cpp` ≈ 1200 строк — god-function. Логика scanner driver, pipeline assembly, position discovery, capital allocation смешана. Должна быть декомпозиция: `ScannerDriver`, `PipelineFactory`, `BalanceSync`.
* **D-app-2 (HIGH).** `std::this_thread::sleep_for(60s)` в retry-цикле scanner блокирует main без shutdown-aware (см. **Defect-D8 в корне**). SIGTERM в это время игнорируется.
* **D-app-3 (MEDIUM).** Открытые позиции на бирже определяются через прямой REST вызов в main (не через RecoveryService) — duplicate logic.
* **D-app-4 (MEDIUM).** Конфиг hardcoded paths для `.env` (`./.env`, `./configs/.env`, `/etc/tomorrow-bot/.env`) — должно быть configurable через CLI.
* **D-app-5 (LOW).** `HttpEndpointServer` synchronous — для metrics endpoint OK, но не масштабируется для другого API.

## Контракты

### `AppBootstrap::initialize(path) → Result<AppComponents>`

* **Pre.** `path` существует.
* **Post.**
  * Успех: все DI инициализированы, ProductionGuard validated.
  * Неудача: `Err(ConfigLoadFailed | ConfigValidationFailed | ProductionGuardFailed)`.
* **Invariant.** После initialize: components.* не nullptr.

### `RuntimeManifest::build(config_hash, endpoint, now) → RuntimeManifest`

* **Pre.** Все аргументы валидные.
* **Post.** Manifest содержит compile-time `TB_GIT_SHA`, `TB_VERSION`, `TB_BUILD_TYPE` + runtime hash и endpoint.
* **noexcept.** Желательно.

## Производственные риски

* **R-app-1.** Долгая инициализация (scanner до 30 мин при market down) блокирует supervisor → нет ready-state для k8s health check.
* **R-app-2.** Manifest не персистится — сложнее post-mortem.
* **R-app-3.** Если scanner продолжит retry на signal — process не завершается gracefully.

## Рекомендации

1. **R-app-Big.** Декомпозировать `main.cpp` на `ScannerDriver`, `PipelineFactory`, `BalanceDiscovery`.
2. Shutdown-aware sleep: использовать `Supervisor`-aware sleep (cv с timeout) вместо `std::this_thread::sleep_for`.
3. CLI-флаг `--env-path` для `.env`.
4. Persist runtime manifest в `persistence` (для аудита).
5. `/healthz` и `/readyz` endpoint'ы (см. R-17 в корне).
6. Тест integration: simulate full bootstrap с mock components, проверка lifecycle.
