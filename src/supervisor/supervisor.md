# Модуль supervisor

## Что это за блок

`src/supervisor` — это модуль верхнеуровневого управления жизненным циклом процесса. Его задача не торговать, не считать сигналы и не исполнять ордера, а держать под контролем состояние системы как набора подсистем.

Если смотреть на архитектуру сверху, `supervisor` стоит между `app` и конкретными runtime-компонентами вроде `TradingPipeline`.

В текущей реализации он умеет несколько разных вещей:

- хранить глобальное состояние системы (`Initializing`, `Starting`, `Running`, `Degraded`, `ShuttingDown`, `Stopped`);
- регистрировать подсистемы как пары `start_fn` / `stop_fn`;
- запускать подсистемы в порядке регистрации;
- останавливать их в обратном порядке;
- переводить систему в degraded mode, если часть подсистем не стартовала;
- принимать `SIGTERM` / `SIGINT` и переводить процесс в состояние shutdown;
- держать реестр symbol-lock'ов;
- держать global kill switch с broadcast по listener'ам;
- держать глобальный реестр открытых позиций и простой лимит по их количеству.

Если описать коротко, `supervisor` — это не orchestration-движок торговой логики, а системный coordinator для lifecycle и нескольких глобальных runtime-регистров.

## Из каких файлов состоит модуль

### `supervisor.hpp`

Это публичный интерфейс модуля.

Здесь описаны:

- enum `SystemState`;
- строковое преобразование `to_string(SystemState)`;
- класс `Supervisor`;
- API lifecycle-управления;
- API degraded mode;
- API регистрации подсистем;
- API symbol-lock registry;
- API kill switch broadcast;
- API глобального учёта позиций;
- API shutdown timeout.

Именно этот файл задаёт внешний контракт модуля.

### `supervisor.cpp`

Это полная runtime-реализация `Supervisor`.

Здесь находится:

- статический `instance_` для signal handler;
- async-signal-safe `signal_handler(int sig)` — обрабатывает Starting/Running/Degraded;
- запуск / остановка подсистем (со snapshot'ами для thread safety);
- переходы состояния системы (все через CAS, без unconditional store);
- condition_variable для shutdown notification;
- внутренняя логика degraded mode (с накоплением причин);
- реализация lock registry;
- реализация kill switch уведомлений;
- реализация глобального реестра позиций;
- метрики наблюдаемости (4 gauge).

### `CMakeLists.txt`

Собирает библиотеку `tb_supervisor` из одного файла `supervisor.cpp`.

По зависимостям видно, что supervisor:

- зависит от `tb_common` из-за типа `Symbol`;
- зависит от `tb_logging`;
- зависит от `tb_metrics`;
- зависит от `tb_clock`.

Это показывает, что модуль задуман как инфраструктурный сервис верхнего уровня, а не как часть доменной торговой логики.

## Что хранит Supervisor внутри

По текущей реализации у класса есть несколько независимых подсистем состояния.

### 1. Состояние процесса

Основное состояние системы лежит в `std::atomic<int> state_`.

Почему `int`, а не `std::atomic<SystemState>`:

- автор явно решил не полагаться на atomic enum напрямую;
- enum приводится к `int` при store/load;
- наружу состояние возвращается обратно как `SystemState` через `current_state()`.

Дополнительно есть:

- `degraded_reason_`;
- `state_mutex_` для защиты составного состояния, где нужно менять и state, и текст причины.

### 2. Реестр подсистем

Supervisor хранит массив `subsystems_`, где каждый элемент — это `SubsystemEntry`:

- `name`;
- `start_fn`;
- `stop_fn`;
- `started`.

Важно: это именно `std::vector`, а не map.

Практически это означает:

- порядок регистрации сохраняется;
- запуск идёт в прямом порядке;
- остановка идёт в обратном порядке;
- модуль не навязывает граф зависимостей между подсистемами, а полагается на порядок добавления.

### 3. Symbol lock registry

Есть map:

- `symbol_locks_ : symbol -> pipeline_id`.

Он используется как простой mutex на уровне торгового символа между pipeline.

### 4. Kill switch registry

Есть:

- атомарный флаг `kill_switch_active_`;
- строка `kill_switch_reason_`;
- map `kill_switch_listeners_ : listener_id -> callback`.

Это уже не lifecycle, а механизм глобального emergency-broadcast.

### 5. Глобальный реестр позиций

Есть map:

- `open_positions_ : symbol -> pipeline_id`.

Плюс:

- `max_global_positions_`.

Это простая глобальная защита от превышения числа одновременно открытых позиций.

### 6. Таймаут остановки

`shutdown_timeout_` по умолчанию равен `30s`.

Он используется в `stop()` как общий deadline на graceful shutdown.

## Как supervisor работает по шагам

Ниже описан реальный порядок работы текущей реализации.

## 1. Конструирование объекта

В конструктор приходят:

- `ILogger`;
- `IMetricsRegistry`;
- `IClock`.

Внутри конструктор:

- сохраняет зависимости;
- ставит `state_ = Initializing`;
- записывает `this` в статический `instance_`.

Последний пункт критичен: signal handler во всём процессе работает только через этот глобальный указатель.

Практический смысл:

- Supervisor в рантайме по сути рассчитан на один активный экземпляр;
- обработка системных сигналов завязана именно на него.

## 2. Установка signal handlers

Метод `install_signal_handlers()` регистрирует:

- `SIGTERM`;
- `SIGINT`.

Оба сигнала привязываются к `Supervisor::signal_handler`.

Сам handler написан осторожно:

- не логирует;
- не трогает mutex;
- не вызывает `request_shutdown()`;
- делает только atomic CAS.

Он написан с учётом async-signal-safety.

### Что именно делает `signal_handler`

Он пытается перевести состояние в `ShuttingDown` из трёх состояний:

- `Running`;
- `Degraded`;
- `Starting`.

Порядок попыток CAS: Running → Degraded → Starting. Это позволяет корректно обработать SIGTERM на любом этапе работы.

Если система в `Initializing` — сигнал не обрабатывается (зависимости ещё не инициализированы).

## 3. Регистрация подсистем

`register_subsystem(name, start_fn, stop_fn)` просто добавляет запись в `subsystems_`.

На этом этапе Supervisor:

- не валидирует уникальность имени;
- не проверяет корректность функций;
- не строит зависимости между подсистемами;
- не запускает ничего сразу.

Он лишь запоминает lifecycle hooks.

Это делает модуль простым: порядок и состав подсистем определяются снаружи, в первую очередь из `app/main.cpp`.

## 4. Запуск системы: `start()`

`start()` делает четыре основных шага.

### 4.1 Переводит состояние в `Starting`

Сразу выполняется:

- `state_ = Starting`;
- логируется начало запуска.

### 4.2 Валидирует базовые зависимости

`validate_startup_dependencies()` проверяет:

- что есть `logger_`;
- что есть `metrics_`;
- что есть `clock_`.

Поведение здесь асимметрично:

- отсутствие `logger_` приводит к немедленному `false`, потому что дальше даже логировать нельзя;
- отсутствие `metrics_` или `clock_` логируется как ошибка и тоже делает запуск невалидным.

Если валидация не прошла:

- логируется ошибка;
- `state_` переводится в `Stopped`;
- `start()` просто завершает работу.

Важно: исключение не бросается.

### 4.3 Запускает подсистемы по порядку регистрации

Supervisor проходит циклом по `subsystems_`.

Для каждой подсистемы:

- логирует попытку запуска;
- вызывает `start_fn()`;
- если вернулось `true`, ставит `started = true`;
- если вернулось `false`, логирует ошибку и вызывает `enter_degraded_mode(...)`.

То есть модель не fail-fast, а fail-soft:

- одна неудача не останавливает запуск остальных;
- система может дойти до partially started состояния.

### 4.4 Финализирует состояние запуска

После цикла Supervisor проверяет текущее состояние.

Если degraded mode не был включён:

- система переводится в `Running`.

Если хотя бы одна подсистема не стартовала:

- состояние остаётся `Degraded`.

Это значит, что `start()` в текущем виде не гарантирует полностью успешный запуск. Он гарантирует лишь попытку стартовать всё зарегистрированное и корректно пометить system state.

## 5. Degraded mode

`enter_degraded_mode(reason)` делает:

- lock `state_mutex_`;
- CAS: только из `Starting` или `Running` → `Degraded` (не перезаписывает `ShuttingDown`/`Stopped`);
- сохраняет `degraded_reason_`;
- при повторных вызовах накапливает причины через `"; "`;
- логирует предупреждение;
- обновляет метрику `supervisor_state`.

`exit_degraded_mode()` делает обратное:

- под lock пытается atomically перевести `Degraded -> Running`;
- очищает `degraded_reason_`;
- логирует выход из деградации.

Практически degraded mode в этой реализации — это:

- флаг состояния;
- текстовые причины (с накоплением).

Важно понимать, чего здесь нет:

- нет автоматического recovery loop;
- нет перезапуска упавших подсистем;
- нет policy engine, который решает, что можно продолжать, а что нельзя.

Degraded mode маркирует состояние системы и безопасен относительно concurrent shutdown.

## 6. Ожидание shutdown: `wait_for_shutdown()`

Этот метод блокирует поток до тех пор, пока `state_` не станет:

- `ShuttingDown`, либо
- `Stopped`.

Реализация использует `std::condition_variable` с периодическим wakeup (200ms):

- `request_shutdown()` notify'ит CV напрямую — мгновенная реакция;
- `signal_handler` не может notify CV (not async-signal-safe) — ловим через 200ms timeout;
- 200ms — приемлемая задержка для начала graceful shutdown.

Практический смысл:

- main-поток спит на CV, а не в busy-wait loop;
- для программных shutdown через `request_shutdown()` — мгновенная реакция;
- для сигналов — максимальная задержка 200ms.

## 7. Запрос shutdown: `request_shutdown()`

`request_shutdown()` пытается перевести систему в `ShuttingDown`, но только из:

- `Running`;
- `Degraded`.

Если переход удался:

- пишется лог о запросе завершения.

Если нет:

- метод ничего не делает.

То есть он идемпотентный и не шумит повторными сообщениями, если shutdown уже начался.

## 8. Остановка системы: `stop()`

Это главный graceful shutdown метод.

Он делает несколько шагов.

### 8.1 Вычисляет deadline

`deadline = now + shutdown_timeout_`.

Этот deadline общий на весь процесс остановки, а не отдельный для каждой подсистемы.

### 8.2 Останавливает подсистемы в обратном порядке

Supervisor идёт по `subsystems_` через reverse iterator.

Для каждой подсистемы:

- проверяет, не истёк ли общий deadline;
- если истёк, логирует предупреждение и прекращает дальнейшую остановку;
- если подсистема была помечена как `started`, вызывает `stop_fn()`;
- после успешного вызова ставит `started = false`.

Это даёт классическую LIFO-семантику остановки: сначала останавливаются самые поздно зарегистрированные компоненты.

### 8.3 Гасит исключения при stop

Если `stop_fn()` бросает исключение:

- Supervisor ловит `std::exception`;
- пишет ошибку;
- продолжает shutdown остальных подсистем.

То есть shutdown designed to continue even on partial stop failures.

### 8.4 Финализирует lifecycle

После прохода состояние принудительно переводится в `Stopped`, и логируется финальное сообщение.

## 9. Удаление подсистем: `unregister_subsystem()`

Этот метод нужен прежде всего для горячей замены pipeline.

Он:

- под lock проходит по `subsystems_`;
- удаляет все записи с данным именем через `erase(remove_if(...))`;
- логирует факт удаления.

Важно:

- имя не обязано быть уникальным, но удаляются все совпадения;
- если подсистема уже была запущена, сам `unregister_subsystem()` её не останавливает.

То есть lifecycle и registry здесь разделены:

- удалить запись из supervisor;
- остановить runtime-объект.

Эти два действия caller обязан координировать сам.

## 10. Symbol lock registry

Эта часть не участвует в запуске процесса, но даёт глобальную защиту от гонки пайплайнов по символу.

### `try_lock_symbol(symbol, pipeline_id)`

Логика такая:

- если символа нет в `symbol_locks_`, он записывается и метод возвращает `true`;
- если символ уже залочен тем же pipeline, тоже возвращается `true`;
- если символ уже залочен другим pipeline, возвращается `false` и пишется warning.

То есть lock idempotent для владельца и эксклюзивен для остальных.

### `unlock_symbol(symbol, pipeline_id)`

Метод снимает лок только если:

- символ реально есть в map;
- лок принадлежит именно этому `pipeline_id`.

Если нет:

- лок не снимается;
- пишется warning.

### `is_symbol_locked(symbol)`

Просто проверяет наличие ключа в map.

Практический смысл этой подсистемы:

- дать централизованный runtime guard на уровне символа.

Но важно: в текущем workspace этот API по коду нигде не используется. То есть механизм реализован, но ещё не встроен в исполняемый trading flow.

## 11. Global kill switch broadcast

Это отдельный глобальный emergency-механизм.

### `register_kill_switch_listener(listener_id, callback)`

Просто добавляет callback в map listener'ов.

### `activate_global_kill_switch(reason)`

Алгоритм здесь аккуратный:

1. Атомарно ставится флаг `kill_switch_active_ = true`.
2. Если kill switch уже был активен, метод просто пишет warning и выходит.
3. Логируется активация.
4. Под mutex копируются callback'и и причина.
5. Callback'и вызываются уже вне lock.

Это важная реализация:

- автор специально избегает deadlock при re-entrant callback'ах;
- исключение одного listener'а не ломает доставку другим.

### `deactivate_global_kill_switch()`

Метод:

- атомарно сбрасывает флаг;
- очищает `kill_switch_reason_` под mutex;
- логирует деактивацию.

### `is_kill_switch_active()`

Возвращает атомарный флаг.

Практически kill switch здесь — это broadcast/event bus, а не механизм принудительной остановки сам по себе. Supervisor только рассылает событие; реальные действия должны реализовываться listener'ами.

И снова важная деталь: по текущему workspace этот API пока не подключён к реальному runtime-коду.

## 12. Глобальный реестр позиций и лимит

Эта часть тоже изолирована от lifecycle-логики.

### `register_open_position(symbol, pipeline_id)`

Под mutex просто записывает пару:

- `symbol -> pipeline_id`.

После этого логируется текущее общее количество позиций.

### `unregister_position(symbol, pipeline_id)`

Удаляет запись только если owner совпадает.

### `global_open_positions_count()`

Возвращает размер map.

### `set_max_global_positions(max_positions)`

Меняет глобальный лимит.

### `can_open_position()`

Возвращает:

- `open_positions_.size() < max_global_positions_`.

То есть это очень простой счётчик глобального capacity limit, без приоритезации, без категории символов и без notionals.

Как и предыдущие две подсистемы, этот механизм в текущем коде реализован, но не интегрирован в main runtime path.

## Как Supervisor встроен в приложение сейчас

Главный consumer модуля — `app/main.cpp`.

Реальный путь использования сейчас такой:

1. `main.cpp` создаёт `Supervisor` после построения `symbol_rules`.
2. Сразу вызывает `install_signal_handlers()`.
3. Для каждого `TradingPipeline` регистрирует подсистему с именем `pipeline_<symbol>`.
4. Вызывает `supervisor.start()`.
5. После старта процесса вызывает `supervisor.wait_for_shutdown()`.
6. При горячей замене symbol-set вызывает `unregister_subsystem(...)` для старых pipeline.
7. На финальном shutdown вызывает `supervisor.stop()`.

То есть в текущем runtime Supervisor используется в узкой, но важной роли:

- registry + lifecycle coordinator для pipeline.

При этом дополнительные возможности supervisor сейчас не используются:

- symbol lock registry;
- kill switch broadcast;
- global positions limit API.

## Что модуль supervisor НЕ делает

Для понимания архитектуры важно явно проговорить ограничения.

Supervisor в текущем виде не:

- мониторит health подсистем в фоне;
- автоматически рестартует упавшие компоненты;
- различает critical и non-critical подсистемы;
- управляет dependency graph между подсистемами;
- сам по себе закрывает позиции, отменяет ордера или останавливает торговлю при kill switch;
- управляет pipeline hot-swap end-to-end.

Последний пункт важен особенно:

- `unregister_subsystem()` лишь удаляет запись из registry;
- реальную остановку pipeline делает внешний код в `main.cpp`.

То есть Supervisor не является полным process manager уровня systemd или actor runtime. Он намного проще.

## Исправления от 2026-04-06

### Исправленные баги

1. **`start()` перезаписывал `ShuttingDown`** — использовал безусловный `state_.store(Running)` после цикла запуска; если сигнал приходил во время startup, Running затирал ShuttingDown. Исправлено на CAS: `Starting → Running`.

2. **`enter_degraded_mode()` перезаписывал `ShuttingDown`** — безусловный `state_.store(Degraded)` мог затереть shutdown-состояние. Исправлено на CAS: только из `Starting`/`Running` → `Degraded`. При повторных вызовах причины накапливаются через `"; "`.

3. **Signal handler игнорировал `Starting`** — SIGTERM во время запуска молча терялся. Добавлен переход `Starting → ShuttingDown`.

4. **Data race на `subsystems_` в `stop()`** — итерация без мьютекса при конкурентном `unregister_subsystem()` из hot-swap потока. Исправлено: snapshot под мьютексом, итерация — без.

5. **Data race в `subsystem_count()`** — чтение `.size()` без мьютекса. Добавлен `lock_guard`.

6. **`register_open_position()` молча игнорировала конфликт** — `emplace` не перезаписывает; если другой pipeline регистрировал позицию по тому же символу, запись оставалась старой. Заменено на `insert_or_assign` с логированием конфликта.

7. **`set_max_global_positions()` принимал отрицательные значения** — добавлена валидация `>= 1`.

### Улучшения

1. **`wait_for_shutdown()` заменён на `condition_variable`** — вместо busy-wait 100ms теперь `wait_for(200ms)`. `request_shutdown()` notify'ит CV напрямую (мгновенная реакция); signal handler не может notify CV (not signal-safe), поэтому ловим через 200ms timeout.

2. **Metrics emission** — `metrics_` больше не мёртвая зависимость. Создаются 4 gauge: `supervisor_state`, `supervisor_subsystems_total`, `supervisor_open_positions`, `supervisor_kill_switch_active`. Обновляются при каждом переходе состояния.

3. **`kill_switch_reason()` getter** — добавлен публичный метод для чтения причины kill switch.

4. **`install_signal_handlers()` использует `sigaction`** — вместо `std::signal()`, что даёт POSIX-portable поведение без SA_RESTART (прерываем блокирующие вызовы).

5. **`start()` проверяет сигнал между запусками подсистем** — если SIGTERM пришёл во время startup, не продолжаем запускать оставшиеся подсистемы.

6. **Запрет move-семантики** — добавлены `Supervisor(Supervisor&&) = delete` (static instance_ делает move некорректным).

7. **Документированный порядок захвата мьютексов** — `state_mutex_ → symbol_lock_mutex_ → kill_switch_mutex_ → positions_mutex_` (предотвращение deadlock).

## Практические наблюдения по текущей реализации

### 1. `Supervisor` — lifecycle-registry с глобальными runtime-регистрами

Основной реально используемый сценарий — зарегистрировать pipeline и потом централизованно их запустить/остановить. Дополнительные регистры (symbol locks, kill switch, positions) реализованы и готовы к интеграции.

### 2. Degraded mode описательный, но безопасный

Он хранит состояние и причины (с накоплением), но не запускает recovery logic. Не может перезаписать shutdown-состояние.

### 3. Shutdown по сигналу сделан аккуратно

`signal_handler` только atomic CAS, принимает Starting/Running/Degraded → ShuttingDown.

### 4. Запуск tolerate partial failure

Если одна подсистема не стартовала, supervisor переводит систему в `Degraded` и продолжает. Между запусками проверяет, не пришёл ли сигнал.

### 5. Порядок регистрации определяет semantics запуска и остановки

Поскольку внутренне используется `vector`, порядок `register_subsystem()` полностью определяет lifecycle-ordering (FIFO запуск, LIFO остановка).

## Архитектурная роль блока

С архитектурной точки зрения `src/supervisor` выполняет сразу несколько ролей.

### 1. Lifecycle Coordinator

Он задаёт общий protocol запуска и остановки набора подсистем.

### 2. Process State Holder

Он хранит общее состояние процесса и даёт другим частям системы единое место, откуда можно узнать lifecycle-status.

### 3. Global Runtime Registry

Он содержит несколько глобальных реестров:

- symbol locks;
- kill switch listeners;
- open positions.

### 4. Signal Boundary

Через него сигналы ОС переводятся в понятный lifecycle-state внутри приложения.

## Практический вывод

Если сформулировать совсем коротко, модуль `supervisor` делает следующее:

`supervisor` собирает в одном месте lifecycle-состояние процесса, запускает и останавливает зарегистрированные подсистемы в правильном порядке, переводит системные сигналы в shutdown-state и дополнительно предоставляет несколько глобальных runtime-регистров, которые могут использоваться торговыми pipeline для координации между собой.

Если понадобится менять:

- порядок запуска и остановки подсистем;
- политику degraded mode;
- реакцию на `SIGTERM` / `SIGINT`;
- модель глобальных symbol-lock'ов;
- kill switch broadcast;
- глобальные лимиты позиций;
- semantics hot-unregister подсистем,

то основная точка изменений находится именно в `src/supervisor`.