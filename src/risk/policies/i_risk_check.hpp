#pragma once
#include "risk/risk_types.hpp"
#include "risk/risk_context.hpp"
#include <string>
#include <string_view>

namespace tb::risk {

/// Интерфейс одной проверки риска (policy-based)
class IRiskCheck {
public:
    virtual ~IRiskCheck() = default;

    /// Имя проверки (для audit trail)
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;

    /// Выполнить проверку и обновить decision
    virtual void evaluate(const RiskContext& ctx, RiskDecision& decision) = 0;
};

} // namespace tb::risk
