# План Доведения Tomorrow Bot До Production-Grade На Bitget

**Дата:** 2026-04-23  
**Статус:** production-only roadmap  
**Цель:** довести систему до состояния, где весь runtime является единым боевым контуром Bitget USDT-M Futures, а прибыль формируется за счёт качественной реакции на рынок, микроструктурного исполнения и управляемой парной/хедж-логики, а не за счёт грубых time-based выходов или рискованного разгона плечом.

---

## 1. Целевое состояние

К концу программы бот должен обладать следующими свойствами:

1. В проекте существует один runtime-путь: production.
2. Решения на открытие, удержание, частичное сокращение, хеджирование и закрытие принимаются по рыночной оценке, а не по elapsed time.
3. Противоположные ордера используются не как аварийная затычка, а как полноценный paired-profit engine: хедж открывается по подтверждённой смене состояния рынка, а обе ноги закрываются по joint EV.
4. Execution stack Bitget-native: private WS first, queue-aware maker/taker choice, deterministic order state, корректный fill accounting, no phantom positions.
5. Risk слой отделён от alpha слоя: risk убивает только по safety-причинам, alpha управляет закрытием по market state.
6. Все production-решения объяснимы, логируются и могут быть восстановлены после рестарта без потери состояния.

---

## 2. Уже Подтверждённые Блокеры

Ниже те блокеры, которые нельзя обойти архитектурными словами; их надо закрывать в коде первыми.

1. В `hedge_pair_manager.cpp` подтверждён hard 5s gate на profitable unwind/reverse. Это прямое нарушение требования market-driven close.
2. В аудите уже зафиксированы критические дефекты по execution, fill accounting, recovery, config safety, order FSM, REST/WS convergence и pair lifecycle.
3. В дереве исходников всё ещё есть stale артефакты paper/shadow эпохи в документации и части public surface. Runtime уже production-only, но кодовая база ещё не полностью очищена семантически.
4. В нескольких местах проект комментарием декларирует «no timeout / market-driven», а рядом остаются time-coupled эвристики или legacy wording. Это опасно, потому что вводит в заблуждение оператора и разработчика одновременно.

---

## 3. Главные Принципы Целевой Архитектуры

### 3.1 Production-Only

- Один боевой бинарник.
- Один тип режима: `TradingMode::Production`.
- Никаких shadow, paper, dry-run submitters, альтернативных execution-путей, скрытых fallback-веток для «ненастоящей» торговли.
- Любая оффлайн-проверка живёт только в тестах и tooling, но не в runtime surface production-кода.

### 3.2 Market-Evaluated Exit Only

- Elapsed time не может быть триггером alpha close.
- Время допустимо только как инфраструктурный сигнал здоровья: stale feed, WS gap, exchange desync, deadman safety.
- Закрытие позиции должно зависеть от joint market state:
  - continuation value,
  - reversal probability,
  - shock probability,
  - expected slippage,
  - fill probability,
  - pair net EV после fees/funding.

### 3.3 Pair/Hedge As Profit Engine

- Хедж не должен быть простым «замком от убытка».
- Пара должна оцениваться как единый экономический объект: `keep both`, `close both`, `close primary`, `close hedge`, `increase hedge`, `reduce hedge`.
- Решение должно приниматься по joint EV surface, а не по двум несвязанным локальным эвристикам.

### 3.4 Deterministic State

- Любой ордер, fill, отмена, reversal, pair transition и forced flatten должны быть событийно восстановимы.
- Любой рестарт должен возвращать систему в тот же state без двойных позиций и без потери fill accounting.

---

## 4. Технологический Стек, На Который Нужно Выходить

Ниже не академический wishlist, а тот стек, который действительно нужен сильному крипто-фьючерсному боту уровня 2026.

### 4.1 Microstructure Layer

- Полный и корректный top-of-book + depth snapshot.
- Order Flow Imbalance (OFI), microprice, queue imbalance, depth depletion velocity.
- Trade-to-book linkage: агрессор, sweep detection, refill speed, absorption, spoof-like pull bursts.
- Short-horizon event features на окнах 50ms / 100ms / 250ms / 1s / 5s.

### 4.2 Execution Intelligence

- Queue-aware fill model: вероятность исполнения лимита как survival/hazard model.
- Adverse selection model: вероятность, что fill будет получен перед неблагоприятным движением.
- Dynamic maker/taker routing по expected value, а не по статическим порогам spread.
- Cancel/repost policy на основе queue decay, refill speed и microprice drift.

### 4.3 Predictive Short-Horizon Stack

- Rule-based ядро остаётся для safety и explainability.
- Сверху добавляется короткогоризонтный predictive layer:
  - calibrated probabilistic models для `P(continue)`, `P(reversal)`, `P(shock)`;
  - contextual bandit для выбора execution style и hedge action;
  - online calibration через isotonic/Platt/conformal layer;
  - lightweight sequence model для microstructure horizon: TCN или SSM-class model для 200ms-5s горизонта.

### 4.4 Pair/Hedge Economics

- Joint EV surface по обеим ногам.
- Funding-aware и spread-aware valuation.
- Cross-leg slippage budget.
- Pair basis / divergence / convergence features.
- Two-leg close coordinator с защитой от orphan leg и partial-fill desync.

### 4.5 Online Adaptation

- Модель не должна адаптироваться только по win rate.
- Нужны:
  - online calibration quality,
  - feature drift detection,
  - regime-conditioned parameter sets,
  - doubly-robust / counterfactual evaluation для execution policy.

---

## 5. Что Надо Переделать В Архитектуре

### 5.1 Единый Exit Arbiter

Нужен один канонический контур закрытия позиции:

1. `exit_orchestrator` оценивает continuation / reversal / shock / cost-to-stay.
2. `market_reaction_engine` даёт вероятностную надстройку.
3. `hedge_pair_manager` работает не отдельно «по своим правилам», а как pair-action policy внутри того же экономического пространства.
4. `trading_pipeline` только исполняет уже принятое решение и следит за state transitions.

Что изменить:

- удалить все elapsed-time gates из alpha close path;
- убрать hard hold thresholds из pair unwind / reverse;
- оставить time thresholds только в `operational_safety` и recovery/infrastructure guards;
- запретить в код-ревью любой новый close trigger вида `if (hold_ns > X) close`.

### 5.2 Полный Redesign HedgePairManager

Текущая версия должна превратиться из rule box в полноценный pair policy engine.

Новая логика должна считать:

- `EV(keep primary + keep hedge)`
- `EV(close hedge)`
- `EV(close primary)`
- `EV(close both)`
- `EV(increase hedge ratio)`
- `EV(reduce hedge ratio)`

Каждый вариант должен учитывать:

- expected slippage обеих ног,
- fill probability обеих ног,
- fees,
- funding carry,
- regime transition probability,
- probability of further spread widening / liquidity collapse,
- asymmetry between primary alpha and hedge alpha.

Обязательные изменения по модулю:

1. Удалить `hedge_hold_ns > 5s` как условие alpha unwind.
2. Ввести hysteresis по economic state, а не по времени.
3. Ввести explicit confidence threshold для reverse transition.
4. Добавить protection against partial reverse: two-phase close/open orchestration.
5. Разделить `safety flatten` и `alpha pair transition`.

### 5.3 Execution Layer Должен Стать Queue-Aware

Нужно довести `execution_alpha_engine`, `execution_planner`, `twap_executor`, `order_watchdog`, private WS и order registry до единой модели исполнения.

Обязательные свойства:

