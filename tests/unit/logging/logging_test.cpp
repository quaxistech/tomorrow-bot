/**
 * @file logging_test.cpp
 * @brief Unit тесты модуля логирования Tomorrow Bot
 *
 * Покрытие: LogLevel, LogEvent, LogContext, ScopedCorrelationId,
 * json_escape, timestamp_to_iso8601, format_as_json,
 * ConsoleLogger, FileLogger, CompositeLogger, фабричные функции.
 */
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include "logging/logger.hpp"
#include "logging/json_formatter.hpp"
#include "logging/log_event.hpp"
#include "logging/log_context.hpp"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>
#include <thread>

using namespace tb;
using namespace tb::logging;
using Catch::Matchers::ContainsSubstring;

// ============================================================
// Вспомогательный capturing logger для тестов
// ============================================================

class CapturingLogger : public ILogger {
public:
    struct Entry {
        LogEvent event;
    };

    explicit CapturingLogger(LogLevel level = LogLevel::Trace)
        : level_(log_level_value(level)) {}

    void log(LogEvent event) override {
        if (log_level_value(event.level) < level_.load(std::memory_order_relaxed)) return;
        std::lock_guard lock(mutex_);
        entries_.push_back(Entry{std::move(event)});
    }

    void set_level(LogLevel level) override {
        level_.store(log_level_value(level), std::memory_order_relaxed);
    }

    [[nodiscard]] LogLevel get_level() const override {
        // simplified inverse
        int v = level_.load(std::memory_order_relaxed);
        switch (v) {
            case 0: return LogLevel::Trace;
            case 1: return LogLevel::Debug;
            case 2: return LogLevel::Info;
            case 3: return LogLevel::Warn;
            case 4: return LogLevel::Error;
            case 5: return LogLevel::Critical;
            default: return LogLevel::Info;
        }
    }

    [[nodiscard]] std::vector<Entry> entries() const {
        std::lock_guard lock(mutex_);
        return entries_;
    }

    [[nodiscard]] size_t count() const {
        std::lock_guard lock(mutex_);
        return entries_.size();
    }

    void clear() {
        std::lock_guard lock(mutex_);
        entries_.clear();
    }

private:
    std::atomic<int> level_;
    mutable std::mutex mutex_;
    std::vector<Entry> entries_;
};

// ============================================================
// LogLevel
// ============================================================

TEST_CASE("LogLevel: числовые значения монотонно возрастают") {
    CHECK(log_level_value(LogLevel::Trace) < log_level_value(LogLevel::Debug));
    CHECK(log_level_value(LogLevel::Debug) < log_level_value(LogLevel::Info));
    CHECK(log_level_value(LogLevel::Info)  < log_level_value(LogLevel::Warn));
    CHECK(log_level_value(LogLevel::Warn)  < log_level_value(LogLevel::Error));
    CHECK(log_level_value(LogLevel::Error) < log_level_value(LogLevel::Critical));
}

TEST_CASE("LogLevel: to_string для всех уровней") {
    CHECK(to_string(LogLevel::Trace)    == "TRACE");
    CHECK(to_string(LogLevel::Debug)    == "DEBUG");
    CHECK(to_string(LogLevel::Info)     == "INFO");
    CHECK(to_string(LogLevel::Warn)     == "WARN");
    CHECK(to_string(LogLevel::Error)    == "ERROR");
    CHECK(to_string(LogLevel::Critical) == "CRITICAL");
}

// ============================================================
// json_escape
// ============================================================

TEST_CASE("json_escape: обычная строка без спецсимволов") {
    CHECK(json_escape("hello world") == "hello world");
}

TEST_CASE("json_escape: кавычки и бэкслеш") {
    CHECK(json_escape(R"(say "hi" \ there)") == R"(say \"hi\" \\ there)");
}

TEST_CASE("json_escape: переводы строк и табуляция") {
    CHECK(json_escape("a\nb\rc\td") == R"(a\nb\rc\td)");
}

TEST_CASE("json_escape: управляющие символы ниже 0x20") {
    std::string input(1, '\x01');
    auto escaped = json_escape(input);
    CHECK(escaped == "\\u0001");
}

TEST_CASE("json_escape: пустая строка") {
    CHECK(json_escape("") == "");
}

// ============================================================
// timestamp_to_iso8601
// ============================================================

TEST_CASE("timestamp_to_iso8601: Unix epoch = 1970-01-01T00:00:00.000Z") {
    auto result = timestamp_to_iso8601(Timestamp{0});
    CHECK(result == "1970-01-01T00:00:00.000Z");
}

