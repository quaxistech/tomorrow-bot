#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "portfolio/portfolio_engine.hpp"
#include "logging/logger.hpp"
#include "clock/clock.hpp"
#include "metrics/metrics_registry.hpp"

using namespace tb;
using namespace tb::portfolio;
using namespace Catch::Matchers;

// ========== Тестовые заглушки ==========

class TestLogger : public logging::ILogger {
public:
    void log(logging::LogEvent /*event*/) override {}
    void set_level(logging::LogLevel /*level*/) override {}
    [[nodiscard]] logging::LogLevel get_level() const override { return logging::LogLevel::Debug; }
};

class TestClock : public clock::IClock {
public:
    [[nodiscard]] Timestamp now() const override { return Timestamp(1000000); }
};

class TestMetrics : public metrics::IMetricsRegistry {
    struct NullCounter : metrics::ICounter {
        std::string name_{"null"};
        void increment(double /*v*/) override {}
        void increment(double /*v*/, const metrics::MetricTags&) override {}
        [[nodiscard]] double value() const override { return 0; }
        [[nodiscard]] const std::string& name() const override { return name_; }
    };
    struct NullGauge : metrics::IGauge {
        std::string name_{"null"};
        void set(double /*v*/) override {}
        void set(double /*v*/, const metrics::MetricTags&) override {}
        void increment(double /*v*/) override {}
        void decrement(double /*v*/) override {}
        [[nodiscard]] double value() const override { return 0; }
        [[nodiscard]] const std::string& name() const override { return name_; }
    };
    struct NullHistogram : metrics::IHistogram {
        std::string name_{"null"};
        void observe(double /*v*/) override {}
        void observe(double /*v*/, const metrics::MetricTags&) override {}
        [[nodiscard]] const std::string& name() const override { return name_; }
    };
public:
    std::shared_ptr<metrics::ICounter> counter(std::string, metrics::MetricTags) override {
        return std::make_shared<NullCounter>();
    }
    std::shared_ptr<metrics::IGauge> gauge(std::string, metrics::MetricTags) override {
        return std::make_shared<NullGauge>();
    }
    std::shared_ptr<metrics::IHistogram> histogram(std::string, std::vector<double>, metrics::MetricTags) override {
        return std::make_shared<NullHistogram>();
    }
    [[nodiscard]] std::string export_prometheus() const override { return ""; }
};

// ========== Вспомогательные функции ==========

static std::shared_ptr<InMemoryPortfolioEngine> make_portfolio(double capital = 10000.0) {
    return std::make_shared<InMemoryPortfolioEngine>(
        capital,
        std::make_shared<TestLogger>(),
        std::make_shared<TestClock>(),
        std::make_shared<TestMetrics>());
}

static Position make_position(const std::string& symbol = "BTCUSDT",
                               Side side = Side::Buy,
                               double size = 0.1,
                               double entry_price = 50000.0) {
    Position pos;
    pos.symbol = Symbol(symbol);
    pos.side = side;
    pos.size = Quantity(size);
    pos.avg_entry_price = Price(entry_price);
    pos.current_price = Price(entry_price);
    pos.notional = NotionalValue(size * entry_price);
    pos.strategy_id = StrategyId("test_strategy");
    return pos;
}

// ========== Тесты ==========

TEST_CASE("Portfolio: Открытие позиции отслеживается корректно", "[portfolio]") {
    auto portfolio = make_portfolio();
    auto pos = make_position();

    portfolio->open_position(pos);

    REQUIRE(portfolio->has_position(Symbol("BTCUSDT")));
    auto retrieved = portfolio->get_position(Symbol("BTCUSDT"));
    REQUIRE(retrieved.has_value());
    REQUIRE(retrieved->symbol.get() == "BTCUSDT");
    REQUIRE(retrieved->size.get() == 0.1);
    REQUIRE(retrieved->avg_entry_price.get() == 50000.0);
}

TEST_CASE("Portfolio: Обновление цены изменяет нереализованную P&L", "[portfolio]") {
    auto portfolio = make_portfolio();
    auto pos = make_position("BTCUSDT", Side::Buy, 0.1, 50000.0);
    portfolio->open_position(pos);

    // Цена выросла
    portfolio->update_price(Symbol("BTCUSDT"), Price(51000.0));

    auto updated = portfolio->get_position(Symbol("BTCUSDT"));
    REQUIRE(updated.has_value());
    // P&L = (51000 - 50000) * 0.1 = 100
    REQUIRE_THAT(updated->unrealized_pnl, WithinAbs(100.0, 0.01));
}

