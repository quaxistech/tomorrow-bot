#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "leverage/leverage_engine.hpp"

using namespace tb;
using namespace tb::leverage;
using namespace tb::config;
using Catch::Matchers::WithinAbs;

namespace {

FuturesConfig make_default_config() {
    FuturesConfig cfg;
    cfg.enabled = true;
    cfg.product_type = "USDT-FUTURES";
    cfg.margin_coin = "USDT";
    cfg.margin_mode = "isolated";
    cfg.default_leverage = 5;
    cfg.max_leverage = 20;
    cfg.leverage_trending = 10;
    cfg.leverage_ranging = 5;
    cfg.leverage_volatile = 3;
    cfg.leverage_stress = 1;
    cfg.liquidation_buffer_pct = 5.0;
    cfg.max_leverage_drawdown_scale = 0.5;
    cfg.funding_rate_threshold = 0.03;
    cfg.funding_rate_penalty = 0.5;
    cfg.maintenance_margin_rate = 0.004;
    return cfg;
}

LeverageContext make_default_context() {
    LeverageContext ctx;
    ctx.regime = RegimeLabel::Trending;
    ctx.uncertainty = UncertaintyLevel::Low;
    ctx.atr_normalized = 0.01;
    ctx.drawdown_pct = 0.0;
    ctx.adversarial_severity = 0.0;
    ctx.conviction = 0.7;
    ctx.funding_rate = 0.0;
    ctx.position_side = PositionSide::Long;
    ctx.entry_price = 50000.0;
    return ctx;
}

} // anonymous namespace

// ============================================================
// Базовое плечо по режиму
// ============================================================

TEST_CASE("Leverage: базовое плечо зависит от режима рынка", "[leverage][regime]") {
    auto cfg = make_default_config();
    LeverageEngine engine(cfg);

    SECTION("Trending → leverage_trending (10)") {
        auto ctx = make_default_context();
        ctx.regime = RegimeLabel::Trending;
        auto d = engine.compute_leverage(ctx);
        CHECK(d.base_leverage == 10);
    }

    SECTION("Ranging → leverage_ranging (5)") {
        auto ctx = make_default_context();
        ctx.regime = RegimeLabel::Ranging;
        auto d = engine.compute_leverage(ctx);
        CHECK(d.base_leverage == 5);
    }

    SECTION("Volatile → leverage_volatile (3)") {
        auto ctx = make_default_context();
        ctx.regime = RegimeLabel::Volatile;
        auto d = engine.compute_leverage(ctx);
        CHECK(d.base_leverage == 3);
    }

    SECTION("Unclear → leverage_stress (1)") {
        auto ctx = make_default_context();
        ctx.regime = RegimeLabel::Unclear;
        auto d = engine.compute_leverage(ctx);
        CHECK(d.base_leverage == 1);
    }
}

// ============================================================
// Множитель от волатильности (гладкий)
// ============================================================

TEST_CASE("Leverage: volatility_multiplier плавно снижает плечо при росте ATR", "[leverage][volatility]") {
    auto cfg = make_default_config();
    LeverageEngine engine(cfg);

    SECTION("Низкая волатильность (ATR ≤ 0.5%) → множитель 1.0") {
        auto ctx = make_default_context();
        ctx.atr_normalized = 0.003;
        auto d = engine.compute_leverage(ctx);
        CHECK_THAT(d.volatility_factor, WithinAbs(1.0, 0.01));
    }

    SECTION("Средняя волатильность (ATR = 1%) → множитель ~0.8") {
        auto ctx = make_default_context();
        ctx.atr_normalized = 0.01;
        auto d = engine.compute_leverage(ctx);
        // (0.01-0.005)/(0.02-0.005) = 0.333 → lerp(1.0, 0.7, 0.333) ≈ 0.90
        CHECK(d.volatility_factor > 0.85);
        CHECK(d.volatility_factor < 1.0);
    }

    SECTION("Высокая волатильность (ATR = 3%) → множитель ~0.55") {
        auto ctx = make_default_context();
        ctx.atr_normalized = 0.03;
        auto d = engine.compute_leverage(ctx);
        CHECK(d.volatility_factor > 0.4);
        CHECK(d.volatility_factor < 0.7);
    }

    SECTION("Экстремальная волатильность (ATR = 6%) → множитель ~0.3") {
        auto ctx = make_default_context();
        ctx.atr_normalized = 0.06;
        auto d = engine.compute_leverage(ctx);
        CHECK(d.volatility_factor > 0.2);
        CHECK(d.volatility_factor < 0.4);
    }

    SECTION("Кризисная волатильность (ATR > 10%) → множитель ≤ 0.15") {
        auto ctx = make_default_context();
        ctx.atr_normalized = 0.12;
        auto d = engine.compute_leverage(ctx);
        CHECK(d.volatility_factor <= 0.15);
        CHECK(d.volatility_factor >= 0.10);
    }

    SECTION("Плавность: нет резких скачков") {
        auto ctx = make_default_context();
        double prev = 1.0;
        for (double atr = 0.002; atr <= 0.10; atr += 0.001) {
            ctx.atr_normalized = atr;
            auto d = engine.compute_leverage(ctx);
            // Каждый шаг в 0.1% ATR не должен менять множитель более чем на 0.05
            CHECK(std::abs(d.volatility_factor - prev) < 0.06);
            prev = d.volatility_factor;
        }
    }
}

