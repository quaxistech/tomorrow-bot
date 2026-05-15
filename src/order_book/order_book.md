# Модуль order_book — подробный разбор

Временный рабочий документ.

Дата: 2026-04-10

## 1. Назначение модуля

`src/order_book` — это локальная in-memory реплика L2-стакана для одного инструмента.

Его задача:

1. принять нормализованный snapshot стакана;
2. принять нормализованные delta-обновления;
3. поддерживать локальное состояние bid/ask уровней;
4. контролировать целостность sequence;
5. отдавать downstream-коду:
   - `top_of_book`;
   - агрегированную глубину;
   - статус качества стакана.

Это не matching engine, не OMS и не routing-слой.

Модуль ничего не знает о:

- постановке ордеров;
- биржевом исполнении;
- позиции;
- spot-торговле.

Он работает в контуре рыночных данных USDT-M futures и обслуживает downstream-движки признаков, риска и uncertainty.

## 2. Состав модуля

В каталоге 4 файла:

| Файл | Назначение |
|---|---|
| `CMakeLists.txt` | сборка библиотеки `tb_order_book` |
| `order_book_types.hpp` | типы качества, top-of-book и depth summary |
| `order_book.hpp` | интерфейс и состояние `LocalOrderBook` |
| `order_book.cpp` | полная реализация локального стакана |

Модуль маленький: публичная поверхность фактически состоит из одного класса `LocalOrderBook` и двух DTO-структур результата.

## 3. Сборка и зависимости

`tb_order_book` собирается как `STATIC` библиотека из одного файла:

- `order_book.cpp`

Зависимости:

- `tb_common`
- `tb_logging`
- `tb_metrics`
- `tb_normalizer`

Практический смысл:

- `tb_normalizer` поставляет `NormalizedOrderBook` и `BookLevel`;
- `tb_common` даёт strong types (`Price`, `Quantity`, `Symbol`, `Timestamp`);
- `tb_logging` нужен для warning при разрыве sequence;
- `tb_metrics` протянут в конструктор, но в текущей реализации фактически не используется.

## 4. Основная сущность — `LocalOrderBook`

Класс хранит локальную копию книги заявок для одного символа.

Публичный API:

```cpp
void apply_snapshot(const normalizer::NormalizedOrderBook& snap);
bool apply_delta(const normalizer::NormalizedOrderBook& delta);
void request_resync();

BookQuality quality() const;
std::optional<TopOfBook> top_of_book() const;
std::optional<DepthSummary> depth_summary(int levels = 10) const;

std::map<tb::Price, tb::Quantity, std::greater<tb::Price>> bids() const;
std::map<tb::Price, tb::Quantity> asks() const;

tb::Symbol symbol() const;
tb::Timestamp last_updated() const;
```

Смысл этого API простой:

- snapshot и delta мутируют внутреннее состояние;
- `quality()` даёт статус пригодности книги;
- `top_of_book()` и `depth_summary()` дают производные метрики;
- `bids()` / `asks()` возвращают полные копии текущих сторон стакана.

## 5. Внутреннее состояние

Внутри `LocalOrderBook` хранит:

- `symbol_` — символ книги;
- `bids_` — bid-side как `std::map<Price, Quantity, std::greater<Price>>`;
- `asks_` — ask-side как `std::map<Price, Quantity>`;
- `last_sequence_` — последний применённый sequence;
- `quality_` — текущее качество книги;
- `last_updated_` — время последнего принятого обновления;
- `logger_` и `metrics_`;
- `mutex_` — защита конкурентного доступа.

Ключевое инженерное решение: книга держится не в vector/heap-структуре по уровням, а в `std::map`.

Это даёт:

- естественную сортировку уровней;
- простое обновление/удаление по цене;
- простую модель для snapshot + delta.

Цена этого решения:

- аллокации и log-time операции;
- копирование полных map при вызове `bids()` / `asks()`;
- отсутствие оптимизации под ultra-low-latency HFT.

Для текущего бота это выглядит осознанным компромиссом в пользу корректности и простоты.

## 6. Типы результата

### 6.1. `BookQuality`

Enum содержит 5 состояний:

- `Valid`
- `Stale`
- `Desynced`
- `Resyncing`
- `Uninitialized`

Фактически в коде реально используются только 3:

- `Uninitialized`
- `Valid`
- `Desynced`

`Stale` и `Resyncing` пока архитектурный задел, но не материализованная логика.

### 6.2. `TopOfBook`

Содержит:

- `best_bid`
- `best_ask`
- `bid_size`
- `ask_size`
- `spread`
- `spread_bps`
- `mid_price`
- `updated_at`
- `quality`

Это готовый top-level DTO для downstream-логики микро-структуры.

### 6.3. `DepthSummary`

Содержит:

- `bid_depth_5`
- `ask_depth_5`
- `bid_depth_10`
- `ask_depth_10`
- `imbalance_5`
- `imbalance_10`
- `weighted_mid`
- `computed_at`

То есть модуль отдаёт не только лучшую цену, но и агрегаты по глубине.

## 7. Контракт входных данных

`LocalOrderBook` принимает `normalizer::NormalizedOrderBook`.

Этот тип содержит:

```cpp
struct NormalizedOrderBook {
    EventEnvelope envelope;
    BookUpdateType update_type;
    std::vector<BookLevel> bids;
    std::vector<BookLevel> asks;
    uint64_t sequence;
};
```

`BookLevel` состоит из:

```cpp
struct BookLevel {
    tb::Price price;
    tb::Quantity size;
};
```

Важно:

- модуль получает уже нормализованные цены/размеры;
- сам он не парсит сырой WS payload;
- сам он не знает биржевой JSON-формат;
- весь exchange-specific слой уже спрятан внутри normalizer.

## 8. Как работает `apply_snapshot()`

Алгоритм прямой:

1. берёт mutex;
2. очищает `bids_` и `asks_`;
3. применяет все bid-уровни;
4. применяет все ask-уровни;
5. сохраняет `last_sequence_ = snap.sequence`;
6. сохраняет `last_updated_ = snap.envelope.processed_ts`;
7. выставляет `BookQuality::Valid`.

Это означает, что snapshot — абсолютная истина, полностью переопределяющая локальное состояние.

Важный нюанс:

- `apply_snapshot()` не проверяет `snap.update_type`;
- модуль доверяет upstream-коду, что сюда действительно приходит snapshot.

В текущем runtime это допустимо, потому что тип обновления уже контролирует `MarketDataGateway`.

## 9. Как работает `apply_delta()`

`apply_delta()` — центральная логика целостности книги.

Алгоритм:

1. берёт mutex;
2. если книга `Uninitialized` или `Desynced`, сразу возвращает `false`;
3. проверяет sequence continuity:

```cpp
delta.sequence == last_sequence_ + 1
```

4. если sequence broken:
   - пишет warning в лог;
   - переводит книгу в `Desynced`;
   - возвращает `false`;
5. если sequence корректный:
   - применяет уровни bid/ask;
   - обновляет `last_sequence_`;
   - обновляет `last_updated_`;
   - оставляет `BookQuality::Valid`;
   - возвращает `true`.

Это важный design choice: модуль предпочитает fail-closed модель.

При пропуске хотя бы одной delta он не пытается «догадаться» о состоянии стакана, а требует resync.

## 10. Что именно считается рассинхронизацией

Рассинхронизация определяется только по sequence gap.

Если было:

- `last_sequence_ = 100`

и пришло:

- `delta.sequence = 102`

то книга считается недостоверной.

Это простой и правильный механизм контроля для incremental L2 feed.

Но важно понимать ограничение:

- checksum уровня книги модуль не проверяет;
- кроссированный стакан не валидирует;
- отрицательные/битые размеры не фильтрует;
- соответствие `symbol_` и `ev.envelope.symbol` не проверяет.

То есть интегральная целостность книги контролируется минимально, только по sequence continuity.

## 11. `request_resync()`

Этот метод:

1. берёт mutex;
2. очищает обе стороны стакана;
3. сбрасывает `last_sequence_` в `0`;
4. выставляет `BookQuality::Uninitialized`.

Это значит, что после resync модуль не находится в состоянии «частично восстанавливается», а просто ждёт новый snapshot.

Практический смысл:

- пустая книга не считается пригодной;
- следующая валидная snapshot-поставка полностью восстановит состояние.

Отсюда же видно, что `BookQuality::Resyncing` объявлен, но не используется.

## 12. Как обновляются уровни стакана

Внутри есть две перегрузки `apply_levels()`:

- для bid-side map с `std::greater<Price>`;
- для ask-side map с обычным ascending order.

Логика одинаковая:

```cpp
if (lvl.size.get() == 0.0) {
    side.erase(lvl.price);
} else {
    side.insert_or_assign(lvl.price, lvl.size);
}
```

Смысл:

- size `0` трактуется как удаление уровня;
- ненулевой размер заменяет или добавляет уровень.