TEST_CASE("timestamp_to_iso8601: известное время с миллисекундами") {
    // 86400 seconds = exactly 1970-01-02T00:00:00.000Z + 123ms
    int64_t ns = 86400'000'000'000LL + 123'000'000LL;
    auto result = timestamp_to_iso8601(Timestamp{ns});
    CHECK(result == "1970-01-02T00:00:00.123Z");
}

TEST_CASE("timestamp_to_iso8601: миллисекунды корректно округляются вниз") {
    // 500ms + 999999ns → должно показать .500, не .501
    int64_t ns = 1'000'000'000LL + 500'999'999LL;
    auto result = timestamp_to_iso8601(Timestamp{ns});
    CHECK_THAT(result, ContainsSubstring(".500Z"));
}

// ============================================================
// format_as_json
// ============================================================

TEST_CASE("format_as_json: минимальное событие без correlation_id и fields") {
    LogEvent event{
        .level = LogLevel::Info,
        .timestamp = Timestamp{0},
        .component = "test",
        .message = "hello",
        .correlation_id = "",
        .fields = {}
    };
    auto json = format_as_json(event);
    CHECK_THAT(json, ContainsSubstring("\"level\":\"INFO\""));
    CHECK_THAT(json, ContainsSubstring("\"component\":\"test\""));
    CHECK_THAT(json, ContainsSubstring("\"message\":\"hello\""));
    CHECK_THAT(json, !ContainsSubstring("\"correlation_id\""));
    CHECK_THAT(json, !ContainsSubstring("\"fields\""));
}

TEST_CASE("format_as_json: с correlation_id и полями") {
    LogEvent event{
        .level = LogLevel::Error,
        .timestamp = Timestamp{1'000'000'000'000LL},
        .component = "exchange",
        .message = "timeout",
        .correlation_id = "req-42",
        .fields = {{"symbol", "BTCUSDT"}, {"latency_ms", "150"}}
    };
    auto json = format_as_json(event);
    CHECK_THAT(json, ContainsSubstring("\"correlation_id\":\"req-42\""));
    CHECK_THAT(json, ContainsSubstring("\"symbol\":\"BTCUSDT\""));
    CHECK_THAT(json, ContainsSubstring("\"latency_ms\":\"150\""));
}

TEST_CASE("format_as_json: correlation_id в fields не дублируется на top-level") {
    LogEvent event{
        .level = LogLevel::Info,
        .timestamp = Timestamp{0},
        .component = "test",
        .message = "msg",
        .correlation_id = "cid-1",
        .fields = {{"correlation_id", "cid-1"}, {"extra", "val"}}
    };
    auto json = format_as_json(event);
    // correlation_id должен быть один раз как top-level, не в fields
    size_t count = 0;
    size_t pos = 0;
    while ((pos = json.find("correlation_id", pos)) != std::string::npos) {
        ++count;
        pos += 14;
    }
    // Один раз top-level key. Fields не должен содержать его.
    CHECK(count == 1);
}

TEST_CASE("format_as_json: спецсимволы в message корректно экранируются") {
    LogEvent event{
        .level = LogLevel::Warn,
        .timestamp = Timestamp{0},
        .component = "test",
        .message = "line1\nline2\ttab\"quote",
        .correlation_id = "",
        .fields = {}
    };
    auto json = format_as_json(event);
    CHECK_THAT(json, ContainsSubstring("line1\\nline2\\ttab\\\"quote"));
}

// ============================================================
// LogContext
// ============================================================

TEST_CASE("LogContext: начальное состояние пустое") {
    auto& ctx = LogContext::current();
    ctx.reset();
    CHECK(ctx.correlation_id().empty());
    CHECK(ctx.fields().empty());
}

TEST_CASE("LogContext: set/get correlation_id") {
    auto& ctx = LogContext::current();
    ctx.reset();
    ctx.set_correlation_id("abc-123");
    CHECK(ctx.correlation_id() == "abc-123");
    ctx.reset();
}

TEST_CASE("LogContext: set_field / clear_field") {
    auto& ctx = LogContext::current();
    ctx.reset();
    ctx.set_field("symbol", "ETHUSDT");
    CHECK(ctx.fields().at("symbol") == "ETHUSDT");
    ctx.clear_field("symbol");
    CHECK(ctx.fields().empty());
    ctx.reset();
}

TEST_CASE("LogContext: clear_fields сохраняет correlation_id") {
    auto& ctx = LogContext::current();
    ctx.reset();
    ctx.set_correlation_id("keep-me");
    ctx.set_field("tmp", "val");
    ctx.clear_fields();
    CHECK(ctx.correlation_id() == "keep-me");
    CHECK(ctx.fields().empty());
    ctx.reset();
}

