# `src/buffers` — Кольцевые буферы

## Назначение

Шаблонные fixed-size кольцевые буферы для хранения свечей и трейдов. Используются `FeatureEngine` для удержания скользящего окна данных.

## Границы ответственности

* `RingBuffer<T, N>` — generic FIFO с фиксированной ёмкостью.
* `CandleBuffer<N>` — типизированный для `NormalizedCandle`.
* `TradeBuffer<N>` — типизированный для `NormalizedTrade`.

Не отвечает: за персистенцию (это `persistence/`), за сериализацию.

## Входы / выходы

* Вход: `push(item)`.
* Выход: индексированный доступ, view последних K элементов, итераторы.

## Публичные интерфейсы (предположительно по соглашениям)

* `void push(T)` — добавить, при переполнении вытолкнуть oldest.
* `size_t size() const`, `size_t capacity() const = N`.
* `T& operator[](size_t i)` — доступ от oldest (0) к newest (size-1).
* `std::vector<T> last(K)` — копия последних K.

## Внутренние компоненты

* `ring_buffer.hpp` — generic.
* `candle_buffer.hpp` — типизированный alias.
* `trade_buffer.hpp` — типизированный alias.

## Зависимости

* `normalizer/normalized_events.hpp` (для конкретных типов).

## Потоки данных

`FeatureEngine::on_candle/on_trade` → push в per-symbol buffer → `compute_*` читает текущее окно.

## Race conditions

`std::unordered_map<string, CandleBuffer<500>>` хранится в `FeatureEngine` под `mutex_`. Сами буферы не имеют внутренней синхронизации; защита поверх через mutex владельца.

## Ошибки проектирования

* **D-buf-1 (LOW).** Размеры (500 свечей, 1000 трейдов) хардкодятся через template параметр в `feature_engine.hpp`. Не настраивается из YAML.
* **D-buf-2 (LOW).** Нет thread-safe варианта для случая concurrent producer/consumer (мог бы пригодиться при разделении public WS / hot-path).

## Контракты

### `RingBuffer<T,N>::push(T)`

* **Pre.** Никаких.
* **Post.** `size <= N`. Если был полон, oldest вытеснен.
* **Invariant.** `N > 0`.

## Производственные риски

* **R-buf-1.** Если N слишком мал для индикаторов (например, ATR(14) с буфером <14), `IndicatorEngine` вернёт invalid → `FeatureEngine::is_ready` = false. Текущее N=500 покрывает все запрашиваемые периоды.

## Рекомендации

1. Параметризовать N через config (`feature_engine.candle_buffer_size`).
2. Если потребуется lock-free: добавить `LockFreeRingBuffer<T,N>` на основе `std::atomic<size_t>` индексов (single producer / single consumer).