- лимитная заявка не ставится, если нет шанса на fill без сильной adverse selection;
- aggressive execution не включается без позитивного EV против maker path;
- watchdog не должен разрушать state machine;
- private WS является первичным источником fills, REST only reconciliation;
- все fills сходятся в одну idempotent точку применения.

### 5.4 Single Source Of Truth Для Состояния

Нужно жёстко закрепить event-sourced модель:

- order state,
- portfolio state,
- pair state,
- hedge state,
- realized / unrealized PnL,
- fees / funding / slippage,
- causal link: signal -> intent -> order -> fill -> position transition -> close reason.

---

## 6. Детальный План Работ По Фазам

## Фаза 0. Убрать ложные утверждения и time-gated alpha close

**Цель:** привести production-path к правде и вычистить архитектурные противоречия.

Работы:

1. Исправить `hedge_pair_manager.cpp`: убрать 5s hard gate из веток `CloseHedge` и `ClosePrimary`.
2. Явно зафиксировать правило: elapsed time запрещён как alpha close trigger.
3. Вычистить stale комментарии и документацию, где ещё фигурируют `time_exit`, `paper`, `shadow` или устаревший execution path.
4. Удалить из public surface всё, что не соответствует production-only контракту.

Acceptance criteria:

- в production `.cpp/.hpp` больше нет живых alpha close условий по `hold_ns > X`;
- runtime surface содержит только production semantics;
- лог объяснения close показывает market driver, а не time driver.

## Фаза 1. Пересобрать Pair/Hedge Engine В Joint EV Policy

**Цель:** сделать opposite-order систему главным источником аккуратной прибыли, а не аварийным костылём.

Работы:

1. Добавить pair EV matrix.
2. Ввести per-leg fill/slippage estimator.
3. Ввести pair close coordinator:
	- atomic intent grouping,
	- partial-fill recovery,
	- orphan-leg rollback,
	- reason-code discipline.
4. Вынести hedge trigger / unwind / reverse в единую конечную автоматику с экономическими переходами.
5. Добавить funding-aware и liquidity-aware pair carry model.

Acceptance criteria:

- pair close/reverse decisions принимаются только по EV surface;
- любой reverse transition устойчив к partial fill;
- pair PnL attribution показывает, где был заработан edge: primary, hedge, timing, execution.

## Фаза 2. Довести Execution До Bitget-Top-Class

**Цель:** минимизировать комиссии, adverse selection и latency slippage.

Работы:

1. Достроить queue-aware maker/taker router.
2. Ввести live fill hazard model на основе собственных fills.
3. Ввести queue decay / microprice drift как триггер cancel/repost.
4. Пересчитать execution alpha на реальных Bitget response patterns.
5. Доработать batch close / click-backhand / reduce-only semantics для reversal и paired exits.

Acceptance criteria:

- execution style выбирается по EV, а не по статической эвристике;
- fill probability calibration сходится;
- p99 state convergence order->fill->portfolio укладывается в заданный бюджет.

## Фаза 3. Обновить Feature и Model Stack До 2026-Grade

**Цель:** бот должен чувствовать microstructure, regime shifts и short-horizon reversals быстрее текущего rule stack.

Работы:

1. Добавить OFI, microprice drift, refill speed, sweep bursts, queue depletion, local volatility shock.
2. Добавить cross-symbol and market-wide stress features: BTC lead-lag, dominance shock, correlated liquidation bursts.
3. Ввести calibrated probability heads:
	- `P(continue)`
	- `P(reversal)`
	- `P(shock)`
	- `P(fill_passive)`
4. Подключить contextual bandit для выбора execution / hedge action.
5. Добавить lightweight sequence model над tape+book features для горизонта 200ms-5s.

Acceptance criteria:

- модели дают калиброванные вероятности, не только score;
- uncertainty engine работает на calibration error, а не на абстрактном heuristics-only score;
- новые признаки действительно используются в execution и pair policy, а не только логируются.

## Фаза 4. Перестроить Risk Layer Так, Чтобы Он Не Подменял Alpha

**Цель:** risk должен защищать капитал, а не ломать торговую логику time-based запретами.

Работы:

1. Оставить risk-triggered close только для safety событий:
	- stale feed,
	- exchange desync,
	- liquidation proximity,
	- extreme market disorder,
	- unrecoverable state mismatch.
2. Все обычные закрытия и reductions перевести в market-evaluated exit path.
3. Ввести inventory-aware risk для paired legs.
4. Разделить hard kill conditions и soft degradation conditions.
5. Зафиксировать, что operational deadman никогда не заменяет normal alpha exit.

Acceptance criteria:

- risk не содержит скрытой alpha логики;
- при нормальном рынке close приходит только из market evaluation;
- paired inventory limits и orphan protection проверяются отдельно.

## Фаза 5. Production Observability, Reliability, Recovery

**Цель:** система должна быть не только прибыльной, но и операционно жёсткой.

Работы:

1. Ввести runtime invariants:
	- no double fill application,
	- no orphan leg without explicit grace reason,
	- no close without causal explanation,
	- no impossible portfolio state.
2. Довести recovery до полного восстановления pair/hedge/origin state.
3. Завести обязательную telemetry цепочку по каждому трейду.
4. Добавить SLO/SLA на data freshness, fill convergence, reconciliation gap, pair unwind latency.
5. Сделать ежедневный production audit report автоматически генерируемым из telemetry.

Acceptance criteria:

- рестарт не ломает paired positions;
- любой инцидент объясняется из логов и событий без ручного гадания;
- есть автоматическая проверка инвариантов до и после каждой критической операции.

---

## 7. Что Именно Менять По Модулям

### `src/pipeline/hedge_pair_manager.cpp`

- убрать time gate `5'000'000'000LL`;
- перейти на EV/hysteresis gate;
- добавить pair action confidence;
- переиспользовать `exit_score_*` и fill/slippage model вместо wall-clock.

### `src/pipeline/exit_orchestrator.cpp`

- удержать его как canonical alpha exit engine;
- расширить decision object до pair-aware close drivers;
- добавить явный causal breakdown для `close primary`, `close hedge`, `close both`.

### `src/pipeline/trading_pipeline.cpp`

- сделать единый close arbiter;
- запретить legacy wording `time_exit` в production logic/comments;
- связать execution telemetry, pair telemetry и risk telemetry в один causal chain.

### `src/pipeline/market_reaction_engine.cpp`

- усилить probability heads для reversal/shock continuation;
- использовать их как вход в pair EV surface, а не только в single-position close.

### `src/execution_alpha/*`

- внедрить live fill hazard;
- сделать queue-aware price placement;
- связать predicted fill quality с urgency и hedge orchestration.

### `src/order_book/*`, `src/features/*`, `src/market_data/*`

- добавить depth events, OFI, queue depletion, refill speed, sweep detection;
- разделить exchange staleness и internal processing latency.

### `src/risk/*`

- оставить только safety logic;
- убрать любые скрытые alpha-заменители;
- добавить pair-inventory и exchange-desync invariants.

### `src/common/*`, `src/config/*`, `src/app/*`

- вычистить paper/shadow артефакты из runtime surface;
- оставить только production semantics;
- закрепить единый config contract для production-only системы.

---

## 8. Набор Метрик, Без Которого Бот Нельзя Считать Готовым

### Alpha / Pair Metrics

- continuation EV realized vs predicted;
- reversal capture rate;
- pair close efficiency;
- hedge win contribution;
- net PnL after fees/funding/slippage by strategy state.

### Execution Metrics

- maker share by regime;
- adverse selection after passive fill;
- expected vs realized slippage;
- cancel-to-repost success;
- p50/p95/p99 fill convergence latency.

### Risk Metrics