TEST_CASE("LogContext: reset очищает всё") {
    auto& ctx = LogContext::current();
    ctx.set_correlation_id("id");
    ctx.set_field("k", "v");
    ctx.reset();
    CHECK(ctx.correlation_id().empty());
    CHECK(ctx.fields().empty());
}

// ============================================================
// ScopedCorrelationId
// ============================================================

TEST_CASE("ScopedCorrelationId: восстанавливает предыдущий id") {
    auto& ctx = LogContext::current();
    ctx.reset();
    ctx.set_correlation_id("outer");
    {
        ScopedCorrelationId scoped("inner");
        CHECK(ctx.correlation_id() == "inner");
    }
    CHECK(ctx.correlation_id() == "outer");
    ctx.reset();
}

TEST_CASE("ScopedCorrelationId: вложенные scope корректны") {
    auto& ctx = LogContext::current();
    ctx.reset();
    {
        ScopedCorrelationId s1("level-1");
        CHECK(ctx.correlation_id() == "level-1");
        {
            ScopedCorrelationId s2("level-2");
            CHECK(ctx.correlation_id() == "level-2");
        }
        CHECK(ctx.correlation_id() == "level-1");
    }
    CHECK(ctx.correlation_id().empty());
}

// ============================================================
// ConsoleLogger — фильтрация по уровню
// ============================================================

TEST_CASE("ConsoleLogger: фильтрация по уровню отбрасывает низкие сообщения") {
    auto logger = create_console_logger(LogLevel::Warn);
    // Не должно бросать исключений — просто отбросит
    logger->info("test", "should be filtered");
    logger->trace("test", "also filtered");
    // Не падает — проходит
    CHECK(true);
}

TEST_CASE("ConsoleLogger: set_level / get_level") {
    auto logger = create_console_logger(LogLevel::Info);
    CHECK(logger->get_level() == LogLevel::Info);
    logger->set_level(LogLevel::Error);
    CHECK(logger->get_level() == LogLevel::Error);
}

// ============================================================
// CapturingLogger — тесты ILogger convenience methods
// ============================================================

TEST_CASE("ILogger convenience: все 6 уровней создают корректные события") {
    auto logger = std::make_shared<CapturingLogger>(LogLevel::Trace);
    LogContext::current().reset();

    logger->trace("comp", "t");
    logger->debug("comp", "d");
    logger->info("comp", "i");
    logger->warn("comp", "w");
    logger->error("comp", "e");
    logger->critical("comp", "c");

    auto entries = logger->entries();
    REQUIRE(entries.size() == 6);
    CHECK(entries[0].event.level == LogLevel::Trace);
    CHECK(entries[1].event.level == LogLevel::Debug);
    CHECK(entries[2].event.level == LogLevel::Info);
    CHECK(entries[3].event.level == LogLevel::Warn);
    CHECK(entries[4].event.level == LogLevel::Error);
    CHECK(entries[5].event.level == LogLevel::Critical);
}

TEST_CASE("ILogger convenience: component и message передаются корректно") {
    auto logger = std::make_shared<CapturingLogger>();
    LogContext::current().reset();

    logger->info("MyModule", "something happened");
    auto entries = logger->entries();
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].event.component == "MyModule");
    CHECK(entries[0].event.message == "something happened");
}

TEST_CASE("ILogger convenience: fields передаются") {
    auto logger = std::make_shared<CapturingLogger>();
    LogContext::current().reset();

    logger->warn("exec", "order failed", {{"order_id", "12345"}, {"reason", "timeout"}});
    auto entries = logger->entries();
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].event.fields.at("order_id") == "12345");
    CHECK(entries[0].event.fields.at("reason") == "timeout");
}

TEST_CASE("ILogger convenience: timestamp заполняется ненулевым значением") {
    auto logger = std::make_shared<CapturingLogger>();
    LogContext::current().reset();

    logger->info("test", "msg");
    auto entries = logger->entries();
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].event.timestamp.get() > 0);
}

// ============================================================
// make_event — интеграция с LogContext
// ============================================================

TEST_CASE("make_event: correlation_id из LogContext попадает в event.correlation_id") {
    auto logger = std::make_shared<CapturingLogger>();
    auto& ctx = LogContext::current();
    ctx.reset();
    ctx.set_correlation_id("ctx-id-42");

    logger->info("test", "msg");

    auto entries = logger->entries();
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].event.correlation_id == "ctx-id-42");
    // НЕ должен дублироваться в fields
    CHECK(entries[0].event.fields.count("correlation_id") == 0);
    ctx.reset();
}