Это стандартная модель применения L2 deltas.

## 13. Как считается `top_of_book()`

Если любая сторона пуста, метод возвращает `std::nullopt`.

Иначе берутся:

- лучший bid = `*bids_.begin()`;
- лучший ask = `*asks_.begin()`.

Затем считаются:

```cpp
spread = ask - bid
mid    = (ask + bid) / 2
spread_bps = (spread / mid) * 10000
```

Дальше строится `TopOfBook`.

Важно:

- метод не проверяет, что `best_bid <= best_ask`;
- если книга кроссирована, `spread` может стать отрицательным;
- quality просто копируется из состояния книги, а не выводится заново из структуры лучшего bid/ask.

## 14. Как считается `depth_summary()`

Метод тоже возвращает `std::nullopt`, если одна из сторон пуста.

Затем делает три вещи.

### 14.1. Сумма объёмов по уровням

Для каждой стороны суммируется `qty` по первым `n` уровням.

В коде используются:

- первые 5 уровней;
- первые `levels` уровней.

Поэтому поля называются:

- `bid_depth_5`, `ask_depth_5`;
- `bid_depth_10`, `ask_depth_10`.

Но вторые два поля на самом деле означают не обязательно 10 уровней, а «глубину на `levels`», где дефолт просто равен 10.

Это важный семанический нюанс DTO.

### 14.2. Imbalance

Формула:

```cpp
imbalance = (bid_depth - ask_depth) / (bid_depth + ask_depth)
```

Диапазон: `[-1, 1]`.

Смысл:

- положительное значение = bid-side доминирует;
- отрицательное = ask-side доминирует;
- около нуля = баланс объёмов.

### 14.3. Weighted mid

Модуль считает:

```cpp
sum(price * qty) / sum(qty)
```

по bid и ask уровням вместе.

Это не microprice в классическом виде.

Классический microprice обычно сильнее учитывает противоположную сторону top-of-book. Здесь же используется более грубый depth-weighted average по нескольким уровням.

То есть название `weighted_mid` корректно, но понимать его нужно именно как volume-weighted combined book price, а не как canonical microprice.

## 15. Потокобезопасность

Все публичные методы, кроме `symbol()`, берут `mutex_`.

Это даёт:

- безопасное чтение/запись из нескольких потоков;
- последовательное применение snapshot/delta;
- отсутствие torn-reads для top-of-book/depth summary.

Ограничение:

- все вызовы сериализуются;
- `bids()` и `asks()` возвращают полные копии map под mutex, что может быть дорого при частом доступе.

Для текущей архитектуры это приемлемо, потому что модуль небольшой и downstream чаще запрашивает агрегаты, а не полные стороны книги.

## 16. Где модуль используется в runtime

Главный consumer — `MarketDataGateway`.

Путь данных такой:

1. `BitgetWsClient` получает сырые WS сообщения;
2. `BitgetNormalizer` переводит их в `NormalizedEvent`;
3. для `NormalizedOrderBook` gateway:
   - вызывает `apply_snapshot()` для snapshot;
   - вызывает `apply_delta()` для delta;
4. если `apply_delta()` вернул `false`, gateway пишет warning и вызывает `request_resync()`;
5. после тикеров `FeatureEngine` строит `FeatureSnapshot`, используя актуальный `LocalOrderBook`.

Это важный момент: `order_book` сам не инициирует network-resync. Он лишь сигнализирует о проблеме через `false`, а orchestration делает gateway.

## 17. Где используются результаты `LocalOrderBook`

Downstream `FeatureEngine` использует книгу в двух местах.

### 17.1. Microstructure features

Из `top_of_book()` берётся:

- `weighted_mid_price = tob->mid_price`, если нет depth summary.

Из `depth_summary()` берутся:

- `book_imbalance_5`
- `book_imbalance_10`
- `weighted_mid_price = depth->weighted_mid`
- `bid_depth_5_notional`
- `ask_depth_5_notional`
- `liquidity_ratio`

То есть order book напрямую участвует в микро-структурных фичах стратегии и execution контекста.

### 17.2. Book quality как downstream gate

`FeatureSnapshot.book_quality` копируется из `book.quality()`.

Дальше это качество реально влияет на другие модули:

- `uncertainty` повышает неопределённость, если `book_quality != Valid`;
- `risk::BookQualityCheck` отклоняет решение, если стакан невалиден.

Значит, `LocalOrderBook` участвует не только в feature extraction, но и в runtime safety.