- orphan leg incidents;
- phantom position incidents;
- desync count WS vs REST vs portfolio;
- emergency flatten count;
- safety-trigger false positive rate.

### Reliability Metrics

- feed freshness SLA;
- private WS gap incidents;
- reconciliation mismatch half-life;
- restart recovery correctness;
- invariant breach count = 0.

---

## 9. Definition Of Done

Систему можно считать production-grade только когда одновременно выполнены все условия:

1. В executable runtime больше нет alpha close по elapsed time.
2. Pair/hedge engine принимает решения через joint EV, а не через fixed wait.
3. Все close paths объяснимы и восстановимы из event log.
4. Runtime surface production-only и не вводит в заблуждение ложными режимами.
5. Private WS + REST reconciliation + portfolio state сходятся детерминированно.
6. Новые тесты покрывают pair unwind, reverse transition, partial fills, orphan rollback, recovery after restart.
7. Телеметрия показывает устойчивую прибыльность после fees/funding/slippage именно от рыночной реакции и paired execution, а не от случайного плеча.

---

## 10. Первые Практические Шаги

Ниже порядок, в котором нужно начинать работу немедленно:

1. Удалить hard 5s gate из `hedge_pair_manager.cpp` и переписать unwind/reverse как market-evaluated decisions.
2. Закрыть открытые CRITICAL/HIGH дефекты аудита по execution, fill accounting, recovery, FSM и pair lifecycle.
3. Вычистить runtime surface и документацию до честного production-only состояния.
4. Внедрить pair EV surface и two-leg close coordinator.
5. Достроить queue-aware execution alpha и fill hazard model.
6. Поднять microstructure feature stack до OFI/microprice/refill/sweep level.
7. Только после этого усиливать predictive models и online adaptation.

---

## 11. Матрица Покрытия Аудита Этим Планом

Этот документ не заменяет `audit.md` как реестр дефектов. `audit.md` остаётся каноническим bug register на уровне конкретных ошибок, а этот план переводит его в программу работ по доменам, зависимостям, deliverables и release-gates.

| Блок аудита | Что реально сломано | Где это закрывается в плане | Результат на выходе |
|------------|----------------------|-----------------------------|---------------------|
| Архитектурный обзор | блокирующие hot-path sleeps, stub paths, silent failures, race на conn pool | Workstream 2, 3, 6, 10 | production runtime без ложных предпосылок и без явных safety-дыр |
| CRITICAL defects | execution, fills, pair lifecycle, config safety, persistence, recovery, scanner, cost attribution | Workstream 1-10 | снятие production blockers |
| HIGH defects | state ownership, hedge-mode semantics, recovery tolerance, registry/FSM races, stale market data, config/risk conflicts | Workstream 2-10 | стабилизация боевого контура |
| MEDIUM defects | calibration drift, mis-tuned thresholds, semantic mismatch между модулями, weak guards | Workstream 7-9 | корректная калибровка и согласованные контракты |
| LOW defects | formula cleanup, naming mismatch, statistical consistency, anti-pattern cleanup | Workstream 7-10 | финальная hardening-polish стадия |
| Межмодульные конфликты | pipeline vs regime, strategy vs exit, pair vs execution, risk vs portfolio, event journal vs storage | Workstream 1, 3, 4, 5, 6, 9 | единые контракты и отсутствие contradictory logic |
| Научная корректность | псевдо-байесовские обновления, weak entropy filter, outdated adaptation | Workstream 8 | algorithm layer, который реально выдерживает 2026-grade требования |
| production.yaml / config | риск-параметры, leverage economics, max loss mismatch, hidden config typos | Workstream 1, 9 | строгий production config contract |
| Сессии 11-38 | HTTP, registry leaks, metrics mistakes, deadman semantics, runtime truth gaps, pair time gate | Workstream 2-12 | полное покрытие поздних находок, а не только ранних 73 проблем |

Вывод простой: полный production-grade план обязан закрывать не только `pipeline/*`, а весь стек от сетевого ввода и journaling до causal telemetry и release discipline.

---

## 12. Правила Исполнения Программы

### 12.1 Базовые правила

1. `audit.md` используется как master defect register.
2. `plan.md` используется как master execution program.
3. Ни один production-path фикс не считается завершённым без regression test или replay proof.
4. Ни одна stub-функция не допускается в пути `signal -> order -> fill -> close -> recovery`.
5. Ни один новый alpha close trigger по времени не допускается.
6. Ни одна silent fallback ветка не допускается в config, persistence, recovery, fill accounting и pair lifecycle.
7. Ни один новый runtime mode кроме production не допускается.

### 12.2 Правила приоритизации

Работы делятся не по «красоте кода», а по реальному риску для денег и состояния:

1. Потеря или искажение денег.
2. Потеря или искажение состояния.
3. Ошибочные рыночные решения.
4. Ошибки observability, из-за которых невозможно понять инцидент.
5. Только после этого research-upgrades.

### 12.3 Правила интеграции изменений

Каждая крупная правка обязана приносить набор артефактов:

- изменённый код;
- unit/integration/replay test;
- обновлённый contract comment или design note;
- telemetry field или reason-code, если изменяется боевая логика;
- явный rollback plan, если модуль влияет на order/fill/recovery state.

### 12.4 Правило заморозки фич

До закрытия Workstream 1-6 запрещено:

- добавлять новые стратегии;
- расширять ML-фичи ради alpha uplift;
- заниматься cosmetic refactor ради стиля;
- менять execution heuristics без invariants и replay coverage.

Это важно: сейчас проект не находится в режиме «ускорения прибыли», он находится в режиме «устранения причин ложных решений и ложного состояния».

---

## 13. Workstream 0 — Program Governance И Архитектурная Заморозка

**Цель:** превратить аудит из списка находок в управляемую программу, где каждый фикс привязан к owner, артефактам и release-gate.

### Что сюда входит

- единая карта модулей и владельцев state;
- единый glossary для `intent`, `order`, `fill`, `position`, `pair`, `hedge`, `close reason`, `risk reason`, `safety exit`;
- freeze policy для risky modules;
- правило «one owner per state transition»;
- решение конфликтов между `strategy`, `pipeline`, `risk`, `portfolio`, `recovery`.

### Почему это обязательно

Аудит показывает системный паттерн: часть проблем возникает не потому, что отдельный if написан неверно, а потому что два соседних модуля думают, что именно они являются владельцем одного и того же решения или одного и того же state.

### Deliverables

1. Architecture ownership map:
	 - кто владеет order state;
	 - кто владеет fill application;
	 - кто владеет close decision;
	 - кто владеет pair lifecycle;
	 - кто владеет recovery truth.
2. State transition contract doc.
3. Error taxonomy doc.
4. Bitget semantics doc.
5. Release checklist template.

### Exit criteria

- в кодовой базе нет спорных owner-paths без явного контракта;
- каждая critical transition имеет designated owner;
- design notes синхронизированы с кодом, а не противоречат ему.

---

## 14. Workstream 1 — Runtime Truth И Production-Only Surface

**Цель:** привести весь runtime surface к одному честному контракту: production-only, release-only, no shadow semantics, no stale wording.

### Primary audit inputs

- BUG-S38-01 (`hedge_pair_manager.cpp`) — hard time gate в supposedly market-driven path;
- stale `paper/shadow/time_exit` wording в части public surface;
- config/runtime truth mismatch around modes;
- historical mismatch between comments and real execution behavior.

### Что надо сделать