// ============================================================
// Множитель от просадки
// ============================================================

TEST_CASE("Leverage: drawdown_multiplier экспоненциально снижает плечо", "[leverage][drawdown]") {
    auto cfg = make_default_config();
    LeverageEngine engine(cfg);

    SECTION("Нет просадки → 1.0") {
        auto ctx = make_default_context();
        ctx.drawdown_pct = 0.0;
        auto d = engine.compute_leverage(ctx);
        CHECK_THAT(d.drawdown_factor, WithinAbs(1.0, 0.01));
    }

    SECTION("5% просадка → ~0.71") {
        auto ctx = make_default_context();
        ctx.drawdown_pct = 5.0;
        auto d = engine.compute_leverage(ctx);
        CHECK(d.drawdown_factor > 0.6);
        CHECK(d.drawdown_factor < 0.8);
    }

    SECTION("10% просадка → ~0.50") {
        auto ctx = make_default_context();
        ctx.drawdown_pct = 10.0;
        auto d = engine.compute_leverage(ctx);
        CHECK(d.drawdown_factor > 0.4);
        CHECK(d.drawdown_factor < 0.6);
    }

    SECTION(">30% просадка → clamped к 0.10") {
        auto ctx = make_default_context();
        ctx.drawdown_pct = 40.0;
        auto d = engine.compute_leverage(ctx);
        CHECK_THAT(d.drawdown_factor, WithinAbs(0.10, 0.01));
    }
}

// ============================================================
// Множитель от conviction (гладкий)
// ============================================================

TEST_CASE("Leverage: conviction_multiplier бонус за высокую убеждённость", "[leverage][conviction]") {
    auto cfg = make_default_config();
    LeverageEngine engine(cfg);

    SECTION("Нулевая conviction → 0.40") {
        auto ctx = make_default_context();
        ctx.conviction = 0.0;
        auto d = engine.compute_leverage(ctx);
        CHECK_THAT(d.conviction_factor, WithinAbs(0.40, 0.02));
    }

    SECTION("Средняя conviction 0.5 → ~0.80") {
        auto ctx = make_default_context();
        ctx.conviction = 0.5;
        auto d = engine.compute_leverage(ctx);
        CHECK(d.conviction_factor > 0.70);
        CHECK(d.conviction_factor < 0.90);
    }

    SECTION("Conviction 0.7 → 1.00") {
        auto ctx = make_default_context();
        ctx.conviction = 0.7;
        auto d = engine.compute_leverage(ctx);
        CHECK_THAT(d.conviction_factor, WithinAbs(1.0, 0.02));
    }

    SECTION("Высокая conviction 0.9 → >1.0 (бонус)") {
        auto ctx = make_default_context();
        ctx.conviction = 0.9;
        auto d = engine.compute_leverage(ctx);
        CHECK(d.conviction_factor > 1.0);
        CHECK(d.conviction_factor < 1.3);
    }

    SECTION("Максимальная conviction 1.0 → 1.30") {
        auto ctx = make_default_context();
        ctx.conviction = 1.0;
        auto d = engine.compute_leverage(ctx);
        CHECK_THAT(d.conviction_factor, WithinAbs(1.30, 0.02));
    }
}

