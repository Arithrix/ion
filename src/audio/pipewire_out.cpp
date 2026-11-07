#include "audio/pipewire_out.h"

#include <pthread.h>
#include <sched.h>

#include <chrono>
#include <thread>

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>

#include "engine/engine.h"
#include "engine/module.h"

namespace ion {
namespace {

constexpr uint32_t kChannels = 2;
constexpr uint32_t kStride = sizeof(float) * kChannels;

// Shared by all streams; must outlive them.
const pw_stream_events& streamEvents() {
  static const pw_stream_events events = [] {
    pw_stream_events e{};
    e.version = PW_VERSION_STREAM_EVENTS;
    e.state_changed = [](void* self, pw_stream_state old, pw_stream_state state, const char* error) {
      PipeWireOutput::onStateChanged(self, old, state, error);
    };
    e.process = &PipeWireOutput::onProcess;
    return e;
  }();
  return events;
}

}  // namespace

PipeWireOutput::PipeWireOutput(Module& module, Options options) : module_(module), options_(options) {
  pw_init(nullptr, nullptr);
}

PipeWireOutput::~PipeWireOutput() {
  stop();
  pw_deinit();
}

void PipeWireOutput::onStateChanged(void* self, int /*oldState*/, int state, const char* error) {
  auto* out = static_cast<PipeWireOutput*>(self);
  if (error) out->lastError_ = error;
  out->state_.store(state, std::memory_order_release);
  pw_thread_loop_signal(out->loop_, false);
}

void PipeWireOutput::onProcess(void* self) {
  auto* out = static_cast<PipeWireOutput*>(self);

  if (!out->checkedRealtime_) {
    int policy = 0;
    sched_param param{};
    pthread_getschedparam(pthread_self(), &policy, &param);
    out->realtime_.store(policy == SCHED_FIFO || policy == SCHED_RR, std::memory_order_relaxed);
    out->checkedRealtime_ = true;
  }

  pw_buffer* b = pw_stream_dequeue_buffer(out->stream_);
  if (!b) return;
  spa_data& d = b->buffer->datas[0];
  auto* dst = static_cast<float*>(d.data);
  if (!dst) return;

  uint32_t frames = d.maxsize / kStride;
  if (b->requested) frames = std::min(frames, static_cast<uint32_t>(b->requested));

  // The graph clock advances by one cycle's worth of frames per callback. If it
  // moved further, cycles were skipped: count a dropout.
  pw_time t{};
  const bool haveTime = pw_stream_get_time_n(out->stream_, &t, sizeof t) == 0;
  if (haveTime && t.ticks != 0) {
    if (out->lastTicks_ != 0 && out->lastFrames_ != 0 && t.ticks > out->lastTicks_ &&
        t.ticks - out->lastTicks_ > out->lastFrames_ + out->lastFrames_ / 2)
      out->dropouts_.fetch_add(1, std::memory_order_relaxed);
    out->lastTicks_ = t.ticks;
  }
  out->lastFrames_ = frames;

  // t.now is the CLOCK_MONOTONIC start of this graph cycle: the reference for
  // placing timestamped MIDI.
  out->module_.process(dst, frames, haveTime ? t.now : 0);

  d.chunk->offset = 0;
  d.chunk->stride = static_cast<int32_t>(kStride);
  d.chunk->size = frames * kStride;
  pw_stream_queue_buffer(out->stream_, b);
}

bool PipeWireOutput::start(std::string& error) {
  loop_ = pw_thread_loop_new("ion-pipewire", nullptr);
  if (!loop_) {
    error = "could not create a PipeWire loop";
    return false;
  }

  pw_properties* props = pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Playback",
                                           PW_KEY_MEDIA_ROLE, "Music", PW_KEY_APP_NAME, "ion",
                                           PW_KEY_NODE_NAME, "ion", PW_KEY_NODE_DESCRIPTION, "ion sound module",
                                           nullptr);
  // A request, not a force: PipeWire runs the graph at the smallest latency any
  // client asks for that the device allows, and goes back when we quit.
  pw_properties_setf(props, PW_KEY_NODE_LATENCY, "%u/%d", options_.latencyFrames, kSampleRate);

  pw_thread_loop_lock(loop_);
  stream_ = pw_stream_new_simple(pw_thread_loop_get_loop(loop_), "ion", props, &streamEvents(), this);
  if (!stream_) {
    pw_thread_loop_unlock(loop_);
    error = "could not create a PipeWire stream (is PipeWire running?)";
    return false;
  }

  uint8_t podBuffer[1024];
  spa_pod_builder builder = SPA_POD_BUILDER_INIT(podBuffer, sizeof podBuffer);
  spa_audio_info_raw info{};
  info.format = SPA_AUDIO_FORMAT_F32;
  info.rate = kSampleRate;
  info.channels = kChannels;
  info.position[0] = SPA_AUDIO_CHANNEL_FL;
  info.position[1] = SPA_AUDIO_CHANNEL_FR;
  const spa_pod* params[1] = {spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &info)};

  const int rc = pw_stream_connect(
      stream_, PW_DIRECTION_OUTPUT, PW_ID_ANY,
      static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS |
                                   PW_STREAM_FLAG_RT_PROCESS),
      params, 1);
  pw_thread_loop_unlock(loop_);
  if (rc < 0) {
    error = "could not connect the PipeWire stream";
    return false;
  }

  if (pw_thread_loop_start(loop_) < 0) {
    error = "could not start the PipeWire thread";
    return false;
  }

  // Wait up to 3 s for the stream to start (or fail).
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (std::chrono::steady_clock::now() < deadline) {
    const int s = state_.load(std::memory_order_acquire);
    if (s == PW_STREAM_STATE_STREAMING) return true;
    if (s == PW_STREAM_STATE_ERROR) {
      error = "PipeWire stream error: " + lastError_;
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  // PAUSED is normal until the graph runs; treat a connected stream as started.
  if (state_.load() == PW_STREAM_STATE_PAUSED) return true;
  error = "PipeWire stream did not start within 3 s";
  return false;
}

void PipeWireOutput::stop() {
  if (loop_) pw_thread_loop_stop(loop_);
  if (stream_) {
    pw_stream_destroy(stream_);
    stream_ = nullptr;
  }
  if (loop_) {
    pw_thread_loop_destroy(loop_);
    loop_ = nullptr;
  }
}

}  // namespace ion