1. Удалить все runtime-facing остатки paper/shadow semantics из production кода.
2. Удалить legacy terminology, которая может интерпретироваться как альтернативный боевой path.
3. Зафиксировать, что единственный допустимый режим runtime — production.
4. Зафиксировать, что любые replay/simulation/scenario harness живут только в tests/tools, но не в app bootstrap path.
5. Зафиксировать build truth:
	 - production запуск только из Release build;
	 - debug binaries не имеют права работать с live credentials;
	 - bootstrap/guard обязан это enforce-ить.

### Code-level backlog

- `src/common/*`: убрать stale enums/examples/messages, не соответствующие production-only семантике;
- `src/config/*`: строгий reject unknown keys, strict schema, no silent parse forgiveness;
- `src/app/*`: bootstrap/DI не должны тянуть альтернативные fake submitters в production path;
- `README.md`, `audit.md`, runtime docs: комментарии должны описывать реальное поведение, а не желаемое.

### Regression obligations

- config parser rejects any non-production mode deterministically;
- production guard rejects debug builds and invalid API hosts;
- public runtime interfaces не expose-ят legacy mode branches;
- grep-based policy checks в CI на `time_exit`, `paper`, `shadow` внутри production tree.

### Exit criteria

- в production tree не осталось живых runtime mode branches кроме production;
- documentation/comments не обещают market-driven behavior там, где его нет;
- release guard and config guard делают ложный запуск практически невозможным.

---

## 15. Workstream 2 — Exchange Connectivity, Submission Path И Hot-Path Latency

**Цель:** убрать любые structural delays и unsafe network semantics из боевого контура Bitget.

### Primary audit inputs

- CRITICAL-1: blocking `sleep_for` under `execute_mutex_`;
- CRITICAL-2: jitter sleep на всех ордерах, включая защитные;
- CRITICAL-21: race на `conn_pool_` в `bitget_rest_client.cpp`;
- CRITICAL-22: `PassiveLimit` forced to `Market`;
- HIGH-6: leverage search пропускает допустимые значения;
- поздние execution/HTTP findings по timeouts, body limits, synchronous handling.

### Что надо сделать

#### 15.1 Submission path separation

- отделить opening order path от protective order path;
- stop-loss, hedge, emergency flatten, close-both не имеют права ждать anti-fingerprinting jitter;
- fill confirmation polling выносится из горячего критического пути.

#### 15.2 Bitget transport hardening

- mutex/ownership discipline на connection pool;
- per-endpoint rate limiting;
- request timeout budget;
- explicit retry classification только для retryable errors;
- connection reuse без shared-state races;
- deterministic fallback from private WS to REST reconciliation.

#### 15.3 Order intent fidelity

- если планировщик выбрал passive/post-only, submitter не имеет права молча делать market override;
- `reduceOnly`, `positionSide`, click-backhand reversal и batch close должны быть explicit в intent model;
- leverage selection должна быть mathematically complete, а не heuristic-binary hack.

#### 15.4 Latency budgets

Нужно формально установить budgets:

- signal -> submit intent: p99 within budget;
- submit -> exchange ack: p99 within budget;
- fill -> portfolio apply: p99 within budget;
- kill/flatten path быстрее normal alpha path.

### Deliverables

1. Hardened Bitget transport adapter.
2. Protective-order fast lane.
3. Submission policy doc for maker/taker/reduce-only/reversal.
4. Latency telemetry dashboard.

### Tests and drills

- concurrent REST load on pool;
- high-volatility stop path without jitter;
- passive order remains passive through planner -> submitter boundary;
- retry matrix by error code;
- live-like soak test with artificial WS gaps.

### Exit criteria

- no blocking sleep inside critical execution locks;
- no unsafe race on transport shared state;
- no silent order style mutation;
- protective path latency is materially lower than entry path latency.

---

## 16. Workstream 3 — Order Lifecycle, Registry, FSM, Cancel Control

**Цель:** order state должен быть линейным, идемпотентным и recoverable, без deadlock и без divergent interpretations between modules.

### Primary audit inputs

- CRITICAL-22c: `order_fsm.cpp` without proper synchronization;
- BUG-NEW-05/06/13: `order_registry.cpp` leak, callback under mutex, wrong fill_applied reset semantics;
- BUG-NEW-12: `cancel_manager.cpp` does not detect stuck `CancelPending`;
- multiple WS/REST convergence findings from later sessions;
- intermodule conflict C7 and related order-state mismatches.

### Что надо сделать

#### 16.1 Canonical FSM

Нужно закрепить одну canonical transition table:

- `New -> PendingAck -> Open/Rejected/PartiallyFilled/Filled/CancelPending`;
- `PartiallyFilled -> Filled/CancelPending/Cancelled/Expired/UnknownRecovery`;
- `CancelPending -> Cancelled/PartiallyFilled/Filled/UnknownRecovery`.

Никакой соседний модуль не может invent-ить собственные terminal/intermediate states.

#### 16.2 Registry invariants

- `trade_id` dedup bounded in memory and time;
- callbacks outside mutex;
- `fill_applied` tied to internal order identity, not ambiguous exchange IDs;
- recovery transition APIs не должны оставлять stale applied flags;
- registry обязуется быть single convergence point for WS + REST fill paths.

#### 16.3 Cancel path discipline

- explicit stuck-cancel detection;
- timeout -> resync -> force-reconcile, not silent limbo;
- partial-fill after cancel request treated as valid state, not exception;
- cancel_remaining semantics must be explicit and enforced.

### Deliverables

1. FSM spec and code parity check.
2. Registry invariant checks.
3. Cancel reconciliation policy.
4. Transition reason taxonomy.

### Tests

- concurrent transition stress tests;
- WS fill arrives after cancel request;
- REST reconciliation arrives after WS partial fill;
- duplicate trade IDs over long run;
- registry callback attempting re-entry.

### Exit criteria

- no undefined transitions in production path;
- no deadlock from callbacks;
- no fill double-application under WS+REST race;
- stuck cancel situations are observable and auto-recovered.

---

## 17. Workstream 4 — Fill Processing, PnL Accounting, Cost Attribution, Portfolio Truth

**Цель:** деньги, комиссии, funding и realized/unrealized PnL должны совпадать между exchange facts, portfolio state, telemetry и recovery.

### Primary audit inputs

- CRITICAL-3: fees not journaled;
- CRITICAL-15: funding sign error;
- CRITICAL-4: recovery assuming full fill;
- HIGH-10: portfolio update return value ignored;
- HIGH-2: hedge-mode close without explicit side;
- LOW-4: portfolio accounting ambiguity;
- intermodule conflict C10.

### Что надо сделать

#### 17.1 Fee and funding truth

- every fee must become an event;
- funding credit/debit sign must match exchange semantics;
- attribution engine and portfolio engine must share one cost convention;
- telemetry must separate gross pnl, fee pnl, funding pnl, slippage pnl.

#### 17.2 Fill truth

- actual filled qty only;
- `baseVolume`, `accBaseVolume`, `fillPrice`, `priceAvg` используются по correct Bitget semantics;
- no inferred 100% fill from average price presence;
- partial fill path must update state incrementally, not overwrite notional/size.

#### 17.3 Hedge-mode portfolio correctness

- close APIs must always specify leg side;
- no ambiguous flatten of wrong leg;
- long and short legs have explicit identifiers across portfolio, pipeline and recovery.

#### 17.4 Causal accounting chain

Нужна полная цепочка:

`exchange event -> normalized fill -> registry dedup -> portfolio apply -> fee/funding apply -> journal append -> telemetry emit -> recovery reproducibility`

### Deliverables

1. Unified cost convention spec.
2. Fill normalization adapter doc for Bitget fields.
3. Portfolio accounting invariants.
4. Cost attribution dashboard with per-component breakdown.

