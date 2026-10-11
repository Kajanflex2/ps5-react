// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
// tools/test_video.py: plays a two-second clip through video::Player in real time, with a stand-in mixer
// that reads the routed audio ring at 48 kHz, and checks what the presenter would show.
#include "video_audio.hpp"
#include "video_player.hpp"
#include "audio/stream_ring.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace {
std::atomic<hui::audio::StreamRing*> routed{nullptr};
std::atomic<bool> mixing{true};
std::atomic<std::uint64_t> mixed{0};

// The mixer: a grain of 256 frames every 5.33 ms from whatever ring is routed.
void mixer() {
  float grain[256 * 2];
  auto next = std::chrono::steady_clock::now();
  while (mixing) {
    next += std::chrono::microseconds(5333);
    std::this_thread::sleep_until(next);
    if (hui::audio::StreamRing* ring = routed.load()) mixed += ring->read(grain, 256);
  }
}

std::int64_t now_us() {
  return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

int failures = 0;
void check(bool ok, const std::string& what) {
  std::printf("%s: %s\n", ok ? "ok" : "FAIL", what.c_str());
  if (!ok) ++failures;
}

struct Run {
  int changes = 0;          // distinct pictures shown
  bool ordered = true;      // their times only went forward
  std::int64_t first_us = -1, last_us = -1;
};

// Presents for `ms` at 60 Hz, as a host does, recording the pictures shown.
Run present(video::Player& player, int ms, bool stop_at_end = true) {
  Run run;
  // The picture already on screen is not a change.
  std::uint64_t sequence = player.frame() ? player.frame()->sequence : 0;
  const std::int64_t end = now_us() + ms * 1000;
  auto next = std::chrono::steady_clock::now();
  while (now_us() < end) {
    next += std::chrono::microseconds(16667);
    std::this_thread::sleep_until(next);
    player.update(now_us(), 16667);
    if (const video::Frame* frame = player.frame(); frame && frame->sequence != sequence) {
      sequence = frame->sequence;
      if (run.last_us >= 0 && frame->pts_us <= run.last_us) run.ordered = false;
      if (run.first_us < 0) run.first_us = frame->pts_us;
      run.last_us = frame->pts_us;
      ++run.changes;
    }
    if (stop_at_end && player.status().state == video::State::ended) break;
  }
  return run;
}
// Plays to the end, noting each change of the subtitle text with the position it showed at.
std::vector<std::pair<double, std::string>> watch_subtitles(video::Player& player) {
  std::vector<std::pair<double, std::string>> seen;
  std::string text;
  auto next = std::chrono::steady_clock::now();
  const std::int64_t end = now_us() + 4'000'000;
  while (now_us() < end && player.status().state != video::State::ended) {
    next += std::chrono::microseconds(16667);
    std::this_thread::sleep_until(next);
    player.update(now_us(), 16667);
    const video::Status status = player.status();
    if (status.subtitle != text) seen.emplace_back(status.position, text = status.subtitle);
  }
  return seen;
}

std::string describe(const std::vector<std::pair<double, std::string>>& seen) {
  std::string out;
  for (const auto& [at, text] : seen) {
    std::string shown = text;
    for (char& c : shown) if (c == '\n') c = '|';
    out += (out.empty() ? "" : ", ") + std::to_string(at).substr(0, 4) + "s \"" + shown + "\"";
  }
  return out;
}

bool shows_both_cues(const std::vector<std::pair<double, std::string>>& seen) {
  // "Hello" from 0.3 s to 0.9 s, then "Two" and "lines" (its italics dropped) from 1.2 s to 1.8 s.
  return seen.size() == 4 && seen[0].second == "Hello" && seen[0].first >= 0.25 && seen[0].first < 0.4 &&
         seen[1].second.empty() && seen[1].first >= 0.85 && seen[1].first < 1.0 &&
         seen[2].second == "Two\nlines" && seen[2].first >= 1.15 && seen[2].first < 1.3 &&
         seen[3].second.empty() && seen[3].first >= 1.75 && seen[3].first < 1.9;
}
} // namespace

namespace video_audio {
bool start() { return true; }
void route(hui::audio::StreamRing* ring) { routed = ring; }
void set_gain(float) {}
} // namespace video_audio

