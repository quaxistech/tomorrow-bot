#pragma once
#include "execution/order_types.hpp"
#include "execution/order_fsm.hpp"
#include "strategy/strategy_types.hpp"
#include "risk/risk_types.hpp"
#include "execution_alpha/execution_alpha_types.hpp"
#include "portfolio/portfolio_engine.hpp"
#include "common/result.hpp"
#include "logging/logger.hpp"
#include "clock/clock.hpp"
#include "metrics/metrics_registry.hpp"
#include <memory>
#include <mutex>
#include <atomic>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace tb::uncertainty { struct UncertaintySnapshot; }

namespace tb::execution {

/// Интерфейс отправки ордеров на биржу (абстракция для тестирования)
class IOrderSubmitter {
public:
    virtual ~IOrderSubmitter() = default;

    /// Отправить ордер
    virtual OrderSubmitResult submit_order(const OrderRecord& order) = 0;

    /// Отменить ордер. Symbol обязателен для Bitget API.
    virtual bool cancel_order(const OrderId& order_id, const Symbol& symbol) = 0;

    /// Запросить реальную цену исполнения market-ордера с биржи.
    /// Возвращает avg fill price > 0 при успехе, 0.0 при неудаче.
    virtual Price query_order_fill_price(const OrderId& exchange_order_id, const Symbol& symbol) { (void)exchange_order_id; (void)symbol; return Price(0.0); }
};

/// Реализация для paper/shadow торговли — немедленно подтверждает ордера без отправки на биржу
class PaperOrderSubmitter : public IOrderSubmitter {
public:
    OrderSubmitResult submit_order(const OrderRecord& order) override;
    bool cancel_order(const OrderId& order_id, const Symbol& symbol) override;

private:
    std::atomic<int64_t> next_exchange_id_{1};
};

/// Движок исполнения ордеров
class ExecutionEngine {
public:
    ExecutionEngine(std::shared_ptr<IOrderSubmitter> submitter,
                    std::shared_ptr<portfolio::IPortfolioEngine> portfolio,
                    std::shared_ptr<logging::ILogger> logger,
                    std::shared_ptr<clock::IClock> clock,
                    std::shared_ptr<metrics::IMetricsRegistry> metrics);

    /// Отправить ордер на основе одобренного интента + risk decision
    [[nodiscard]] Result<OrderId> execute(const strategy::TradeIntent& intent,
                            const risk::RiskDecision& risk_decision,
                            const execution_alpha::ExecutionAlphaResult& exec_alpha,
                            const uncertainty::UncertaintySnapshot& uncertainty);

    /// Запросить отмену ордера
    VoidResult cancel(const OrderId& order_id);

    /// Обновить состояние ордера (от биржи)
    void on_order_update(const OrderId& order_id, OrderState new_state,
                         Quantity filled_qty = Quantity(0.0),
                         Price fill_price = Price(0.0));

    /// Получить запись ордера
    std::optional<OrderRecord> get_order(const OrderId& order_id) const;

    /// Все активные ордера
    std::vector<OrderRecord> active_orders() const;

    /// Проверка дублирования (не отправлять одинаковый интент дважды)
    [[nodiscard]] bool is_duplicate(const strategy::TradeIntent& intent);

    /// Обработать fill event (partial или full)
    void on_fill_event(const FillEvent& fill);

    /// Проверить ордера с истекшим timeout и отменить
    std::vector<OrderId> cancel_timed_out_orders(int64_t max_open_duration_ms);

    /// Получить все ордера для символа
    [[nodiscard]] std::vector<OrderRecord> orders_for_symbol(const Symbol& symbol) const;

    /// Получить статистику fills для ордера
    [[nodiscard]] std::optional<OrderExecutionInfo> get_execution_info(const OrderId& order_id) const;

    /// Установить partial fill policy по умолчанию
    void set_default_fill_policy(PartialFillPolicy policy);

    /// Установить leverage для фьючерсов (влияет на cash reservation)
    void set_leverage(double leverage) { leverage_ = std::max(leverage, 1.0); }

    /// Удалить ордера в терминальных состояниях старше max_age_ns.
    /// Предотвращает утечку памяти на долгоработающем боте.
    /// @return Количество удалённых ордеров
    size_t cleanup_terminal_orders(int64_t max_age_ns);

private:
    /// Создать запись ордера из интента, решения риска и параметров исполнения
    OrderRecord create_order_record(const strategy::TradeIntent& intent,
                                     const risk::RiskDecision& risk_decision,
                                     const execution_alpha::ExecutionAlphaResult& exec_alpha,
                                     const uncertainty::UncertaintySnapshot& uncertainty);

    /// Генерировать уникальный идентификатор ордера
    std::string generate_order_id();

    /// Очистить записи из recent_intents_ старше window_ns наносекунд.
    /// MUST be called under mutex_.
    void cleanup_old_intents(int64_t now_ns);

    /// Обновить портфель при SELL fill: уменьшить позицию, рассчитать PnL, зафиксировать комиссию.
    /// Вызывается из execute(), on_order_update() и on_fill_event(). Не блокирует mutex.
    void apply_sell_fill_to_portfolio(const OrderRecord& order);

    /// Обновить портфель при BUY fill: открыть позицию, освободить резерв, зафиксировать комиссию.
    /// Вызывается из execute(), on_order_update() и on_fill_event(). Не блокирует mutex.
    void apply_buy_fill_to_portfolio(const OrderRecord& order);

    std::shared_ptr<IOrderSubmitter> submitter_;
    std::shared_ptr<portfolio::IPortfolioEngine> portfolio_;
    std::shared_ptr<logging::ILogger> logger_;
    std::shared_ptr<clock::IClock> clock_;
    std::shared_ptr<metrics::IMetricsRegistry> metrics_;

    std::unordered_map<std::string, OrderRecord> orders_;      ///< По order_id
    std::unordered_map<std::string, OrderFSM> order_fsms_;     ///< FSM для каждого ордера
    std::unordered_map<std::string, int64_t> recent_intents_;  ///< Для обнаружения дублей (key -> timestamp_ns)
    mutable std::mutex mutex_;
    std::atomic<int> next_order_seq_{1};
    PartialFillPolicy default_fill_policy_{PartialFillPolicy::WaitForFull};
    double leverage_{1.0};  ///< Leverage for futures margin calculation

    /// Набор order_id для которых fill уже применён к портфелю (идемпотентность)
    std::unordered_set<std::string> fill_applied_;
};

} // namespace tb::execution