### Tests

- partial fill followed by crash/recovery;
- funding positive credit vs negative debit cases;
- hedge-mode close for both legs;
- duplicate fill event ignored exactly once;
- journal replay reproduces same realized PnL.

### Exit criteria

- accounting replay equals live portfolio state;
- funding and fees never flip sign incorrectly;
- hedge-mode close never touches wrong leg;
- portfolio and attribution totals reconcile exactly.

---

## 18. Workstream 5 — Pair/Hedge Engine, Dual-Leg Coordination, Market-Driven Exit

**Цель:** превратить текущую pair/hedge подсистему из набора конфликтующих эвристик в единый economically coherent engine.

### Primary audit inputs

- BUG-S38-01: hard 5s gate in `hedge_pair_manager.cpp`;
- HIGH-7: hedge state advances before fill confirmation;
- CRITICAL-16/17: `dual_leg_manager.cpp` corrupts partial fill state and clears leg too early;
- CRITICAL-18: stub in `pair_execution_coordinator.cpp`;
- CRITICAL-19: fee arithmetic wrong in `pair_lifecycle_engine.cpp`;
- CRITICAL-20: funding scale wrong in `market_reaction_engine.cpp`;
- intermodule conflicts C3, C8.

### Что надо сделать

#### 18.1 Убрать time-coupled alpha exit

- полностью удалить `hedge_hold_ns > 5s` и любые аналогичные hard gates из alpha path;
- допускается только safety time guard in operational layer;
- pair engine must act on EV and state, not clock elapsed.

#### 18.2 Pair EV engine

Для каждой пары вычислять:

- `EV(keep both)`
- `EV(close primary)`
- `EV(close hedge)`
- `EV(close both)`
- `EV(reverse)`
- `EV(increase hedge)`
- `EV(reduce hedge)`

С компонентами:

- fill probability per leg;
- expected slippage per leg;
- net spread capture;
- funding carry;
- regime transition probability;
- liquidation/slippage stress penalty;
- partial fill orphan penalty.

#### 18.3 Two-leg coordinator

- no stub methods;
- grouped intent for two-leg actions;
- explicit rollback/repair when only one leg fills;
- no optimistic state advance before fill confirmation;
- no leg cleanup before close success or compensating path.

#### 18.4 Pair lifecycle truth

- fee arithmetic fixed;
- state machine explicit for `PrimaryOnly`, `PrimaryPlusHedge`, `ProfitLockPair`, `ReverseTransition`, `AsymmetricUnwind`, `EmergencyFlatten`;
- each transition emits causal reason and expected economics.

### Deliverables

1. Pair EV spec.
2. Pair lifecycle state machine doc.
3. Two-leg coordinator implementation.
4. Pair PnL attribution pipeline.

### Tests

- profitable asymmetric unwind before 5 seconds;
- reverse transition with partial fill on first leg;
- hedge open rejected after intent creation;
- close_single_leg fully implemented and replay-tested;
- recovery reconstructs pair state after crash mid-transition.

### Exit criteria

- no pair alpha decision depends on elapsed time;
- no optimistic hedge-opened state before fill;
- no orphan leg without explicit recovery path;
- every pair transition has deterministic accounting and telemetry.

---

## 19. Workstream 6 — Persistence, WAL, Snapshots, Event Sourcing, Recovery

**Цель:** после любого краша система обязана восстановить то же самое состояние, а не приблизительное или «похоже правильное».

### Primary audit inputs

- CRITICAL-5: WAL corruption counter ignored;
- CRITICAL-6: snapshot/store ID monotonicity race;
- HIGH-8: duplicate sequence IDs in postgres adapter;
- HIGH-9: one bad journal entry halts recovery;
- MEDIUM-11: sequence counter resets after crash;
- CRITICAL-4: recovery assumes 100% fill;
- multiple late-session findings around reconciliation and idempotency.

### Что надо сделать

#### 19.1 Event-sourcing hardening

- monotonically increasing IDs guaranteed by storage, not by wishful in-memory counters;
- append-only journal semantics with corruption detection and escalation;
- snapshot IDs and journal IDs must be globally monotonic where required by recovery logic.

#### 19.2 Corruption handling policy

- corruption is not just a counter; it is an operational incident;
- define threshold, alert, quarantine and fallback behavior;
- recovery may skip single corrupt entries only if invariant checks remain satisfied and operator telemetry captures the loss window.

#### 19.3 Recovery truth

- restore actual filled quantities;
- rebuild pair, hedge and order FSM state from journal + exchange reconciliation;
- if journal and exchange disagree, deterministic conflict resolution must exist.

#### 19.4 Replay determinism

- same journal must reproduce same resulting state;
- replay result must equal live state for capital, open legs, order states and accumulated costs.

### Deliverables

1. Recovery spec with precedence rules.
2. Corruption policy and incident severities.
3. Snapshot/journal ID model.
4. Replay equivalence test suite.

### Tests and drills

- corrupted WAL entry in middle of session;
- duplicate sequence ID attempt;
- crash after partial fill but before fee journal append;
- restart during reverse transition;
- exchange says partial fill while journal only has order ack.

### Exit criteria

- crash recovery does not overstate size or capital;
- corruption is surfaced, not silently eaten;
- sequence IDs remain monotonic across restarts;
- journal replay and live state reconcile exactly or fail loudly.

---

## 20. Workstream 7 — Market Data, Normalization, Features, Scanner

**Цель:** на вход alpha, risk и execution должны приходить корректные, свежие и numerically safe данные.

### Primary audit inputs

- CRITICAL-10: NaN correlation in scanner;
- CRITICAL-14: unknown interval kills live signals;
- HIGH-17/18: scanner torn reads and stale orderbook usage;
- HIGH-21/22: NaN feature handling and case-sensitive symbol mismatch;
- MEDIUM-18/19: timeout semantics and NaN funding rate in pair ranker;
- LOW-2: `market_data_age_ns` semantics mismatch.

### Что надо сделать

#### 20.1 Freshness discipline

- freshness checked after fetch, not only before fetch;
- age semantics split into:
	- ticker age,
	- book age,
	- candle age,
	- internal processing lag.

#### 20.2 Numerical safety

- `isfinite()` guards for all external feature inputs;
- NaN in correlations, funding, volatility, imbalance or drift scores must never degrade into random trading direction or silent fallback;
- unknown interval or symbol case mismatch must fail safe but observable.

#### 20.3 Feature modernization

- add OFI, microprice drift, queue depletion, refill speed, sweep burst tags, local shock flags;
- feature store must declare validity explicitly per feature;
- execution and pair engines consume only valid fields.

#### 20.4 Scanner truth

- scanner timeframe economics must be aligned with runtime entry economics;
- diversification constraints cannot be bypassed by NaN/Inf;
- stale orderbook rejection must be explicit.

### Deliverables

1. Market data freshness contract.
2. Feature validity contract.
3. Numeric-safety checklist.
4. Scanner decision trace with rejection reasons.

### Tests

- NaN/Inf injection across feature snapshots;
- stale orderbook and late candle arrivals;
- unknown timeframe normalization;
- symbol casing mismatch across Bitget payloads;
- scanner timeout mid-rotation.

### Exit criteria

- no NaN propagates into trade direction or score;
- freshness semantics are unambiguous and separately logged;
- scanner and runtime use economically aligned thresholds;
- feature validity is explicit and enforced.

---

## 21. Workstream 8 — Strategy, Decisioning, Regime, Uncertainty, ML

