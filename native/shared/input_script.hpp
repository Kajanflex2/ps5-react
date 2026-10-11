// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Scripted controller actions for unattended profiling, e.g. "wait:3000,right*3,confirm,quit".
// Steps are separated by commas or whitespace. An action takes one frame; `action*N` repeats it at
// the held-key repeat interval; `wait:MS` pauses for wall-clock milliseconds; `quit` asks the host
// to close; `shot:NAME` asks it to save the frame on screen as NAME.bmp; `swipe:DIRECTION:TRAVEL`
// slides a touchpad finger TRAVEL (0..0.5 of the pad) from its middle, one `touch:X,Y` sample per
// frame, then `touch:up`, which the host passes on as touchpad input. The desktop reads
// PS5_REACT_INPUT_SCRIPT, the PS5 /app0/dev/input-script.txt.
class InputScript {
public:
  // Replaces the script. On a bad step returns false and describes it in `error`.
  bool parse(const char* text, char* error, std::size_t size);
  // The action due at `now_ms` (a monotonic clock), at most one per call, or nullptr. Valid until the
  // next parse().
  const char* next(std::uint64_t now_ms);
  // Whether every step has run.
  bool done() const { return index_ >= steps_.size(); }

private:
  struct Step { std::string action; std::uint32_t wait_ms; }; // empty action: a wait
  std::vector<Step> steps_;
  std::size_t index_ = 0;
  std::uint64_t resume_ = 0;
};