// ============================================================
// Множитель от funding rate
// ============================================================

TEST_CASE("Leverage: funding_multiplier снижает плечо при оплате фандинга", "[leverage][funding]") {
    auto cfg = make_default_config();
    LeverageEngine engine(cfg);

    SECTION("Нулевой funding → 1.0") {
        auto ctx = make_default_context();
        ctx.funding_rate = 0.0;
        auto d = engine.compute_leverage(ctx);
        CHECK_THAT(d.funding_factor, WithinAbs(1.0, 0.01));
    }

    SECTION("Funding ниже порога → 1.0") {
        auto ctx = make_default_context();
        ctx.funding_rate = 0.02; // < 0.03 threshold
        ctx.position_side = PositionSide::Long;
        auto d = engine.compute_leverage(ctx);
        CHECK_THAT(d.funding_factor, WithinAbs(1.0, 0.01));
    }

    SECTION("Long + положительный высокий funding → штраф") {
        auto ctx = make_default_context();
        ctx.funding_rate = 0.06; // 2× threshold → penalty
        ctx.position_side = PositionSide::Long;
        auto d = engine.compute_leverage(ctx);
        CHECK(d.funding_factor < 1.0);
        CHECK(d.funding_factor > 0.25);
    }

    SECTION("Short + положительный funding → мы получаем → 1.0") {
        auto ctx = make_default_context();
        ctx.funding_rate = 0.06;
        ctx.position_side = PositionSide::Short;
        auto d = engine.compute_leverage(ctx);
        CHECK_THAT(d.funding_factor, WithinAbs(1.0, 0.01));
    }

    SECTION("Short + отрицательный funding → мы платим → штраф") {
        auto ctx = make_default_context();
        ctx.funding_rate = -0.06;
        ctx.position_side = PositionSide::Short;
        auto d = engine.compute_leverage(ctx);
        CHECK(d.funding_factor < 1.0);
    }
}

// ============================================================
// Множитель от adversarial severity (гладкий)
// ============================================================

TEST_CASE("Leverage: adversarial_multiplier плавно снижает плечо при угрозе", "[leverage][adversarial]") {
    auto cfg = make_default_config();
    LeverageEngine engine(cfg);

    SECTION("Нет угрозы → 1.0") {
        auto ctx = make_default_context();
        ctx.adversarial_severity = 0.0;
        auto d = engine.compute_leverage(ctx);
        CHECK_THAT(d.adversarial_factor, WithinAbs(1.0, 0.01));
    }

    SECTION("Средняя угроза 0.5 → ~0.57") {
        auto ctx = make_default_context();
        ctx.adversarial_severity = 0.5;
        auto d = engine.compute_leverage(ctx);
        // lerp(1.0, 0.15, 0.5) = 0.575
        CHECK(d.adversarial_factor > 0.50);
        CHECK(d.adversarial_factor < 0.65);
    }

    SECTION("Максимальная угроза 1.0 → 0.15") {
        auto ctx = make_default_context();
        ctx.adversarial_severity = 1.0;
        auto d = engine.compute_leverage(ctx);
        CHECK_THAT(d.adversarial_factor, WithinAbs(0.15, 0.02));
    }

    SECTION("Плавность: нет резких скачков") {
        auto ctx = make_default_context();
        double prev = 1.0;
        for (double s = 0.0; s <= 1.0; s += 0.05) {
            ctx.adversarial_severity = s;
            auto d = engine.compute_leverage(ctx);
            CHECK(std::abs(d.adversarial_factor - prev) < 0.10);
            prev = d.adversarial_factor;
        }
    }
}

// ============================================================
// Множитель от неопределённости
// ============================================================

