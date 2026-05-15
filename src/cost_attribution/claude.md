# `src/cost_attribution` — Атрибуция cost-компонентов

## Назначение

Декомпозиция реализованных издержек (на закрытие сделки) на компоненты: spread, taker/maker fees, slippage, queue cost, adversarial cost, opportunity cost. Используется для post-mortem анализа стратегий.

## Границы ответственности

* `compute_attribution(trade_data, entry_features, exit_features, market_state) → CostAttribution`.
* Сравнение realized vs expected costs (от `ExecutionAlpha`).

## Публичные интерфейсы

* `class CostAttributionEngine`:
  * `compute(...)` — главный метод (точная сигнатура — в `cost_attribution_engine.hpp`).
* `CostAttribution` DTO — компоненты + total.

## Внутренние компоненты

* `cost_attribution_engine.hpp/cpp`.
* `cost_attribution_types.hpp` — DTO.

## Зависимости

* `features`, `execution`, `portfolio`, `execution_alpha`.

## Потоки данных

`TradingPipeline` после `notify_position_closed` → собирает данные сделки → `cost_attribution_.compute(...)` → telemetry / persistence.

## Race conditions

Stateless. Безопасно для конкурентного вызова.

## Ошибки проектирования

* **D-ca-1 (LOW).** Атрибуция post-fact; нет режима ex-ante (предсказание). См. `execution_alpha` для ex-ante.

## Контракты

### `compute(...) → CostAttribution`

* **Pre.** Все snapshot'ы валидные.
* **Post.** Сумма компонентов = total cost.

## Производственные риски

* **R-ca-1.** Если компонент рассчитан неточно (например, slippage по mid-price вместо by-fill-price) — искажает analytics.

## Рекомендации

1. Тест: атрибуция на synthetic-сделках с known-cost-распределениями.
2. Дашборд: `cost_attribution_avg{component}` per стратегия.
3. Реализовать ex-ante версию (для тестирования предсказаний `ExecutionAlpha`).
