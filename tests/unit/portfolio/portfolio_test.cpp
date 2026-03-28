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

// ==================== Phase 1: Cash Reserve Accounting ====================

TEST_CASE("Portfolio: reserve_cash уменьшает available_cash", "[portfolio][cash]") {
    auto portfolio = make_portfolio(1000.0);

    bool ok = portfolio->reserve_cash(
        OrderId("ORD-1"), Symbol("BTCUSDT"), 500.0, 0.5, StrategyId("test"));
    REQUIRE(ok);

    auto ledger = portfolio->cash_ledger();
    REQUIRE_THAT(ledger.reserved_for_orders, WithinAbs(500.5, 0.01));
    REQUIRE_THAT(ledger.available_cash, WithinAbs(499.5, 0.01));
}

TEST_CASE("Portfolio: reserve_cash отклоняет при недостатке средств", "[portfolio][cash]") {
    auto portfolio = make_portfolio(100.0);

    bool ok = portfolio->reserve_cash(
        OrderId("ORD-1"), Symbol("BTCUSDT"), 200.0, 0.2, StrategyId("test"));
    REQUIRE_FALSE(ok);

    auto ledger = portfolio->cash_ledger();
    REQUIRE_THAT(ledger.reserved_for_orders, WithinAbs(0.0, 0.01));
    REQUIRE_THAT(ledger.available_cash, WithinAbs(100.0, 0.01));
}

TEST_CASE("Portfolio: release_cash возвращает резерв", "[portfolio][cash]") {
    auto portfolio = make_portfolio(1000.0);

    portfolio->reserve_cash(OrderId("ORD-1"), Symbol("BTCUSDT"), 300.0, 0.3);
    portfolio->release_cash(OrderId("ORD-1"));

    auto ledger = portfolio->cash_ledger();
    REQUIRE_THAT(ledger.reserved_for_orders, WithinAbs(0.0, 0.01));
    REQUIRE_THAT(ledger.available_cash, WithinAbs(1000.0, 0.01));
}

TEST_CASE("Portfolio: release_cash для неизвестного ордера — no-op", "[portfolio][cash]") {
    auto portfolio = make_portfolio(1000.0);
    portfolio->release_cash(OrderId("ORD-UNKNOWN"));

    auto ledger = portfolio->cash_ledger();
    REQUIRE_THAT(ledger.available_cash, WithinAbs(1000.0, 0.01));
}

TEST_CASE("Portfolio: multiple reserves и partial release", "[portfolio][cash]") {
    auto portfolio = make_portfolio(1000.0);

    portfolio->reserve_cash(OrderId("ORD-1"), Symbol("BTCUSDT"), 200.0, 0.2);
    portfolio->reserve_cash(OrderId("ORD-2"), Symbol("ETHUSDT"), 300.0, 0.3);

    auto ledger = portfolio->cash_ledger();
    REQUIRE_THAT(ledger.reserved_for_orders, WithinAbs(500.5, 0.01));
    REQUIRE_THAT(ledger.available_cash, WithinAbs(499.5, 0.01));

    // Release only one
    portfolio->release_cash(OrderId("ORD-1"));
    ledger = portfolio->cash_ledger();
    REQUIRE_THAT(ledger.reserved_for_orders, WithinAbs(300.3, 0.01));
}

TEST_CASE("Portfolio: record_fee уменьшает available_cash", "[portfolio][cash]") {
    auto portfolio = make_portfolio(1000.0);

    portfolio->record_fee(Symbol("BTCUSDT"), 1.5, OrderId("ORD-1"));

    auto ledger = portfolio->cash_ledger();
    REQUIRE_THAT(ledger.fees_accrued_today, WithinAbs(1.5, 0.01));
}

TEST_CASE("Portfolio: pending_orders возвращает список", "[portfolio][cash]") {
    auto portfolio = make_portfolio(1000.0);

    portfolio->reserve_cash(OrderId("ORD-1"), Symbol("BTCUSDT"), 200.0, 0.2);
    portfolio->reserve_cash(OrderId("ORD-2"), Symbol("ETHUSDT"), 300.0, 0.3);

    auto pending = portfolio->pending_orders();
    REQUIRE(pending.size() == 2);
}