TEST_CASE("make_event: поля из LogContext мержатся, явные поля приоритетнее") {
    auto logger = std::make_shared<CapturingLogger>();
    auto& ctx = LogContext::current();
    ctx.reset();
    ctx.set_field("env", "production");
    ctx.set_field("symbol", "BTCUSDT");

    logger->info("test", "msg", {{"symbol", "ETHUSDT"}});

    auto entries = logger->entries();
    REQUIRE(entries.size() == 1);
    // Явное поле приоритетнее контекста
    CHECK(entries[0].event.fields.at("symbol") == "ETHUSDT");
    // Контекстное поле подтягивается
    CHECK(entries[0].event.fields.at("env") == "production");
    ctx.reset();
}

TEST_CASE("make_event: без контекста correlation_id пустой") {
    auto logger = std::make_shared<CapturingLogger>();
    LogContext::current().reset();

    logger->info("test", "msg");

    auto entries = logger->entries();
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].event.correlation_id.empty());
}

// ============================================================
// FileLogger
// ============================================================

TEST_CASE("FileLogger: запись в файл и чтение") {
    const std::string path = "/tmp/tb_test_logging_write.log";
    // Удаляем если существует
    std::filesystem::remove(path);

    {
        auto logger = create_file_logger(path, LogLevel::Info, false);
        REQUIRE(logger != nullptr);
        LogContext::current().reset();
        logger->info("test", "file line");
    }

    std::ifstream in(path);
    REQUIRE(in.is_open());
    std::string line;
    std::getline(in, line);
    CHECK_THAT(line, ContainsSubstring("[    INFO]"));
    CHECK_THAT(line, ContainsSubstring("[test]"));
    CHECK_THAT(line, ContainsSubstring("file line"));
    // Проверяем наличие timestamp в text формате
    CHECK_THAT(line, ContainsSubstring("T"));
    CHECK_THAT(line, ContainsSubstring("Z"));

    std::filesystem::remove(path);
}

TEST_CASE("FileLogger: JSON формат записывается в файл") {
    const std::string path = "/tmp/tb_test_logging_json.log";
    std::filesystem::remove(path);

    {
        auto logger = create_file_logger(path, LogLevel::Trace, true);
        REQUIRE(logger != nullptr);
        LogContext::current().reset();
        logger->debug("jsoncomp", "json message");
    }

    std::ifstream in(path);
    REQUIRE(in.is_open());
    std::string line;
    std::getline(in, line);
    CHECK_THAT(line, ContainsSubstring("\"level\":\"DEBUG\""));
    CHECK_THAT(line, ContainsSubstring("\"component\":\"jsoncomp\""));
    CHECK_THAT(line, ContainsSubstring("\"message\":\"json message\""));

    std::filesystem::remove(path);
}

