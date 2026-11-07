// fake-oxi: an ALSA sequencer client that looks like an OXI One (by name) and
// plays a short phrase, for testing ion's MIDI input and hot-plug without the
// hardware.
//
//   fake-oxi [--channel N] [--seconds S] [--clock BPM]

#include <alsa/asoundlib.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

int main(int argc, char** argv) {
  int channel = 1;
  double seconds = 4;
  double bpm = 0;
  for (int i = 1; i + 1 < argc; i += 2) {
    const std::string a = argv[i];
    if (a == "--channel") channel = std::atoi(argv[i + 1]);
    else if (a == "--seconds") seconds = std::atof(argv[i + 1]);
    else if (a == "--clock") bpm = std::atof(argv[i + 1]);
  }

  snd_seq_t* seq = nullptr;
  if (snd_seq_open(&seq, "default", SND_SEQ_OPEN_OUTPUT, 0) < 0) {
    std::fprintf(stderr, "fake-oxi: can't open the ALSA sequencer\n");
    return 1;
  }
  snd_seq_set_client_name(seq, "OXI One MK2 (fake)");
  const int port = snd_seq_create_simple_port(seq, "OXI One MK2 MIDI 1",
                                              SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
                                              SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_HARDWARE);
  std::printf("fake-oxi: client %d port %d, channel %d\n", snd_seq_client_id(seq), port, channel);
  std::fflush(stdout);
  // Give subscribers (ion) a moment to see the new port and connect.
  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  auto send = [&](snd_seq_event_t& ev) {
    snd_seq_ev_set_source(&ev, port);
    snd_seq_ev_set_subs(&ev);
    snd_seq_ev_set_direct(&ev);
    snd_seq_event_output_direct(seq, &ev);
  };

  const unsigned char ch = static_cast<unsigned char>((channel - 1) & 0x0F);
  const int phrase[] = {60, 64, 67, 72, 67, 64};
  const auto start = std::chrono::steady_clock::now();
  const auto end = start + std::chrono::duration<double>(seconds);
  auto nextNote = start;
  auto nextClock = start;
  int step = 0;
  int sent = 0;
  if (bpm > 0) {
    snd_seq_event_t ev;
    snd_seq_ev_clear(&ev);
    ev.type = SND_SEQ_EVENT_START;
    send(ev);
  }
  while (std::chrono::steady_clock::now() < end) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= nextNote) {
      snd_seq_event_t ev;
      snd_seq_ev_clear(&ev);
      if (step > 0) {
        snd_seq_ev_set_noteoff(&ev, ch, phrase[(step - 1) % 6], 0);
        send(ev);
      }
      snd_seq_ev_clear(&ev);
      snd_seq_ev_set_noteon(&ev, ch, phrase[step % 6], 100);
      send(ev);
      ++step;
      ++sent;
      nextNote += std::chrono::milliseconds(250);
    }
    if (bpm > 0 && now >= nextClock) {
      snd_seq_event_t ev;
      snd_seq_ev_clear(&ev);
      ev.type = SND_SEQ_EVENT_CLOCK;
      send(ev);
      nextClock += std::chrono::duration_cast<std::chrono::steady_clock::duration>(
          std::chrono::duration<double>(60.0 / bpm / 24.0));
    }
    std::this_thread::sleep_for(std::chrono::microseconds(500));
  }
  snd_seq_event_t ev;
  snd_seq_ev_clear(&ev);
  snd_seq_ev_set_noteoff(&ev, ch, phrase[(step - 1) % 6], 0);
  send(ev);
  std::printf("fake-oxi: sent %d notes\n", sent);
  snd_seq_close(seq);
  return 0;
}
