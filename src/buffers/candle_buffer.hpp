#pragma once
#include "ring_buffer.hpp"
#include "normalizer/normalized_events.hpp"
#include "common/types.hpp"
#include <vector>
#include <cstddef>

namespace tb::buffers {

// OHLCV свеча для хранения в буфере
struct OhlcvCandle {
    tb::Timestamp open_time{0};
    double open{0.0}, high{0.0}, low{0.0}, close{0.0};
    double volume{0.0};
    bool is_closed{false};
};

// Специализированный буфер свечей с методами извлечения для TA-Lib
template<std::size_t Capacity = 500>
class CandleBuffer {
public:
    void push(OhlcvCandle candle) { buffer_.push(std::move(candle)); }

    void push(const normalizer::NormalizedCandle& candle) {
        OhlcvCandle c;
        c.open_time = candle.envelope.exchange_ts;
        c.open      = candle.open.get();
        c.high      = candle.high.get();
        c.low       = candle.low.get();
        c.close     = candle.close.get();
        c.volume    = candle.volume.get();
        c.is_closed = candle.is_closed;
        buffer_.push(c);
    }

    // Обновляет последнюю незакрытую свечу (не добавляет новую)
    void update_last(const normalizer::NormalizedCandle& candle) {
        if (buffer_.empty()) { push(candle); return; }
        auto& last = buffer_.back();
        last.high   = candle.high.get();
        last.low    = candle.low.get();
        last.close  = candle.close.get();
        last.volume = candle.volume.get();
        last.is_closed = candle.is_closed;
    }

    // Извлечение массивов для TA-Lib (последние n свечей, от старых к новым)
    std::vector<double> close_prices(std::size_t n = Capacity) const {
        std::vector<double> out;
        buffer_.extract_field(out, &OhlcvCandle::close, n);
        return out;
    }
    std::vector<double> high_prices(std::size_t n = Capacity) const {
        std::vector<double> out;
        buffer_.extract_field(out, &OhlcvCandle::high, n);
        return out;
    }
    std::vector<double> low_prices(std::size_t n = Capacity) const {
        std::vector<double> out;
        buffer_.extract_field(out, &OhlcvCandle::low, n);
        return out;
    }
    std::vector<double> open_prices(std::size_t n = Capacity) const {
        std::vector<double> out;
        buffer_.extract_field(out, &OhlcvCandle::open, n);
        return out;
    }
    std::vector<double> volumes(std::size_t n = Capacity) const {
        std::vector<double> out;
        buffer_.extract_field(out, &OhlcvCandle::volume, n);
        return out;
    }

    std::size_t size() const noexcept { return buffer_.size(); }
    bool has_enough(std::size_t required) const noexcept { return buffer_.size() >= required; }

private:
    RingBuffer<OhlcvCandle, Capacity> buffer_;
};

} // namespace tb::buffers