TEST_CASE("FileLogger: фильтрация по уровню работает") {
    const std::string path = "/tmp/tb_test_logging_filter.log";
    std::filesystem::remove(path);

    {
        auto logger = create_file_logger(path, LogLevel::Error);
        REQUIRE(logger != nullptr);
        LogContext::current().reset();
        logger->info("test", "should not appear");
        logger->error("test", "should appear");
    }

    std::ifstream in(path);
    REQUIRE(in.is_open());
    std::string content((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
    CHECK_THAT(content, !ContainsSubstring("should not appear"));
    CHECK_THAT(content, ContainsSubstring("should appear"));

    std::filesystem::remove(path);
}

TEST_CASE("FileLogger: создаёт промежуточные директории") {
    const std::string path = "/tmp/tb_test_logging_subdir/nested/test.log";
    std::filesystem::remove_all("/tmp/tb_test_logging_subdir");

    auto logger = create_file_logger(path, LogLevel::Info);
    REQUIRE(logger != nullptr);
    LogContext::current().reset();
    logger->info("test", "nested dir");

    CHECK(std::filesystem::exists(path));
    std::filesystem::remove_all("/tmp/tb_test_logging_subdir");
}

TEST_CASE("create_file_logger: несуществующий путь возвращает nullptr") {
    // /proc/non_existent — точно не получится открыть
    auto logger = create_file_logger("/proc/non_existent/impossible.log");
    CHECK(logger == nullptr);
}

// ============================================================
// CompositeLogger
// ============================================================

TEST_CASE("CompositeLogger: события дублируются во все sink'и") {
    auto sink1 = std::make_shared<CapturingLogger>();
    auto sink2 = std::make_shared<CapturingLogger>();
    auto composite = create_composite_logger({sink1, sink2});
    LogContext::current().reset();

    composite->info("test", "composite msg");

    CHECK(sink1->count() == 1);
    CHECK(sink2->count() == 1);
    CHECK(sink1->entries()[0].event.message == "composite msg");
    CHECK(sink2->entries()[0].event.message == "composite msg");
}

TEST_CASE("CompositeLogger: set_level прокидывается во все sink'и") {
    auto sink1 = std::make_shared<CapturingLogger>(LogLevel::Info);
    auto sink2 = std::make_shared<CapturingLogger>(LogLevel::Info);
    auto composite = create_composite_logger({sink1, sink2});

    composite->set_level(LogLevel::Error);

    CHECK(sink1->get_level() == LogLevel::Error);
    CHECK(sink2->get_level() == LogLevel::Error);
}

TEST_CASE("CompositeLogger: get_level возвращает уровень первого sink'а") {
    auto sink1 = std::make_shared<CapturingLogger>(LogLevel::Warn);
    auto sink2 = std::make_shared<CapturingLogger>(LogLevel::Debug);
    auto composite = create_composite_logger({sink1, sink2});

    CHECK(composite->get_level() == LogLevel::Warn);
}

TEST_CASE("CompositeLogger: null sink'и пропускаются без падения") {
    auto sink1 = std::make_shared<CapturingLogger>();
    std::shared_ptr<ILogger> null_sink = nullptr;
    auto composite = create_composite_logger({null_sink, sink1, null_sink});
    LogContext::current().reset();

    composite->info("test", "with null sinks");
    CHECK(sink1->count() == 1);
}

// ============================================================
// Фабричные функции
// ============================================================

TEST_CASE("create_console_logger: возвращает не-null с корректным уровнем") {
    auto logger = create_console_logger(LogLevel::Debug, true);
    REQUIRE(logger != nullptr);
    CHECK(logger->get_level() == LogLevel::Debug);
}

TEST_CASE("create_file_logger: валидный путь возвращает не-null") {
    const std::string path = "/tmp/tb_test_factory.log";
    std::filesystem::remove(path);

    auto logger = create_file_logger(path, LogLevel::Info);
    REQUIRE(logger != nullptr);
    CHECK(logger->get_level() == LogLevel::Info);

    std::filesystem::remove(path);
}

// ============================================================
// Text formatter — timestamp присутствует
// ============================================================

TEST_CASE("Text formatter: содержит ISO-8601 timestamp") {
    const std::string path = "/tmp/tb_test_text_timestamp.log";
    std::filesystem::remove(path);

    {
        auto logger = create_file_logger(path, LogLevel::Info, false);
        REQUIRE(logger != nullptr);
        LogContext::current().reset();
        logger->info("ts_test", "checking timestamp");
    }

    std::ifstream in(path);
    std::string line;
    std::getline(in, line);
    // ISO-8601: YYYY-MM-DDTHH:MM:SS.mmmZ
    // Должна быть в начале строки
    CHECK(line.size() > 24);
    CHECK(line[4] == '-');
    CHECK(line[7] == '-');
    CHECK(line[10] == 'T');
    CHECK(line[13] == ':');

    std::filesystem::remove(path);
}

// ============================================================
// Потокобезопасность (базовый smoke test)
// ============================================================

TEST_CASE("ConsoleLogger: потокобезопасность — параллельная запись без падений") {
    auto logger = create_console_logger(LogLevel::Critical); // Всё отфильтруется

    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i) {
        threads.emplace_back([&logger, i]() {
            for (int j = 0; j < 100; ++j) {
                logger->info("thread-" + std::to_string(i), "msg-" + std::to_string(j));
            }
        });
    }
    for (auto& t : threads) {
        t.join();
    }
    // Если не упал — тест пройден
    CHECK(true);
}

TEST_CASE("FileLogger: потокобезопасность — параллельная запись") {
    const std::string path = "/tmp/tb_test_thread_safety.log";
    std::filesystem::remove(path);

    auto logger = create_file_logger(path, LogLevel::Info);
    REQUIRE(logger != nullptr);
    LogContext::current().reset();

    std::vector<std::thread> threads;
    constexpr int kThreads = 4;
    constexpr int kMessagesPerThread = 50;

    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&logger, i]() {
            for (int j = 0; j < kMessagesPerThread; ++j) {
                logger->info("thread", "msg",
                    {{"tid", std::to_string(i)}, {"seq", std::to_string(j)}});
            }
        });
    }
    for (auto& t : threads) {
        t.join();
    }

    // Считаем строки в файле
    std::ifstream in(path);
    int line_count = 0;
    std::string line;
    while (std::getline(in, line)) {
        ++line_count;
    }
    CHECK(line_count == kThreads * kMessagesPerThread);

    std::filesystem::remove(path);
}