**Цель:** decision layer должен быть одновременно scientifically defensible, calibrated и contract-safe относительно downstream execution/risk modules.

### Primary audit inputs

- HIGH-4: `exit_signal_sent_` not reset on all close paths;
- MEDIUM-2/3/20: HTF gates and conviction priors inconsistent;
- MEDIUM-8/10: time decay and regime dwell issues;
- MEDIUM-1: pseudo-bayesian update;
- MEDIUM-4: weak entropy filter;
- MEDIUM-9: uncertainty locking issue;
- HIGH-3, LOW-5: Thompson sampler bias/no temporal decay;
- section 7 of audit on scientific validity and 2024+ replacements.

### Что надо сделать

#### 21.1 Contract alignment

- strategy engine must emit signals; exit ownership belongs to canonical exit layer;
- no hidden local exit side-effects that bypass orchestrator;
- close-signal latches reset on every legitimate terminal path.

#### 21.2 Calibration cleanup

- harmonize prior mean, min conviction threshold, risk gates and opportunity cost thresholds;
- drift, alpha-decay and uncertainty modules must agree on meaning of degraded confidence.

#### 21.3 Scientific corrections

- replace pseudo-bayesian update with real posterior update or explicitly rename/remove the adapter;
- permutation entropy upgrade for noise filter if retained;
- contextual bandit layer only after state/control plane is correct;
- regime engine dwell/transition rules made explicit and testable.

#### 21.4 Probability outputs

- all important action layers move toward calibrated probabilities, not opaque scores;
- add `P(continue)`, `P(reversal)`, `P(shock)`, `P(fill_passive)` with calibration tracking.

### Deliverables

1. Decision semantics doc.
2. Calibration spec.
3. Model replacement or rename plan for pseudo-bayesian components.
4. Uncertainty aggregation contract.

### Tests

- exit-signal reset across stop-loss/trailing/emergency exits;
- prior/threshold consistency tests;
- regime dwell regression tests;
- uncertainty engine concurrency tests;
- calibration score monitoring on replay datasets.

### Exit criteria

- no decision module quietly bypasses exit owner boundaries;
- conviction thresholds and priors are coherent;
- pseudo-bayesian naming mismatch eliminated;
- uncertainty and regime outputs are deterministic and reproducible.

---

## 22. Workstream 9 — Risk, Portfolio Allocation, Leverage, Config Economics

**Цель:** risk должен реально ограничивать потери, а не смешивать разные шкалы и не создавать mathematically impossible operating envelope.

### Primary audit inputs

- HIGH-5: MAE vs portfolio drawdown mixed in kill switch;
- MEDIUM-7: risky config values in `production.yaml`;
- BUG-NEW-11: invalid leverage-stop combinations only warn;
- existing allocator/risk fixes in repo memory that still require systemic completion;
- intermodule conflicts C4 and C5.

### Что надо сделать

#### 22.1 Separate risk domains

- per-trade stop risk;
- intra-position MAE risk;
- portfolio intraday drawdown;
- strategy-level drawdown;
- pair inventory mismatch risk;
- exchange liquidation proximity.

Эти домены не должны использовать одну и ту же переменную как будто они взаимозаменяемы.

#### 22.2 Config contract hardening

- unknown keys rejected;
- impossible economics rejected as error, not warning;
- leverage x stop distance x min notional x account size cross-validated;
- config comments and defaults reflect real exchange constraints.

#### 22.3 Portfolio allocator truth

- no temporary violation of hard limits;
- stop-implied capital risk cap enforced before exchange min-notional uplift;
- pair and hedge notional budgets explicit;
- correlated exposure budgets enforced across basket.

#### 22.4 Safety-only deadman

- operational deadman may react only when degradation signals confirm infrastructure risk;
- it must never become a hidden alpha exit replacement.

### Deliverables

1. Risk domain model.
2. Config economics validator.
3. Portfolio and pair budget spec.
4. Safety-vs-alpha exit taxonomy.

### Tests

- impossible config rejected;
- micro-account min-notional edge cases;
- pair inventory risk breaches;
- intraday drawdown computed off current equity, not stale baseline;
- deadman only fires with degradation evidence.

### Exit criteria

- no mixed-threshold kill switch logic;
- impossible production configs cannot boot;
- allocator never upsizes into impossible loss;
- safety exits are distinguishable from alpha exits in logs and telemetry.

---

## 23. Workstream 10 — Resilience, Supervisor, Security, Self-Diagnosis, HTTP Surface

**Цель:** инфраструктурный слой не должен сам создавать аварии, гонки и security leaks.

### Primary audit inputs

- CRITICAL-8: circuit breaker dead code in half-open;
- CRITICAL-9: self-diagnosis JSON escaping bug;
- HIGH-11/12/13: concurrency issues in defense/circuit breaker;
- HIGH-15/16 and MEDIUM-15: supervisor atomicity/index safety;
- BUG-NEW-02/03/04: HTTP server UB, no timeout, no body limit;
- BUG-NEW-10: credentials not zeroed after use.

### Что надо сделать

#### 23.1 Resilience primitives

- circuit breaker must actually transition through half-open;
- supervisor state transitions atomic and replayable;
- defensive modules must not mutate hysteresis/cleanup state outside lock discipline.

#### 23.2 Security hardening

- secrets in memory zeroized where possible;
- redaction covers JSON, env-style and header-style exposures;
- production host allowlist enforced;
- crash dumps and self-diagnosis outputs must not leak secrets.

#### 23.3 HTTP surface

- async or bounded handling model;
- connection and read timeouts;
- max body size;
- no unsafe acceptor sharing;
- explicit auth and method guards where applicable.

#### 23.4 Self-diagnosis truth

- diagnostics JSON produced via safe serializer;
- IDs monotonic and concurrency-safe;
- health reports distinguish market-health, infra-health, risk-health and state-health.

### Deliverables

1. Resilience primitive contract.
2. Secure diagnostics and redaction policy.
3. HTTP hardening checklist.
4. Incident taxonomy for supervisor/degraded mode transitions.

### Tests

- concurrent degraded-mode toggles;
- half-open circuit breaker recovery;
- malformed HTTP body flood;
- secret leak scanning on logs and diagnostics;
- supervisor shutdown races.

### Exit criteria

- resilience modules no longer contain known atomicity/race defects;
- self-diagnosis output is safe and valid JSON;
- HTTP surface is bounded and non-blocking enough for operational use;
- credentials do not remain trivially exposed in memory/log artifacts.

---

## 24. Workstream 11 — Observability, Telemetry, Invariants, Incident Response

**Цель:** любой инцидент, любое решение и любой unexpected PnL outcome должны быть объяснимы без ручного гадания по scattered logs.

### Что надо сделать

#### 24.1 Causal telemetry chain

Для каждого trade lifecycle хранить:

- signal snapshot;
- decision explanation;
- risk verdict;
- execution intent;
- exchange ack/fill details;
- portfolio delta;
- pair state delta;
- close explanation;
- recovery reconciliation notes.

#### 24.2 Invariant engine

Нужны runtime checks:

- no double fill application;
- no terminal order re-open;
- no orphan pair leg beyond grace window;
- no close without causal reason;
- no negative fee/funding sign inconsistency;
- no impossible exposure after reconciliation.

#### 24.3 Operational dashboards

- order/fill convergence;
- WS gap frequency;
- reconciliation mismatch age;
- pair reverse latency;
- emergency flatten count;
- safety exit false positive rate;
- capital drift between portfolio and exchange.

#### 24.4 Daily audit automation

- generate daily production health summary;
- surface invariant breaches and near-misses;
- include regression against previous day;
- use the same taxonomy as `audit.md`, чтобы накопление дефектов не пряталось в «общих» логах.

