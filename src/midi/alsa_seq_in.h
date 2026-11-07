#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/spsc_queue.h"
#include "engine/midi_types.h"

typedef struct _snd_seq snd_seq_t;

namespace ion {

class Module;

// MIDI input through the ALSA sequencer (DECISIONS D-01). Creates a client
// "ion" with one input port, connects to every source port whose client or
// port name contains the match string (default "OXI"), and reconnects when
// devices come and go. Other apps can still use the same device.
class AlsaSeqInput {
 public:
  struct Options {
    std::string match = "OXI";  // case-insensitive; empty = every hardware input
  };

  struct Port {
    int client = 0;
    int port = 0;
    std::string name;  // "client name: port name"
  };

  AlsaSeqInput(Module& module, Options options);
  ~AlsaSeqInput();
  AlsaSeqInput(const AlsaSeqInput&) = delete;
  AlsaSeqInput& operator=(const AlsaSeqInput&) = delete;

  bool start(std::string& error);
  void stop();

  // Names of the ports we're currently connected to.
  std::vector<std::string> connected() const;
  // Name of a connected client, for the MIDI monitor ("" if unknown).
  std::string clientName(int client) const;

  // Changes which inputs are connected, live (Settings). Disconnects ports that
  // no longer match.
  void setMatch(const std::string& match);

  // Every message as it arrives (clock included), for the MIDI monitor. The UI
  // thread is the only consumer.
  struct MonitorEntry {
    MidiMessage msg;
    int64_t timeNs;
    int client;
  };
  SpscQueue<MonitorEntry, 512>& monitor() { return monitor_; }

  // Every readable MIDI port on the system, for --list-midi.
  static std::vector<Port> listInputs(std::string& error);

 private:
  void run();
  void connectMatching();
  bool matches(const std::string& client, const std::string& port) const;

  Module& module_;
  Options options_;
  snd_seq_t* seq_ = nullptr;
  int myPort_ = -1;
  int wakeFd_[2] = {-1, -1};
  std::thread thread_;
  std::atomic<bool> running_{false};

  mutable std::mutex mutex_;  // guards connected_ and options_.match
  std::vector<Port> connected_;
  SpscQueue<MonitorEntry, 512> monitor_;
};

}  // namespace ion
