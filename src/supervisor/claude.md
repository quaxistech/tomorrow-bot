# `src/supervisor` — Супервизор жизненного цикла

## Назначение

Центральный supervisor процесса: управление состоянием системы (Init/Starting/Running/Degraded/ShuttingDown/Stopped), регистрация подсистем (FIFO start, LIFO stop), обработка POSIX-сигналов, symbol-lock registry для координации pipeline'ов, broadcast kill-switch, лимит глобальных позиций.

## Границы ответственности

* Lifecycle: `start/stop/wait_for_shutdown/request_shutdown`.
* `register_subsystem(name, start_fn, stop_fn)`.
* `validate_startup_dependencies()`.
* `enter/exit_degraded_mode(reason)`.
* `try_lock_symbol/unlock_symbol/is_symbol_locked` — координация ордеров на одном символе.
* `register_kill_switch_listener` + `activate_global_kill_switch(reason)`.
* `register_open_position/unregister_position` + `can_open_position()`.
* SIGTERM/SIGINT через async-signal-safe `signal_handler` с CAS на `state_`.
* Configurable shutdown timeout (default 30 s).

## Публичные интерфейсы

* `class Supervisor`:
  * Все методы выше.
  * `current_state() → SystemState noexcept`.
  * `is_kill_switch_active() noexcept`, `kill_switch_reason()`.
  * `is_degraded() noexcept`, `degraded_reason()`.
  * `set_max_global_positions(int)`, `set_shutdown_timeout(seconds)`.
* `enum class SystemState {Initializing, Starting, Running, Degraded, ShuttingDown, Stopped}`.
* `using KillSwitchCallback = function<void(string reason)>`.

## Внутренние компоненты

* `supervisor.hpp/cpp`.

## Зависимости

* `logging`, `metrics`, `clock`, `common`.

## Потоки данных

```
main → supervisor.register_subsystem(...) для каждого pipeline + scanner.rotation
main → supervisor.start():
  state → Starting
  for each subsystem in FIFO: ok = subsystem.start_fn()
       if !ok: enter_degraded_mode (или abort)
  state → Running
main → supervisor.install_signal_handlers()
main → supervisor.wait_for_shutdown() // блокирует
SIGTERM → signal_handler → atomic CAS state → ShuttingDown → cv.notify
main → supervisor.stop():
  state → ShuttingDown
  for each subsystem in LIFO: stop_fn() (с таймаутом)
  state → Stopped
```

## Race conditions

* Иерархия mutex'ов: `state_mutex_ → symbol_lock_mutex_ → kill_switch_mutex_ → positions_mutex_`. См. **Inv-10 в корне**.
* `state_` атомарный (для signal-safe access).
* Subsystem start/stop sequential.

## Ошибки проектирования

* **D-sv-1 (MEDIUM).** SIGINT/SIGTERM CAS — async-signal-safe, но если `signal_handler` идёт во время `start` (невозможно?), state может быть Starting → ShuttingDown с partial init. Mitigation: signal handler устанавливает только флаг, не вызывает stop напрямую.
* **D-sv-2 (LOW).** `validate_startup_dependencies` — что именно валидируется не очевидно из header'а. Требует review реализации.
* **D-sv-3 (LOW).** `unregister_subsystem` для hot-replace — если subsystem ещё running, может race с stop.

## Контракты

### `try_lock_symbol(symbol, pipeline_id) → bool`

* **Pre.** `pipeline_id != ""`.
* **Post.**
  * `true`: `is_symbol_locked(symbol) = true ∧ symbol_locks_[symbol] = pipeline_id`.
  * `false`: символ уже залочен другим pipeline.
* **Invariant.** Reentrant: один и тот же `pipeline_id` получает лок повторно (idempotent).

### `activate_global_kill_switch(reason)`

* **Pre.** Никаких.
* **Post.** `is_kill_switch_active() = true`. Все listener'ы вызваны с `reason`.
* **Invariant.** Idempotent: повторный вызов с тем же reason — no-op.

### `register_open_position(symbol, pipeline_id)`

* **Pre.** Текущий count < `max_global_positions`. (Иначе caller должен проверить через `can_open_position`).
* **Post.** Position зарегистрирована.

## Производственные риски

* **R-sv-1.** Inversion mutex hierarchy при misuse → deadlock. См. **Inv-10**.
* **R-sv-2.** SIGTERM при subsystem stop fail — pipeline может остаться открытым (нет force-kill). Mitigation: hard timeout → SIGKILL через external manager (systemd).
* **R-sv-3.** kill-switch listeners не имеют timeout — если callback виснет, broadcast не завершится.

## Рекомендации

1. Контракт-тест на mutex hierarchy через TSAN.
2. SIGTERM behavior: если stop не успел в timeout → log + abort (controlled crash > stuck process).
3. Kill switch listener timeout (например, 1 sec per listener), then skip.
4. Метрики: `supervisor_state` (gauge enum), `supervisor_subsystems` (gauge count), `supervisor_kill_switch_active` (binary), `supervisor_open_positions`.