### Deliverables

1. Causal telemetry schema.
2. Invariant checker module.
3. Daily audit report generator.
4. Incident review template.

### Exit criteria

- every close has explainable primary driver and supporting data;
- incident triage can start from telemetry alone;
- invariant breaches are impossible to miss operationally;
- production review becomes routine, not forensic archaeology.

---

## 25. Workstream 12 — Test Program, Replay Program, Fault Injection, Verification Harness

**Цель:** production-grade статус подтверждается не «у нас вроде работает», а reproducible proof under stress and recovery.

### Что надо сделать

#### 25.1 Unit coverage by defect family

- concurrency and atomicity tests;
- FSM transition tests;
- fee/funding accounting tests;
- pair reverse/unwind tests;
- config validation tests;
- numerical safety tests with NaN/Inf.

#### 25.2 Integration coverage

- Bitget order mapping;
- WS + REST reconciliation;
- recovery after crash with open orders and partial fills;
- HTTP and supervisor control paths;
- pair lifecycle end-to-end with two legs.

#### 25.3 Deterministic replay

- recorded event streams for:
	- normal fill path;
	- partial fill;
	- duplicate fill;
	- cancel-after-partial;
	- reverse transition;
	- stale feed / degraded mode;
	- WAL corruption and restart.

#### 25.4 Fault injection

- delayed REST;
- dropped WS fills;
- duplicated WS fills;
- corrupt journal record;
- exchange rejection storm;
- stale orderbook;
- transport reconnect storm.

### Verification rule

Каждый critical/high defect family из аудита должен иметь хотя бы один of the following:

- direct regression test;
- deterministic replay scenario;
- fault-injection proof;
- benchmark/telemetry gate.

### Exit criteria

- no P0/P1 fix ships without executable proof;
- replay scenarios reproduce known historical failures and show corrected behavior;
- fault injection cannot push the bot into silent broken state.

---

## 26. Полная Программа Работ По Фазам

Ниже уже не high-level roadmap, а dependency-driven execution sequence. Логика такая: сначала truth of state and money, потом pair/economic control, потом model upgrades.

### Фаза A — Runtime Truth И Production Safety Lockdown

**Состав:** Workstream 0, 1, базовые части 2 и 10.

**Обязательные задачи:**

1. Удалить remaining time-based alpha close gates.
2. Синхронизировать production-only contract across code/docs/config.
3. Enforce release-only live runtime.
4. Исправить transport hot-path sleeps/jitter on protective orders.
5. Закрыть HTTP/circuit-breaker/supervisor defects, которые могут ломать боевой контур сами по себе.

**Выход из фазы:**

- проект честно описывает сам себя;
- runtime не врёт про market-driven exits;
- production process не запускается в ложной конфигурации.

### Фаза B — Execution Control Plane Stabilization

**Состав:** Workstream 2, 3, часть 4.

**Обязательные задачи:**

1. Убрать planner/submitter semantic drift.
2. Починить FSM, registry, cancel path, dedup.
3. Зафиксировать single convergence point for fills.
4. Починить basic fee/fill accounting truth.

**Выход из фазы:**

- ордерный контур идемпотентен;
- fill application deterministic;
- protective execution больше не блокируется собственной инфраструктурой.

### Фаза C — Money Truth And Recovery Truth

**Состав:** Workstream 4, 6.

**Обязательные задачи:**

1. Исправить funding/fee attribution.
2. Journal every monetary side effect.
3. Починить snapshot/journal ID monotonicity.
4. Сделать crash recovery faithful to partial fills and open state.

**Выход из фазы:**

- деньги и state сходятся после рестарта;
- журнал достаточен для forensic reconstruction.

### Фаза D — Pair/Hedge Engine Rebuild

**Состав:** Workstream 5.

**Обязательные задачи:**

1. Удалить optimistic state transitions.
2. Реализовать two-leg coordinator fully.
3. Внедрить pair EV surface.
4. Стабилизировать reverse transition and orphan handling.

**Выход из фазы:**

- pair engine closes/reverses by economics, not by wait time;
- two-leg actions recover correctly after partial execution.

### Фаза E — Input Quality, Feature Truth, Risk Contract Harmonization

**Состав:** Workstream 7, 8, 9.

**Обязательные задачи:**

1. Починить freshness semantics and numeric guards.
2. Согласовать conviction, uncertainty, opportunity cost and risk thresholds.
3. Сделать config economics strict and mathematically valid.
4. Починить regime/drift/decision contract mismatches.

**Выход из фазы:**

- upstream signals numerically trustworthy;
- downstream thresholds coherent;
- config cannot encode self-contradictory risk model.

### Фаза F — Observability And Reliability Closure

**Состав:** Workstream 11, 12.

**Обязательные задачи:**

1. Внедрить causal telemetry schema.
2. Внедрить invariant engine.
3. Закрыть deterministic replay and fault injection program.
4. Настроить daily production audit artifacts.

**Выход из фазы:**

- состояние объяснимо;
- incident response стандартизирован;
- release decision основан на telemetry and tests, not gut feel.

### Фаза G — 2026-Grade Model/Execution Upgrades

**Состав:** advanced parts of Workstream 7 and 8.

**Обязательные задачи:**

1. Add OFI/microprice/refill/sweep feature layer.
2. Add calibrated short-horizon probability heads.
3. Add contextual bandit for execution/pair action.
4. Add lightweight sequence model only after infra truth is closed.

**Выход из фазы:**

- upgrades build on trustworthy substrate;
- alpha improvements do not reintroduce state/control ambiguity.

---

## 27. Module-By-Module Полный Checklist

Ниже список модульных направлений, который должен использоваться как execution checklist поверх `audit.md`.

| Модуль/кластер | Что должно быть сделано |
|----------------|-------------------------|
| `src/app/*` | bootstrap truth, release-only live guard, credentials lifecycle, HTTP boundedness |
| `src/common/*` | убрать stale runtime semantics, унифицировать error/result contracts |
| `src/config/*` | strict schema, unknown-key rejection, economic validation, one canonical production contract |
| `src/exchange/bitget/*` | pool synchronization, rate limiting, retry taxonomy, correct hedge-mode semantics, no blocking protective path |
| `src/execution/*` | async confirmation, planner fidelity, cancel manager, order quality telemetry, TWAP idempotency |
| `src/execution/orders/*` | bounded dedup memory, callbacks outside lock, recovery-safe transitions |
| `src/execution/fills/*` | fee journaling, partial fill truth, Bitget field normalization, portfolio update hard checks |
| `src/execution_alpha/*` | EV-correct maker/taker routing, live fill hazard, no stale EIS after overrides |
| `src/pipeline/*` | canonical exit ownership, pair EV, two-leg coordination, no elapsed-time alpha close |
| `src/portfolio/*` | hedge-side explicit close, accounting invariants, exposure truth |
| `src/portfolio_allocator/*` | hard limits before uplift, pair inventory budgets, correlated exposure enforcement |
| `src/risk/*` | split risk domains, safety-vs-alpha taxonomy, pair inventory risk, deadman semantics |
| `src/recovery/*` | corrupt-entry tolerance, actual fill restoration, pair-aware restart logic |
| `src/persistence/*` | monotonic IDs, WAL corruption policy, replay determinism, snapshot consistency |
| `src/scanner/*` | freshness, numeric guards, diversification truth, funding and correlation correctness |
| `src/market_data/*` | source freshness separation, WS/REST timeline consistency |
| `src/features/*` | explicit validity, OFI/microprice/refill/sweep integration, no semantic age confusion |
| `src/normalizer/*` | timeframe mapping, symbol normalization, safe fallbacks |
| `src/strategy/*` | signal ownership only, no local hidden exit side-effects |
| `src/decision/*` | coherent time decay, contract-safe aggregation |
| `src/regime/*` | minimum dwell, validated transitions, no premature regime flips |
| `src/uncertainty/*` | lock-safe aggregation, calibration-linked uncertainty |
| `src/ml/*` | real posterior/calibration logic, contextual bandit rollout only after infra stabilization |
| `src/adversarial_defense/*` | lock discipline, asymmetric toxic-flow math, valid cleanup semantics |
| `src/resilience/*` | circuit breaker correctness, degraded-mode transitions, reconnect/retry discipline |
| `src/supervisor/*` | atomic correctness, no lost shutdown/degraded signals, safe watcher deregistration |
| `src/security/*` | zeroization, redaction, production guard, host allowlist |
| `src/self_diagnosis/*` | valid JSON serialization, monotonic IDs, safe diagnostics |
| `src/telemetry/*` | causal chain, invariant counters, daily audit reports |
| `tests/*` | add regression, integration, replay, fault-injection, recovery and pair lifecycle coverage |