TEST_CASE("Leverage: uncertainty_multiplier снижает плечо при высокой неопределённости", "[leverage][uncertainty]") {
    auto cfg = make_default_config();
    LeverageEngine engine(cfg);

    SECTION("Low → 1.0") {
        auto ctx = make_default_context();
        ctx.uncertainty = UncertaintyLevel::Low;
        auto d = engine.compute_leverage(ctx);
        CHECK_THAT(d.uncertainty_factor, WithinAbs(1.0, 0.01));
    }

    SECTION("Moderate → 0.80") {
        auto ctx = make_default_context();
        ctx.uncertainty = UncertaintyLevel::Moderate;
        auto d = engine.compute_leverage(ctx);
        CHECK_THAT(d.uncertainty_factor, WithinAbs(0.80, 0.02));
    }

    SECTION("Extreme → 0.25") {
        auto ctx = make_default_context();
        ctx.uncertainty = UncertaintyLevel::Extreme;
        auto d = engine.compute_leverage(ctx);
        CHECK_THAT(d.uncertainty_factor, WithinAbs(0.25, 0.02));
    }
}

// ============================================================
// Клемпинг результата
// ============================================================

TEST_CASE("Leverage: результат клемпится в [1, max_leverage]", "[leverage][clamp]") {
    auto cfg = make_default_config();
    LeverageEngine engine(cfg);

    SECTION("Плечо не опускается ниже 1") {
        auto ctx = make_default_context();
        ctx.regime = RegimeLabel::Unclear; // base = 1
        ctx.atr_normalized = 0.08;        // vol mult ~0.20
        ctx.adversarial_severity = 0.9;   // adv mult ~0.24
        ctx.conviction = 0.1;             // conv mult ~0.49
        ctx.uncertainty = UncertaintyLevel::Extreme; // unc mult = 0.25
        auto d = engine.compute_leverage(ctx);
        CHECK(d.leverage >= 1);
    }

    SECTION("Плечо не превышает max_leverage") {
        auto ctx = make_default_context();
        ctx.regime = RegimeLabel::Trending; // base = 10
        ctx.conviction = 1.0;              // conv mult = 1.30
        ctx.atr_normalized = 0.001;        // vol = 1.0
        auto d = engine.compute_leverage(ctx);
        CHECK(d.leverage <= cfg.max_leverage);
    }
}

// ============================================================
// Ликвидационная цена
// ============================================================

TEST_CASE("Leverage: liquidation_price корректна для Long и Short", "[leverage][liquidation]") {

    SECTION("Long: ликвидация ниже entry") {
        LiquidationParams p;
        p.entry_price = 50000.0;
        p.position_side = PositionSide::Long;
        p.leverage = 10;
        p.maintenance_margin_rate = 0.004;

        double liq = LeverageEngine::compute_liquidation_price(p);
        CHECK(liq < 50000.0);
        CHECK(liq > 0.0);
        // liq ≈ 50000 × (1 - 0.1 + 0.004 + 0.001) = 50000 × 0.905 = 45250
        CHECK_THAT(liq, WithinAbs(45250.0, 100.0));
    }

    SECTION("Short: ликвидация выше entry") {
        LiquidationParams p;
        p.entry_price = 50000.0;
        p.position_side = PositionSide::Short;
        p.leverage = 10;
        p.maintenance_margin_rate = 0.004;

        double liq = LeverageEngine::compute_liquidation_price(p);
        CHECK(liq > 50000.0);
        // liq ≈ 50000 × (1 + 0.1 - 0.004 - 0.001) = 50000 × 1.095 = 54750
        CHECK_THAT(liq, WithinAbs(54750.0, 100.0));
    }

    SECTION("Leverage 1 → максимальный буфер до ликвидации") {
        LiquidationParams p;
        p.entry_price = 50000.0;
        p.position_side = PositionSide::Long;
        p.leverage = 1;
        p.maintenance_margin_rate = 0.004;

        double liq = LeverageEngine::compute_liquidation_price(p);
        // liq ≈ 50000 × (1 - 1.0 + 0.004 + 0.001) = 50000 × 0.005 = 250
        CHECK(liq < 1000.0);
    }

    SECTION("Invalid leverage → 0") {
        LiquidationParams p;
        p.leverage = 0;
        CHECK(LeverageEngine::compute_liquidation_price(p) == 0.0);
    }
}

// ============================================================
// Проверка безопасности
// ============================================================