TEST_CASE("Portfolio: event_log записывает события", "[portfolio][events]") {
    auto portfolio = make_portfolio(1000.0);
    auto pos = make_position("BTCUSDT", Side::Buy, 0.1, 50000.0);

    portfolio->open_position(pos);
    portfolio->reserve_cash(OrderId("ORD-1"), Symbol("ETHUSDT"), 200.0, 0.2);
    portfolio->release_cash(OrderId("ORD-1"));
    portfolio->record_fee(Symbol("BTCUSDT"), 0.5);
    portfolio->close_position(Symbol("BTCUSDT"), Price(51000.0), 100.0);

    auto events = portfolio->recent_events(100);
    REQUIRE(events.size() >= 4);
}

TEST_CASE("Portfolio: check_invariants проходит после нормальных операций", "[portfolio][cash]") {
    auto portfolio = make_portfolio(10000.0);

    portfolio->reserve_cash(OrderId("ORD-1"), Symbol("BTCUSDT"), 5000.0, 5.0);
    REQUIRE(portfolio->check_invariants());

    portfolio->release_cash(OrderId("ORD-1"));
    REQUIRE(portfolio->check_invariants());
}

TEST_CASE("Portfolio: snapshot содержит cash ledger данные", "[portfolio][cash]") {
    auto portfolio = make_portfolio(1000.0);

    portfolio->reserve_cash(OrderId("ORD-1"), Symbol("BTCUSDT"), 300.0, 0.3);

    auto snap = portfolio->snapshot();
    REQUIRE(snap.pending_buy_count == 1);
    REQUIRE_THAT(snap.cash.reserved_for_orders, WithinAbs(300.3, 0.01));
    REQUIRE(snap.pending_orders.size() == 1);
}

TEST_CASE("Portfolio: reset_daily очищает fee и cash счётчики", "[portfolio][cash]") {
    auto portfolio = make_portfolio(1000.0);

    portfolio->record_fee(Symbol("BTCUSDT"), 2.0);
    portfolio->add_realized_pnl(50.0);

    portfolio->reset_daily();

    auto ledger = portfolio->cash_ledger();
    REQUIRE_THAT(ledger.fees_accrued_today, WithinAbs(0.0, 0.01));

    auto pnl_after = portfolio->pnl();
    REQUIRE_THAT(pnl_after.realized_pnl_today, WithinAbs(0.0, 0.01));
}

// ========== Regression: reduce_position (partial close) ==========

TEST_CASE("Portfolio: reduce_position — частичное закрытие 50%", "[portfolio][regression]") {
    auto portfolio = make_portfolio();
    auto pos = make_position("BTCUSDT", Side::Buy, 1.0, 100.0);  // 1 BTC @ $100
    portfolio->open_position(pos);

    // Цена выросла до $110, продаём 50%
    portfolio->update_price(Symbol("BTCUSDT"), Price(110.0));
    double pnl_half = (110.0 - 100.0) * 0.5;  // $5 profit on 0.5 BTC
    double remaining = portfolio->reduce_position(
        Symbol("BTCUSDT"), Quantity(0.5), Price(110.0), pnl_half);

    REQUIRE_THAT(remaining, WithinAbs(0.5, 1e-9));
    REQUIRE(portfolio->has_position(Symbol("BTCUSDT")));

    auto after = portfolio->get_position(Symbol("BTCUSDT"));
    REQUIRE(after.has_value());
    REQUIRE_THAT(after->size.get(), WithinAbs(0.5, 1e-9));
    // avg_entry_price сохраняется — это цена входа, не средняя
    REQUIRE_THAT(after->avg_entry_price.get(), WithinAbs(100.0, 1e-9));
}

