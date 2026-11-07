#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <optional>
#include <type_traits>

namespace ion {

// Bounded single-producer / single-consumer queue. Lock-free and allocation-free,
// so either end can be the audio thread. Capacity must be a power of two;
// one slot is kept empty, so it holds Capacity - 1 items.
template <typename T, std::size_t Capacity>
class SpscQueue {
  static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");
  static_assert(std::is_trivially_copyable_v<T>, "items are copied by value across threads");

 public:
  // Producer side. Returns false (and drops the item) when full.
  bool push(const T& item) noexcept {
    const std::size_t head = head_.load(std::memory_order_relaxed);
    const std::size_t next = (head + 1) & kMask;
    if (next == tail_.load(std::memory_order_acquire)) return false;
    slots_[head] = item;
    head_.store(next, std::memory_order_release);
    return true;
  }

  // Consumer side.
  std::optional<T> pop() noexcept {
    const std::size_t tail = tail_.load(std::memory_order_relaxed);
    if (tail == head_.load(std::memory_order_acquire)) return std::nullopt;
    T item = slots_[tail];
    tail_.store((tail + 1) & kMask, std::memory_order_release);
    return item;
  }

  bool empty() const noexcept {
    return tail_.load(std::memory_order_acquire) == head_.load(std::memory_order_acquire);
  }

 private:
  static constexpr std::size_t kMask = Capacity - 1;
  alignas(64) std::atomic<std::size_t> head_{0};
  alignas(64) std::atomic<std::size_t> tail_{0};
  std::array<T, Capacity> slots_{};
};

}  // namespace ion