---

## 28. Required Engineering Artifacts До Production-Grade Sign-Off

Проект не должен считаться готовым только по зелёным тестам. Нужен комплект артефактов.

### 28.1 Design artifacts

- architecture ownership map;
- order/FSM state chart;
- pair lifecycle state chart;
- recovery precedence document;
- risk domain taxonomy;
- Bitget field semantics note.

### 28.2 Operational artifacts

- runbook: normal startup;
- runbook: degraded mode;
- runbook: reconciliation mismatch;
- runbook: orphan leg incident;
- runbook: WAL corruption;
- runbook: exchange disconnect / stale private WS.

### 28.3 Verification artifacts

- regression suite results;
- replay pack results;
- fault injection report;
- latency benchmark report;
- accounting reconciliation report;
- pair lifecycle drill report.

### 28.4 Observability artifacts

- dashboard definitions;
- metric dictionary;
- invariant dictionary;
- daily audit template;
- incident postmortem template.

---

## 29. Quantitative Release Gates

Без этих gates никакой «production-grade» статус не присваивается.

### Gate 1 — Build And Contract Gate

- release build only;
- config schema strict;
- no runtime stubs in critical path;
- no grep hits for forbidden runtime semantics in production tree.

### Gate 2 — State Integrity Gate

- no duplicate fill application in stress test;
- order FSM survives concurrent events;
- journal replay reproduces state;
- recovery after crash with partial fill is exact.

### Gate 3 — Money Integrity Gate

- fees and funding reconcile between exchange facts and portfolio;
- no sign inversion in attribution;
- hedge-mode close cannot close wrong leg;
- pair PnL components sum exactly.

### Gate 4 — Pair Engine Gate

- profitable unwind/reverse works before 5 seconds when economics say yes;
- no orphan legs after tested partial-fill scenarios;
- every pair transition has explicit causal reason;
- two-leg coordinator has no stubs.

### Gate 5 — Operational Reliability Gate

- stale WS / REST fallback handled deterministically;
- circuit breaker and supervisor transitions verified under concurrency;
- invariant breach count remains zero under soak target;
- incident drill artifacts exist.

### Gate 6 — Data/Model Gate

- no NaN/Inf can reach decision path unguarded;
- freshness semantics logged and enforced;
- calibration metrics available for critical probability outputs;
- config economics validated against account/exchange constraints.

### Gate 7 — Soak Gate

- multi-session soak without unexplained capital/state drift;
- no unresolved CRITICAL/HIGH issues in active production path;
- daily audit report remains clean enough for go/no-go decision.

---

## 30. Capital Ramp Plan После Технической Готовности

Production-grade не означает «сразу торговать крупно». Нужен disciplined ramp.

### Stage 0 — Zero-risk verification

- local tests;
- deterministic replay;
- fault injection;
- log/invariant review.

### Stage 1 — Minimal live capital

- minimum viable real notional consistent with exchange rules;
- verify order mapping, fills, costs, recovery and telemetry under live Bitget behavior;
- no model expansion yet.

### Stage 2 — Controlled micro-account soak

- multiple sessions;
- check pair transitions, funding attribution, stale feed behavior, reconnect behavior;
- every anomaly becomes either a fix or a documented false positive with proof.

### Stage 3 — Gradual scale-up

- scale only if all prior gates stay green;
- scale decision based on operational stability, not isolated profit bursts;
- any unexplained divergence freezes scale-up.

### Stage 4 — Full production status

- only after technical and operational gates stay green over enough sessions;
- still retains rollback path and daily audit discipline.

---

## 31. Working Rules Для Команды Во Время Исполнения Плана

1. Не объединять в один PR unrelated fixes across state, money and alpha layers.
2. Не чинить accounting и одновременно менять strategy thresholds без отдельной telemetry attribution.
3. Каждый PR должен явно говорить, какой defect family он закрывает.
4. Каждый PR, влияющий на `signal -> order -> fill -> position -> close`, обязан иметь replay or integration evidence.
5. Каждый PR, влияющий на pair/hedge state, обязан иметь partial-fill and recovery scenario.
6. Каждый PR, влияющий на risk thresholds или config economics, обязан иметь config validation tests.
7. Нельзя добавлять «temporary» silent fallback в production path.

---

## 32. Полный Definition Of Done Для Проекта

Проект можно считать действительно production-grade только если одновременно выполнено всё ниже.

### 32.1 Runtime truth

- production-only runtime contract enforced in code and config;
- documentation matches reality;
- no elapsed-time alpha exits remain.

### 32.2 State truth

- order/fill/portfolio/pair state deterministic under concurrency and restart;
- no stub methods in critical lifecycle paths;
- replay equals live state reconstruction.

### 32.3 Money truth

- fees, funding, slippage and realized PnL reconcile exactly;
- pair attribution is explainable and sums correctly;
- no capital drift after crash/recovery.

### 32.4 Market decision truth

- exits and reversals driven by market/economic evaluation;
- pair engine operates via joint EV;
- safety exits clearly separated from alpha exits.

### 32.5 Operational truth

- transport, supervisor, circuit breaker and diagnostics stable under stress;
- telemetry is causal and complete;
- invariant breaches are zero or immediately blocking.

### 32.6 Verification truth

- regression/integration/replay/fault-injection evidence exists for all critical families;
- release gates are green;
- soak sessions do not produce unexplained divergence.

### 32.7 Upgrade readiness

- only after all above is green can the project legitimately invest in stronger model stack, contextual bandits, short-horizon sequence models and more aggressive execution alpha.

---

## 33. Immediate Execution Order Из Этого Полного Плана

Если двигаться без распыления, порядок должен быть именно таким:

1. Убрать оставшиеся ложные runtime утверждения и hard time-based alpha exits.
2. Починить hot-path execution, transport safety, planner/submitter fidelity и FSM/registry convergence.
3. Зафиксировать money truth: fills, fees, funding, side-specific close, journaling.
4. Перестроить persistence/recovery до deterministic replayable state.
5. Только потом полноценно пересобрать pair/hedge engine на EV surface и two-leg coordination.
6. Затем harmonize market data, scanner, feature validity, risk/config economics and strategy/regime contracts.
7. Затем закрыть observability/invariants and complete verification harness.
8. И только после этого усиливать predictive stack и масштабировать live capital.

Это и есть полный путь доведения проекта до production-grade на основании всего аудита, а не только его верхнего summary слоя.
