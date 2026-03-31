#pragma once
#include <array>
#include <vector>
#include <cstddef>
#include <stdexcept>

namespace tb::buffers {

// Кольцевой буфер фиксированного размера (zero-allocation после инициализации)
template<typename T, std::size_t Capacity>
class RingBuffer {
    static_assert(Capacity > 0, "Ёмкость должна быть больше нуля");
public:
    // Добавляет элемент (перезаписывает самый старый при переполнении)
    void push(T value) noexcept {
        data_[head_] = std::move(value);
        head_ = (head_ + 1) % Capacity;
        if (count_ < Capacity) ++count_;
    }

    // Доступ по индексу: 0 = самый старый, size()-1 = самый новый
    const T& operator[](std::size_t idx) const {
        std::size_t actual = (start_index() + idx) % Capacity;
        return data_[actual];
    }
    T& operator[](std::size_t idx) {
        std::size_t actual = (start_index() + idx) % Capacity;
        return data_[actual];
    }

    // Самый новый элемент
    T& back() {
        if (count_ == 0) throw std::out_of_range("RingBuffer::back() called on empty buffer");
        return (*this)[count_ - 1];
    }
    const T& back() const {
        if (count_ == 0) throw std::out_of_range("RingBuffer::back() called on empty buffer");
        return (*this)[count_ - 1];
    }
    const T& front() const {
        if (count_ == 0) throw std::out_of_range("RingBuffer::front() called on empty buffer");
        return (*this)[0];
    }

    std::size_t size() const noexcept { return count_; }
    bool empty() const noexcept { return count_ == 0; }
    bool full() const noexcept { return count_ == Capacity; }
    static constexpr std::size_t capacity() noexcept { return Capacity; }

    void clear() noexcept { head_ = 0; count_ = 0; }

    // Копирует последние n элементов в вектор (новейшие последними)
    std::vector<T> to_vector(std::size_t n = Capacity) const {
        n = std::min(n, count_);
        std::vector<T> result;
        result.reserve(n);
        std::size_t start = count_ > n ? count_ - n : 0;
        for (std::size_t i = start; i < count_; ++i) {
            result.push_back((*this)[i]);
        }
        return result;
    }

    // Копирует поле field последних n элементов в double-вектор
    template<typename Field>
    void extract_field(std::vector<double>& output, Field T::* field, std::size_t n = Capacity) const {
        n = std::min(n, count_);
        output.resize(n);
        std::size_t start = count_ > n ? count_ - n : 0;
        for (std::size_t i = 0; i < n; ++i) {
            output[i] = static_cast<double>((*this)[start + i].*field);
        }
    }

private:
    std::size_t start_index() const noexcept {
        if (count_ < Capacity) return 0;
        return head_;  // полный буфер: head_ указывает на самый старый
    }

    std::array<T, Capacity> data_{};
    std::size_t head_{0};   // следующая позиция записи
    std::size_t count_{0};
};

} // namespace tb::buffers