TEST_CASE("Portfolio: reduce_position — полное закрытие удаляет позицию", "[portfolio][regression]") {
    auto portfolio = make_portfolio();
    auto pos = make_position("ETHUSDT", Side::Buy, 2.0, 3000.0);
    portfolio->open_position(pos);

    double pnl = (3100.0 - 3000.0) * 2.0;  // $200 profit
    double remaining = portfolio->reduce_position(
        Symbol("ETHUSDT"), Quantity(2.0), Price(3100.0), pnl);

    REQUIRE_THAT(remaining, WithinAbs(0.0, 1e-9));
    REQUIRE_FALSE(portfolio->has_position(Symbol("ETHUSDT")));
}

TEST_CASE("Portfolio: reduce_position — несколько последовательных partial closes", "[portfolio][regression]") {
    auto portfolio = make_portfolio();
    auto pos = make_position("BTCUSDT", Side::Buy, 1.0, 100.0);
    portfolio->open_position(pos);

    // Close 30%
    double pnl1 = (105.0 - 100.0) * 0.3;  // $1.50
    double rem1 = portfolio->reduce_position(
        Symbol("BTCUSDT"), Quantity(0.3), Price(105.0), pnl1);
    REQUIRE_THAT(rem1, WithinAbs(0.7, 1e-9));

    // Close another 30%
    double pnl2 = (108.0 - 100.0) * 0.3;  // $2.40
    double rem2 = portfolio->reduce_position(
        Symbol("BTCUSDT"), Quantity(0.3), Price(108.0), pnl2);
    REQUIRE_THAT(rem2, WithinAbs(0.4, 1e-9));

    // Close remaining 40%
    double pnl3 = (110.0 - 100.0) * 0.4;  // $4.00
    double rem3 = portfolio->reduce_position(
        Symbol("BTCUSDT"), Quantity(0.4), Price(110.0), pnl3);
    REQUIRE_THAT(rem3, WithinAbs(0.0, 1e-9));
    REQUIRE_FALSE(portfolio->has_position(Symbol("BTCUSDT")));

    // Проверяем суммарный realized PnL
    auto pnl = portfolio->pnl();
    REQUIRE_THAT(pnl.realized_pnl_today, WithinAbs(pnl1 + pnl2 + pnl3, 0.01));
}

TEST_CASE("Portfolio: reduce_position — reduce больше чем позиция обрезает до размера", "[portfolio][regression]") {
    auto portfolio = make_portfolio();
    auto pos = make_position("BTCUSDT", Side::Buy, 0.5, 200.0);
    portfolio->open_position(pos);

    // Пытаемся продать 1.0 при позиции 0.5
    double remaining = portfolio->reduce_position(
        Symbol("BTCUSDT"), Quantity(1.0), Price(210.0), 5.0);

    REQUIRE_THAT(remaining, WithinAbs(0.0, 1e-9));
    REQUIRE_FALSE(portfolio->has_position(Symbol("BTCUSDT")));
}

TEST_CASE("Portfolio: reduce_position — несуществующая позиция возвращает 0", "[portfolio][regression]") {
    auto portfolio = make_portfolio();

    double remaining = portfolio->reduce_position(
        Symbol("BTCUSDT"), Quantity(1.0), Price(100.0), 0.0);

    REQUIRE_THAT(remaining, WithinAbs(0.0, 1e-9));
}

TEST_CASE("Portfolio: reduce_position — realized PnL корректно аккумулируется в cash ledger", "[portfolio][regression]") {
    auto portfolio = make_portfolio(10000.0);
    auto pos = make_position("BTCUSDT", Side::Buy, 1.0, 100.0);
    portfolio->open_position(pos);

    double pnl = 5.0;  // $5 profit on partial
    portfolio->reduce_position(Symbol("BTCUSDT"), Quantity(0.5), Price(110.0), pnl);

    auto ledger = portfolio->cash_ledger();
    REQUIRE_THAT(ledger.realized_pnl_net, WithinAbs(5.0, 0.01));
    REQUIRE_THAT(ledger.total_cash, WithinAbs(10005.0, 0.01));
}