TEST_CASE("Leverage: is_liquidation_safe", "[leverage][safety]") {

    SECTION("Long: достаточный буфер → safe") {
        CHECK(LeverageEngine::is_liquidation_safe(50000.0, 45000.0, PositionSide::Long, 5.0));
    }

    SECTION("Long: недостаточный буфер → unsafe") {
        CHECK_FALSE(LeverageEngine::is_liquidation_safe(50000.0, 48000.0, PositionSide::Long, 5.0));
    }

    SECTION("Short: достаточный буфер → safe") {
        CHECK(LeverageEngine::is_liquidation_safe(50000.0, 55000.0, PositionSide::Short, 5.0));
    }

    SECTION("Short: недостаточный буфер → unsafe") {
        CHECK_FALSE(LeverageEngine::is_liquidation_safe(50000.0, 51000.0, PositionSide::Short, 5.0));
    }

    SECTION("Невалидные цены → unsafe") {
        CHECK_FALSE(LeverageEngine::is_liquidation_safe(0.0, 45000.0, PositionSide::Long, 5.0));
        CHECK_FALSE(LeverageEngine::is_liquidation_safe(50000.0, 0.0, PositionSide::Long, 5.0));
    }
}

// ============================================================
// Интеграционный тест: композитный расчёт
// ============================================================

TEST_CASE("Leverage: полный расчёт через LeverageContext", "[leverage][integration]") {
    auto cfg = make_default_config();
    LeverageEngine engine(cfg);

    SECTION("Идеальные условия → высокое плечо") {
        LeverageContext ctx;
        ctx.regime = RegimeLabel::Trending;
        ctx.uncertainty = UncertaintyLevel::Low;
        ctx.atr_normalized = 0.005;  // low vol
        ctx.drawdown_pct = 0.0;
        ctx.adversarial_severity = 0.0;
        ctx.conviction = 0.95;       // high conviction
        ctx.funding_rate = 0.0;
        ctx.position_side = PositionSide::Long;
        ctx.entry_price = 50000.0;

        auto d = engine.compute_leverage(ctx);
        CHECK(d.leverage >= 8); // base 10 × high factors
        CHECK(d.is_safe);
        CHECK(!d.rationale.empty());
    }

    SECTION("Плохие условия → минимальное плечо") {
        LeverageContext ctx;
        ctx.regime = RegimeLabel::Unclear;
        ctx.uncertainty = UncertaintyLevel::Extreme;
        ctx.atr_normalized = 0.06;
        ctx.drawdown_pct = 15.0;
        ctx.adversarial_severity = 0.8;
        ctx.conviction = 0.2;
        ctx.funding_rate = 0.08;
        ctx.position_side = PositionSide::Long;
        ctx.entry_price = 50000.0;

        auto d = engine.compute_leverage(ctx);
        CHECK(d.leverage == 1);
    }

    SECTION("Unsafe ликвидация при высоком плече и малом буфере") {
        auto cfg2 = make_default_config();
        cfg2.liquidation_buffer_pct = 15.0; // требуем 15% буфер
        LeverageEngine engine2(cfg2);

        LeverageContext ctx;
        ctx.regime = RegimeLabel::Trending;
        ctx.uncertainty = UncertaintyLevel::Low;
        ctx.atr_normalized = 0.005;
        ctx.drawdown_pct = 0.0;
        ctx.adversarial_severity = 0.0;
        ctx.conviction = 0.95;
        ctx.funding_rate = 0.0;
        ctx.position_side = PositionSide::Long;
        ctx.entry_price = 50000.0;

        auto d = engine2.compute_leverage(ctx);
        // При leverage ~12 и mmr 0.004, буфер ~8.5% < 15% → unsafe
        if (d.leverage >= 8) {
            CHECK_FALSE(d.is_safe);
        }
    }
}

// ============================================================
// update_config
// ============================================================

TEST_CASE("Leverage: update_config изменяет поведение", "[leverage][config]") {
    auto cfg = make_default_config();
    LeverageEngine engine(cfg);

    auto ctx = make_default_context();
    auto d1 = engine.compute_leverage(ctx);

    // Снижаем max_leverage
    cfg.max_leverage = 5;
    cfg.leverage_trending = 5;
    engine.update_config(cfg);

    auto d2 = engine.compute_leverage(ctx);
    CHECK(d2.leverage <= 5);
    CHECK(d2.base_leverage == 5);
}
