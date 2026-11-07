#include "engine/clock.h"

#include <algorithm>
#include <cmath>

namespace ion {
namespace {

constexpr double kTicksPerBeat = 24.0;
constexpr double kLostSeconds = 0.5;       // no tick for this long: clock gone

}  // namespace

void ClockFollower::tick(int64_t frame) {
  if (lastTick_ >= 0 && frame <= lastTick_) return;  // out of order: ignore
  window_[static_cast<std::size_t>(head_)] = frame;
  head_ = (head_ + 1) % kWindow;
  count_ = std::min(count_ + 1, kWindow);
  if (count_ >= 3) {
    // Least-squares slope of frame against tick index, oldest first.
    const int n = count_;
    const int oldest = (head_ - n + kWindow) % kWindow;
    const auto base = window_[static_cast<std::size_t>(oldest)];
    const double meanX = (n - 1) / 2.0;
    double meanY = 0;
    for (int i = 0; i < n; ++i) meanY += static_cast<double>(window_[static_cast<std::size_t>((oldest + i) % kWindow)] - base);
    meanY /= n;
    double sxy = 0, sxx = 0;
    for (int i = 0; i < n; ++i) {
      const double dx = i - meanX;
      sxy += dx * (static_cast<double>(window_[static_cast<std::size_t>((oldest + i) % kWindow)] - base) - meanY);
      sxx += dx * dx;
    }
    const double framesPerTick = sxy / sxx;
    if (framesPerTick > 0) bpm_ = 60.0 * sr_ / (framesPerTick * kTicksPerBeat);
  } else if (count_ == 2) {
    bpm_ = 60.0 * sr_ / (static_cast<double>(frame - lastTick_) * kTicksPerBeat);
  }
  lastTick_ = frame;
  present_ = true;
  if (running_) ++ticks_;
}

void ClockFollower::start(int64_t) {
  ticks_ = 0;
  running_ = true;
}

void ClockFollower::cont(int64_t) { running_ = true; }

void ClockFollower::stop(int64_t) { running_ = false; }

void ClockFollower::songPosition(int beats) { ticks_ = static_cast<int64_t>(beats) * 6; }

void ClockFollower::update(int64_t frame) {
  if (present_ && lastTick_ >= 0 && static_cast<double>(frame - lastTick_) > kLostSeconds * sr_) {
    present_ = false;
    lastTick_ = -1;
    count_ = 0;
    head_ = 0;
    bpm_ = 0;
  }
}

}  // namespace ion