## 18. Что покрыто тестами

Есть отдельный unit target:

- `tests/unit/order_book/test_order_book`

По текущему состоянию:

- `7` test cases;
- `21` assertions;
- все проходят.

Покрыто тестами:

- `apply_snapshot()` → `Valid`;
- `apply_delta()` при правильном sequence;
- `apply_delta()` при sequence gap → `Desynced`;
- `request_resync()` → `Uninitialized`;
- корректность `top_of_book()`;
- корректность imbalance в `depth_summary()`;
- удаление уровня через `size=0`.

## 19. Что покрыто слабо или не покрыто

Напрямую не покрыты:

- конкурентный доступ из нескольких потоков;
- поведение на кроссированном стакане;
- некорректные/отрицательные цены и объёмы;
- защита от символа, не совпадающего с `symbol_`;
- поведение при пустых delta-обновлениях;
- реальная интеграция с `MarketDataGateway` в тестовом сценарии reconnect;
- отсутствие использования `metrics_`.

## 20. Главные инженерные наблюдения

### 20.1. Это локальная реплика книги, а не источник истины

Модуль целиком зависит от корректности upstream normalizer/gateway и не пытается сам восстанавливать пропущенные данные.

### 20.2. Sequence continuity — основной guardrail

Главная гарантия корректности книги здесь — непрерывность sequence.

Это хороший минимальный production guard, но не полная data-integrity модель.

### 20.3. `BookQuality` шире, чем реальная логика

Enum предусматривает больше состояний, чем реально используется.

Сейчас живое поведение сводится к:

- `Uninitialized` → до первого snapshot;
- `Valid` → при корректном snapshot/delta потоке;
- `Desynced` → при sequence gap;
- затем `request_resync()` снова возвращает в `Uninitialized`.

`Stale` и `Resyncing` пока не встроены в рабочий state machine.

### 20.4. Метрики пока не реализованы

`metrics_` передаётся и хранится, но нигде не используется.

То есть observability у модуля сейчас ограничена логами и косвенными downstream-эффектами.

### 20.5. Символ книги не верифицируется

`symbol_` хранится в объекте, но `apply_snapshot()` и `apply_delta()` не проверяют, что `ev.envelope.symbol == symbol_`.

В текущем pipeline это, вероятно, контролируется снаружи, но внутри модуля такой защитной проверки нет.

### 20.6. Книга не валидирует рыночную физику

Нет проверки на:

- `best_bid <= best_ask`;
- отрицательные объёмы;
- невалидные цены;
- дубль-уровни или нарушенную форму snapshot.

Модуль предполагает, что это уже гарантировано нормализатором.

### 20.7. Weighted mid — это не классический microprice

Это важно для downstream-интерпретации: `weighted_mid` здесь — агрегированный volume-weighted price по обеим сторонам на нескольких уровнях, а не textbook microprice по top-of-book.

## 21. Отдельный важный нюанс по документации

Внутренняя документация `market_data.md` частично устарела.

Там есть утверждение, что `MarketDataGateway` не реагирует на `apply_delta() == false`.

Фактический код уже делает следующее:

1. пишет warning;
2. вызывает `order_book_->request_resync()`;
3. ждёт следующий snapshot.

То есть анализ модуля нужно опирать на код, а не на старый markdown.

## 22. Краткий итог

`src/order_book` — это компактный и достаточно строгий локальный L2 order book для одного USDT-M futures инструмента.

Он:

- принимает snapshot и delta из `normalizer`;
- поддерживает bid/ask книгу в отсортированных map;
- отслеживает sequence continuity;
- отдаёт `top_of_book`, глубину и imbalance;
- транслирует статус качества книги вниз по pipeline.

В архитектуре модуль стоит между normalizer и feature/risk/uncertainty слоями.

Его сильные стороны:

- простая и понятная модель состояния;
- fail-closed поведение при sequence gap;
- thread-safe чтение/запись;
- хорошие базовые unit-тесты;
- полезные downstream-метрики для микро-структуры.

Его текущие ограничения:

- нет checksum и cross-book validation;
- не используются `Stale`, `Resyncing` и `metrics_`;
- нет проверки символа входного события;
- нет отдельной stale-логики по времени;
- `weighted_mid` — это упрощённая depth-weighted метрика, а не полноценный microprice.

В текущем виде модуль лучше всего понимать как **надёжную локальную реплику книги заявок для USDT-M futures data pipeline**, которая обеспечивает downstream-системам базовую целостность order book и объяснимые агрегаты для feature extraction.