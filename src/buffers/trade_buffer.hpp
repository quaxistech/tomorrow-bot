#pragma once
#include "ring_buffer.hpp"
#include "normalizer/normalized_events.hpp"
#include "common/types.hpp"
#include <algorithm>
#include <vector>
#include <cstddef>

namespace tb::buffers {

// Торговая запись для буфера
struct TradeRecord {
    tb::Timestamp ts{0};
    double price{0.0};
    double size{0.0};
    tb::Side side{tb::Side::Buy};
    bool is_aggressive{false};
};

template<std::size_t Capacity = 1000>
class TradeBuffer {
public:
    void push(TradeRecord trade) { buffer_.push(std::move(trade)); }

    void push(const normalizer::NormalizedTrade& trade) {
        TradeRecord r;
        r.ts = trade.envelope.exchange_ts;
        r.price = trade.price.get();
        r.size = trade.size.get();
        r.side = trade.side;
        r.is_aggressive = trade.is_aggressive;
        buffer_.push(r);
    }

    // VWAP последних n записей
    double vwap(std::size_t n = Capacity) const {
        n = std::min(n, buffer_.size());
        if (n == 0) return 0.0;
        double sum_pv = 0.0, sum_v = 0.0;
        std::size_t start = buffer_.size() > n ? buffer_.size() - n : 0;
        for (std::size_t i = start; i < buffer_.size(); ++i) {
            const auto& t = buffer_[i];
            sum_pv += t.price * t.size;
            sum_v  += t.size;
        }
        return sum_v > 0.0 ? sum_pv / sum_v : 0.0;
    }

    // buy_vol / sell_vol за последние n записей.
    // Ограничен сверху: односторонний поток даёт cap, а не sentinel.
    // Cap = 100.0: предотвращает blow-up в downstream ML/стратегиях,
    // при этом 100:1 уже представляет экстремальный дисбаланс.
    double buy_sell_ratio(std::size_t n = Capacity) const {
        n = std::min(n, buffer_.size());
        if (n == 0) return 1.0;
        double buy_vol = 0.0, sell_vol = 0.0;
        std::size_t start = buffer_.size() > n ? buffer_.size() - n : 0;
        for (std::size_t i = start; i < buffer_.size(); ++i) {
            const auto& t = buffer_[i];
            if (t.side == tb::Side::Buy) buy_vol += t.size;
            else sell_vol += t.size;
        }
        if (sell_vol > 0.0) return std::min(buy_vol / sell_vol, 100.0);
        return buy_vol > 0.0 ? 100.0 : 1.0;
    }

    // Доля агрессивного объёма [0, 1] — volume-weighted.
    // Kyle (1985): price impact пропорционален размеру сделки, не количеству.
    // Для скальпинга volume-weighted агрессивный поток корректнее
    // отражает реальное давление на стакан.
    double aggressive_flow(std::size_t n = Capacity) const {
        n = std::min(n, buffer_.size());
        if (n == 0) return 0.5;
        double agg_volume = 0.0, total_volume = 0.0;
        std::size_t start = buffer_.size() > n ? buffer_.size() - n : 0;
        for (std::size_t i = start; i < buffer_.size(); ++i) {
            const auto& t = buffer_[i];
            total_volume += t.size;
            if (t.is_aggressive) agg_volume += t.size;
        }
        return total_volume > 0.0 ? agg_volume / total_volume : 0.5;
    }

    std::size_t size() const noexcept { return buffer_.size(); }
    bool empty() const noexcept { return buffer_.empty(); }

private:
    RingBuffer<TradeRecord, Capacity> buffer_;
};

} // namespace tb::buffers