TEST_CASE("Portfolio: Закрытие позиции обновляет реализованную P&L", "[portfolio]") {
    auto portfolio = make_portfolio();
    auto pos = make_position();
    portfolio->open_position(pos);

    portfolio->close_position(Symbol("BTCUSDT"), Price(51000.0), 100.0);

    REQUIRE_FALSE(portfolio->has_position(Symbol("BTCUSDT")));

    auto pnl = portfolio->pnl();
    REQUIRE_THAT(pnl.realized_pnl_today, WithinAbs(100.0, 0.01));
    REQUIRE(pnl.trades_today == 1);
}

TEST_CASE("Portfolio: Расчёт экспозиции корректен", "[portfolio]") {
    auto portfolio = make_portfolio(100000.0);

    // Открыть длинную позицию
    auto long_pos = make_position("BTCUSDT", Side::Buy, 0.1, 50000.0);
    portfolio->open_position(long_pos);

    // Открыть короткую позицию
    auto short_pos = make_position("ETHUSDT", Side::Sell, 1.0, 3000.0);
    portfolio->open_position(short_pos);

    auto exp = portfolio->exposure();
    REQUIRE(exp.open_positions_count == 2);
    REQUIRE_THAT(exp.long_exposure, WithinAbs(5000.0, 0.01)); // 0.1 * 50000
    REQUIRE_THAT(exp.short_exposure, WithinAbs(3000.0, 0.01)); // 1.0 * 3000
    REQUIRE_THAT(exp.gross_exposure, WithinAbs(8000.0, 0.01));
    REQUIRE_THAT(exp.net_exposure, WithinAbs(2000.0, 0.01)); // 5000 - 3000
}

TEST_CASE("Portfolio: Отслеживание просадки от пика", "[portfolio]") {
    auto portfolio = make_portfolio(10000.0);

    // Открыть позицию
    auto pos = make_position("BTCUSDT", Side::Buy, 0.1, 50000.0);
    portfolio->open_position(pos);

    // Цена сначала выросла (обновляет пик)
    portfolio->update_price(Symbol("BTCUSDT"), Price(52000.0));

    // Потом упала
    portfolio->update_price(Symbol("BTCUSDT"), Price(48000.0));

    auto pnl = portfolio->pnl();
    // Пик equity = 10000 + (52000-50000)*0.1 = 10200
    // Текущая equity = 10000 + (48000-50000)*0.1 = 9800
    // Просадка = (10200 - 9800) / 10200 * 100 ≈ 3.92%
    REQUIRE(pnl.current_drawdown_pct > 0.0);
}

TEST_CASE("Portfolio: Счётчик серии убытков работает", "[portfolio]") {
    auto portfolio = make_portfolio();

    // Открыть и закрыть с убытком 3 раза
    for (int i = 0; i < 3; ++i) {
        auto pos = make_position("SYM" + std::to_string(i));
        portfolio->open_position(pos);
        portfolio->close_position(Symbol("SYM" + std::to_string(i)), Price(49000.0), -100.0);
    }

    auto pnl = portfolio->pnl();
    REQUIRE(pnl.consecutive_losses == 3);

    // Прибыльная сделка сбрасывает счётчик
    auto pos = make_position("BTCUSDT");
    portfolio->open_position(pos);
    portfolio->close_position(Symbol("BTCUSDT"), Price(51000.0), 100.0);

    pnl = portfolio->pnl();
    REQUIRE(pnl.consecutive_losses == 0);
}

TEST_CASE("Portfolio: reset_daily очищает дневную статистику", "[portfolio]") {
    auto portfolio = make_portfolio();
    auto pos = make_position();
    portfolio->open_position(pos);
    portfolio->close_position(Symbol("BTCUSDT"), Price(51000.0), 100.0);

    auto pnl_before = portfolio->pnl();
    REQUIRE(pnl_before.trades_today == 1);
    REQUIRE_THAT(pnl_before.realized_pnl_today, WithinAbs(100.0, 0.01));

    portfolio->reset_daily();

    auto pnl_after = portfolio->pnl();
    REQUIRE(pnl_after.trades_today == 0);
    REQUIRE_THAT(pnl_after.realized_pnl_today, WithinAbs(0.0, 0.01));
    REQUIRE(pnl_after.consecutive_losses == 0);
}

TEST_CASE("Portfolio: Снимок содержит все позиции", "[portfolio]") {
    auto portfolio = make_portfolio();

    portfolio->open_position(make_position("BTCUSDT"));
    portfolio->open_position(make_position("ETHUSDT", Side::Buy, 1.0, 3000.0));

    auto snap = portfolio->snapshot();
    REQUIRE(snap.positions.size() == 2);
    REQUIRE(snap.total_capital == 10000.0);
    REQUIRE(snap.exposure.open_positions_count == 2);
}
