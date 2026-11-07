#include "midi/alsa_seq_in.h"

#include <alsa/asoundlib.h>
#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>

#include "engine/module.h"

namespace ion {
namespace {

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

// Calls fn(client id, client name, port id, port name) for every port other
// clients can read MIDI from.
template <typename Fn>
void forEachReadablePort(snd_seq_t* seq, Fn fn) {
  snd_seq_client_info_t* cinfo;
  snd_seq_port_info_t* pinfo;
  snd_seq_client_info_alloca(&cinfo);
  snd_seq_port_info_alloca(&pinfo);
  snd_seq_client_info_set_client(cinfo, -1);
  while (snd_seq_query_next_client(seq, cinfo) >= 0) {
    const int client = snd_seq_client_info_get_client(cinfo);
    snd_seq_port_info_set_client(pinfo, client);
    snd_seq_port_info_set_port(pinfo, -1);
    while (snd_seq_query_next_port(seq, pinfo) >= 0) {
      const unsigned caps = snd_seq_port_info_get_capability(pinfo);
      const unsigned type = snd_seq_port_info_get_type(pinfo);
      if ((caps & (SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ)) !=
          (SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ))
        continue;
      if (caps & SND_SEQ_PORT_CAP_NO_EXPORT) continue;
      if (!(type & SND_SEQ_PORT_TYPE_MIDI_GENERIC)) continue;
      fn(client, std::string(snd_seq_client_info_get_name(cinfo)), snd_seq_port_info_get_port(pinfo),
         std::string(snd_seq_port_info_get_name(pinfo)), type);
    }
  }
}

}  // namespace

AlsaSeqInput::AlsaSeqInput(Module& module, Options options) : module_(module), options_(std::move(options)) {}

AlsaSeqInput::~AlsaSeqInput() { stop(); }

bool AlsaSeqInput::start(std::string& error) {
  if (snd_seq_open(&seq_, "default", SND_SEQ_OPEN_INPUT, SND_SEQ_NONBLOCK) < 0) {
    error = "could not open the ALSA sequencer";
    seq_ = nullptr;
    return false;
  }
  snd_seq_set_client_name(seq_, "ion");
  myPort_ = snd_seq_create_simple_port(seq_, "ion in", SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
                                       SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
  if (myPort_ < 0) {
    error = "could not create an ALSA sequencer port";
    return false;
  }
  // Announcements tell us when devices are plugged in or removed.
  snd_seq_connect_from(seq_, myPort_, SND_SEQ_CLIENT_SYSTEM, SND_SEQ_PORT_SYSTEM_ANNOUNCE);

  if (pipe(wakeFd_) != 0) {
    error = "could not create a pipe";
    return false;
  }

  connectMatching();
  running_ = true;
  thread_ = std::thread([this] { run(); });
  return true;
}

void AlsaSeqInput::stop() {
  if (running_.exchange(false)) {
    const char c = 0;
    [[maybe_unused]] auto n = write(wakeFd_[1], &c, 1);
    thread_.join();
  }
  for (int& fd : wakeFd_) {
    if (fd >= 0) close(fd);
    fd = -1;
  }
  if (seq_) snd_seq_close(seq_);
  seq_ = nullptr;
}

bool AlsaSeqInput::matches(const std::string& client, const std::string& port) const {
  if (client == "ion" || client == "Midi Through") return false;
  if (options_.match.empty()) return true;
  const std::string m = lower(options_.match);
  return lower(client).find(m) != std::string::npos || lower(port).find(m) != std::string::npos;
}

void AlsaSeqInput::connectMatching() {
  const int self = snd_seq_client_id(seq_);
  std::vector<Port> found;
  forEachReadablePort(seq_, [&](int client, const std::string& cname, int port, const std::string& pname,
                                unsigned type) {
    if (client == self || client == SND_SEQ_CLIENT_SYSTEM) return;
    // With no match string, take hardware ports only (not other apps' outputs).
    if (options_.match.empty() && !(type & (SND_SEQ_PORT_TYPE_HARDWARE | SND_SEQ_PORT_TYPE_PORT))) return;
    if (!matches(cname, pname)) return;
    found.push_back({client, port, cname + ": " + pname});
  });

  std::lock_guard lock(mutex_);
  for (const Port& p : found) {
    const bool already = std::any_of(connected_.begin(), connected_.end(),
                                     [&](const Port& c) { return c.client == p.client && c.port == p.port; });
    if (already) continue;
    if (snd_seq_connect_from(seq_, myPort_, p.client, p.port) >= 0) connected_.push_back(p);
  }
}

std::vector<std::string> AlsaSeqInput::connected() const {
  std::lock_guard lock(mutex_);
  std::vector<std::string> names;
  for (const Port& p : connected_) names.push_back(p.name);
  return names;
}

void AlsaSeqInput::run() {
  const int n = snd_seq_poll_descriptors_count(seq_, POLLIN);
  std::vector<pollfd> fds(static_cast<std::size_t>(n) + 1);
  snd_seq_poll_descriptors(seq_, fds.data(), static_cast<unsigned>(n), POLLIN);
  fds[static_cast<std::size_t>(n)] = {wakeFd_[0], POLLIN, 0};

  while (running_) {
    if (poll(fds.data(), fds.size(), -1) < 0) continue;
    if (!running_) break;

    snd_seq_event_t* ev = nullptr;
    while (snd_seq_event_input(seq_, &ev) >= 0 && ev) {
      MidiMessage m;
      const uint8_t ch = ev->data.note.channel & 0x0F;
      switch (ev->type) {
        case SND_SEQ_EVENT_NOTEON:
          m = {static_cast<uint8_t>(0x90 | ch), ev->data.note.note, ev->data.note.velocity};
          break;
        case SND_SEQ_EVENT_NOTEOFF:
          m = {static_cast<uint8_t>(0x80 | ch), ev->data.note.note, ev->data.note.velocity};
          break;
        case SND_SEQ_EVENT_CONTROLLER:
          m = {static_cast<uint8_t>(0xB0 | (ev->data.control.channel & 0x0F)),
               static_cast<uint8_t>(ev->data.control.param & 0x7F), static_cast<uint8_t>(ev->data.control.value & 0x7F)};
          break;
        case SND_SEQ_EVENT_PGMCHANGE:
          m = {static_cast<uint8_t>(0xC0 | (ev->data.control.channel & 0x0F)),
               static_cast<uint8_t>(ev->data.control.value & 0x7F), 0};
          break;
        case SND_SEQ_EVENT_PITCHBEND: {
          const int v = ev->data.control.value + 8192;
          m = {static_cast<uint8_t>(0xE0 | (ev->data.control.channel & 0x0F)), static_cast<uint8_t>(v & 0x7F),
               static_cast<uint8_t>((v >> 7) & 0x7F)};
          break;
        }
        case SND_SEQ_EVENT_CLOCK: m = {0xF8, 0, 0}; break;
        case SND_SEQ_EVENT_START: m = {0xFA, 0, 0}; break;
        case SND_SEQ_EVENT_CONTINUE: m = {0xFB, 0, 0}; break;
        case SND_SEQ_EVENT_STOP: m = {0xFC, 0, 0}; break;

        case SND_SEQ_EVENT_PORT_START:
        case SND_SEQ_EVENT_CLIENT_START:
          connectMatching();
          continue;
        case SND_SEQ_EVENT_PORT_EXIT:
        case SND_SEQ_EVENT_CLIENT_EXIT: {
          std::lock_guard lock(mutex_);
          const int gone = ev->data.addr.client;
          const int port = ev->data.addr.port;
          const bool wholeClient = ev->type == SND_SEQ_EVENT_CLIENT_EXIT;
          std::erase_if(connected_, [&](const Port& p) {
            return p.client == gone && (wholeClient || p.port == port);
          });
          continue;
        }
        default:
          continue;
      }
      module_.pushMidi(m);
    }
  }
}

std::vector<AlsaSeqInput::Port> AlsaSeqInput::listInputs(std::string& error) {
  std::vector<Port> ports;
  snd_seq_t* seq = nullptr;
  if (snd_seq_open(&seq, "default", SND_SEQ_OPEN_INPUT, 0) < 0) {
    error = "could not open the ALSA sequencer";
    return ports;
  }
  forEachReadablePort(seq, [&](int client, const std::string& cname, int port, const std::string& pname, unsigned) {
    if (client == SND_SEQ_CLIENT_SYSTEM) return;
    ports.push_back({client, port, cname + ": " + pname});
  });
  snd_seq_close(seq);
  return ports;
}

}  // namespace ion
