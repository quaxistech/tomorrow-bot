# Разработка

## Сборка

### Debug (с санитайзерами)

```bash
./scripts/build_debug.sh
# или
cmake -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_COMPILER=g++-14 \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
ninja -C build -j$(nproc)
```

### Release

```bash
./scripts/build_release.sh
# или
cmake -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER=g++-14
ninja -C build -j$(nproc)
```

### Compile commands

`compile_commands.json` генерируется в `build/` для IDE (clangd, VS Code).

---

## Тесты

**Фреймворк**: Catch2 v3 (через FetchContent).

```bash
cd build && ctest -j$(nproc) --output-on-failure
```

### Структура тестов

```
tests/
├── unit/                    # 27 модулей юнит-тестов
│   ├── order_book/
│   ├── indicators/
│   ├── features/
│   ├── world_model/
│   ├── regime/
│   ├── uncertainty/
│   ├── strategy/
│   ├── decision/
│   ├── ml/
│   ├── execution_alpha/
│   ├── opportunity_cost/
│   ├── portfolio/
│   ├── portfolio_allocator/
│   ├── risk/
│   ├── config/
│   ├── execution/
│   ├── persistence/
│   ├── reconciliation/
│   ├── recovery/
│   ├── resilience/
│   ├── security/
│   ├── supervisor/
│   ├── leverage/
│   ├── logging/
│   ├── metrics/
│   └── scenario_integration/
├── integration/             # Интеграционные тесты
│   ├── indicator_smoke_test.cpp
│   └── normalizer_test.cpp
├── common/                  # Общие моки (test_mocks.hpp)
└── data/                    # Тестовые данные
```

### Запуск конкретного теста

```bash
cd build && ctest -R "test_risk" --output-on-failure
```

---

## Структура модуля

Каждый модуль в `src/` содержит:

```
src/module_name/
├── CMakeLists.txt           # Библиотека + зависимости
├── module_name.hpp          # Публичный интерфейс
├── module_name.cpp          # Реализация
├── i_module_name.hpp        # Интерфейс (опционально)
└── types.hpp                # Типы модуля (опционально)
```

`CMakeLists.txt` модуля:

```cmake
add_library(module_name
    module_name.cpp
)
target_include_directories(module_name PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(module_name PUBLIC common logging)
```

---

## Добавление нового модуля

1. Создайте директорию `src/new_module/`
2. Создайте `CMakeLists.txt` с библиотекой
3. Добавьте `add_subdirectory(new_module)` в `src/CMakeLists.txt` (если есть) или корневой `CMakeLists.txt`
4. Создайте тесты в `tests/unit/new_module/`
5. Зарегистрируйте тесты в `tests/CMakeLists.txt`

---

## Интерфейсы

Ключевые интерфейсы (для моков в тестах):

| Интерфейс | Модуль | Методы |
|-----------|--------|--------|
| `ILogger` | logging | `debug()`, `info()`, `warn()`, `error()` |
| `IClock` | clock | `now_ns()`, `now_ms()` |
| `IMetricsRegistry` | metrics | `counter()`, `gauge()`, `histogram()` |
| `ISecretProvider` | security | `get_secret()` |
| `IOrderSubmitter` | execution | `submit_order()`, `cancel_order()` |
| `IStrategy` | strategy | `evaluate()`, `meta()`, `is_active()` |
| `IRiskEngine` | risk | `evaluate()`, `activate_kill_switch()` |
| `IPortfolioEngine` | portfolio | `snapshot()`, `apply_fill()` |

`TestClock` в тестах позволяет контролировать время (`advance()`, `set()`).

---

## CMake опции

Файлы в `cmake/`:

| Файл | Описание |
|------|----------|
| `CompilerWarnings.cmake` | Строгие предупреждения (-Wall, -Wextra, -Wpedantic) |
| `Sanitizers.cmake` | ASan, UBSan, TSan для Debug-сборок |
| `ProjectOptions.cmake` | Общие опции проекта |

---

## Утилиты

| Утилита | Описание |
|---------|----------|
| `tools/config_validator/` | Валидация YAML-конфигов |
| `tools/log_summarizer/` | Суммаризация логов |
| `tools/replay_inspector/` | Инспектор записей |
| `tools/telemetry_viewer/` | Просмотр телеметрии |

---

## Конвенции

- **C++23**: используйте `std::optional`, `std::expected`, structured bindings
- **Result<T>**: для fallible операций вместо исключений
- **StrongType**: типизированные обёртки для предотвращения ошибок
- **Интерфейсы**: виртуальные базовые классы с префиксом `I` для DI
- **Тесты**: каждый модуль тестируется отдельно, моки через интерфейсы
- **Логирование**: структурированный JSON, уровни через `ILogger`