int main(int argc, char** argv) {
  if (argc < 5) {
    std::fprintf(stderr, "usage: %s clip subtitled-clip subtitles.srt short-audio-clip\n", argv[0]);
    return 2;
  }
  std::thread audio(mixer);
  std::vector<std::shared_ptr<void>> kept; // rings the mixer may still read
  {
    video::Player player(argv[1], {});
    const Run first = present(player, 3500);
    video::Status status = player.status();
    check(status.width == 320 && status.height == 180 && status.video_codec == "h264" && status.audio_codec == "aac",
          "opens the clip: " + std::to_string(status.width) + "x" + std::to_string(status.height) + " " +
          status.video_codec + "/" + status.audio_codec);
    check(status.audio_clock || mixed > 0, "audio is routed to the mixer and read");
    check(first.ordered, "pictures are shown in time order");
    check(first.first_us == 0, "the first picture is the first frame (" + std::to_string(first.first_us) + " us)");
    check(first.changes >= 46 && first.changes <= 50, "about every frame of 50 is shown: " + std::to_string(first.changes));
    check(status.state == video::State::ended, "the clip ends");
    check(std::fabs(status.late_ms) < 20, "pictures keep time with the audio: " + std::to_string(status.late_ms) + " ms late");

    player.seek(1.0);
    const Run sought = present(player, 3000);
    check(sought.first_us >= 1000000 && sought.first_us < 1040000,
          "a seek to 1 s shows the frame at 1 s first (" + std::to_string(sought.first_us) + " us)");
    check(sought.ordered && sought.changes >= 22, "and plays on from there: " + std::to_string(sought.changes) + " pictures");
    check(player.status().state == video::State::ended, "to the end again");

    player.seek(0);
    present(player, 600, false);
    player.set_paused(true, now_us());
    const double held = player.status().position;
    const Run paused = present(player, 500, false);
    check(paused.changes == 0 && std::fabs(player.status().position - held) < 0.02,
          "a pause holds the picture and the position at " + std::to_string(held) + " s (" +
          std::to_string(paused.changes) + " pictures, now " + std::to_string(player.status().position) + " s)");
    player.set_paused(false, now_us());
    const Run resumed = present(player, 400, false);
    check(resumed.changes >= 8, "and playback resumes: " + std::to_string(resumed.changes) + " pictures in 0.4 s");
    kept = player.detach_audio();
  }
  {
    video::Player player(argv[2], {});
    present(player, 300, false);
    const std::vector<video::Track> tracks = player.status().subtitle_tracks;
    check(tracks.size() == 1 && tracks[0].index == 2 && tracks[0].language == "cze" && tracks[0].codec == "subrip",
          "lists the file's subtitle track");
    player.select_subtitle_track(2);
    player.seek(0);
    const auto seen = watch_subtitles(player);
    check(shows_both_cues(seen), "shows the file's cues at their times: " + describe(seen));
    for (auto& ring : player.detach_audio()) kept.push_back(ring);
  }
  {
    video::Player player(argv[1], {});
    const int track = player.add_subtitle_file(argv[3], "eng", "Added");
    player.select_subtitle_track(track);
    const auto seen = watch_subtitles(player);
    check(track == video::kExternalSubtitle && shows_both_cues(seen),
          "shows an added subtitle file's cues at their times: " + describe(seen));
    for (auto& ring : player.detach_audio()) kept.push_back(ring);
  }
  {
    video::Player player(argv[1], {});
    player.set_audio_delay(0.2);
    present(player, 1200, false);
    const video::Frame* frame = player.frame();
    const double ahead = frame ? frame->pts_us / 1e6 - player.status().position : 0;
    check(ahead > 0.17 && ahead < 0.23, "an audio delay of 0.2 s shows the picture 0.2 s ahead of the sound: " +
          std::to_string(ahead) + " s");
    for (auto& ring : player.detach_audio()) kept.push_back(ring);
  }
  {
    // Its audio ends a second before its picture: the clock carries on without it.
    video::Player player(argv[4], {});
    const Run run = present(player, 3500);
    check(player.status().state == video::State::ended && run.changes >= 46 && run.last_us >= 1960000,
          "a video whose audio ends first plays its last pictures and ends: " + std::to_string(run.changes) +
          " pictures, the last at " + std::to_string(run.last_us) + " us");
    for (auto& ring : player.detach_audio()) kept.push_back(ring);
  }
  {
    video::Player missing("/nonexistent/clip.mp4", {});
    present(missing, 300, false);
    const video::Status status = missing.status();
    check(status.state == video::State::failed && status.error.find("video.open") == 0,
          "a missing file fails with an error naming the call: " + status.error);
  }
  mixing = false;
  audio.join();
  std::printf("%s\n", failures ? "FAILED" : "PASS");
  return failures ? 1 : 0;
}
