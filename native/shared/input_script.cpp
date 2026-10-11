// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
#include "input_script.hpp"

#include "actions.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {
constexpr std::uint32_t kRepeatMs = 110; // The desktop host's held-key repeat interval.
constexpr int kSwipeFrames = 16;         // A swipe's finger samples, one per frame.
} // namespace

bool InputScript::parse(const char* text, char* error, std::size_t size) {
  steps_.clear();
  index_ = 0;
  resume_ = 0;
  const std::string script(text);
  for (std::size_t start = 0; start < script.size();) {
    const std::size_t end = std::min(script.find_first_of(", \t\r\n", start), script.size());
    std::string token = script.substr(start, end - start);
    start = end + 1;
    if (token.empty()) continue;
    char* rest = nullptr;
    if (token.rfind("wait:", 0) == 0) {
      const long ms = std::strtol(token.c_str() + 5, &rest, 10);
      if (rest == token.c_str() + 5 || *rest || ms < 0) {
        std::snprintf(error, size, "bad wait '%s'", token.c_str());
        return false;
      }
      steps_.push_back({"", static_cast<std::uint32_t>(ms)});
      continue;
    }
    if (token.rfind("shot:", 0) == 0) {
      const std::string name = token.substr(5);
      if (name.empty() || name.size() > 32 || name.find_first_not_of(
            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != std::string::npos) {
        std::snprintf(error, size, "bad shot '%s' (shot:NAME, 1-32 letters, digits, _ or -)", token.c_str());
        return false;
      }
      steps_.push_back({token, 0});
      continue;
    }
    if (token.rfind("swipe:", 0) == 0) {
      // swipe:DIRECTION:TRAVEL: a finger sliding TRAVEL (0..1 of the pad) from its middle, then lifting.
      const std::size_t colon = token.find(':', 6);
      const std::string direction = token.substr(6, colon == std::string::npos ? std::string::npos : colon - 6);
      const double travel = colon == std::string::npos ? -1 : std::strtod(token.c_str() + colon + 1, &rest);
      const double dx = direction == "right" ? 1 : direction == "left" ? -1 : 0;
      const double dy = direction == "down" ? 1 : direction == "up" ? -1 : 0;
      if ((!dx && !dy) || travel <= 0 || travel > 0.5 || *rest) {
        std::snprintf(error, size, "bad swipe '%s' (swipe:left|right|up|down:TRAVEL, TRAVEL 0..0.5 of the pad)", token.c_str());
        return false;
      }
      for (int frame = 0; frame <= kSwipeFrames; ++frame) {
        const double at = travel * frame / kSwipeFrames;
        char sample[48];
        std::snprintf(sample, sizeof sample, "touch:%.4f,%.4f", 0.5 + dx * at, 0.5 + dy * at);
        steps_.push_back({sample, 0});
      }
      steps_.push_back({"touch:up", 0});
      continue;
    }
    long count = 1;
    if (const std::size_t star = token.find('*'); star != std::string::npos) {
      count = std::strtol(token.c_str() + star + 1, &rest, 10);
      if (rest == token.c_str() + star + 1 || *rest || count < 1) {
        std::snprintf(error, size, "bad repeat in '%s'", token.c_str());
        return false;
      }
      token.resize(star);
    }
    const char* action = token == "quit" ? "quit" : nullptr;
    for (const char* known : kActionNames)
      if (token == known) action = known;
    if (!action) {
      std::string known;
      for (const char* name : kActionNames) known += std::string(name) + ", ";
      std::snprintf(error, size, "unknown action '%s' (use %squit, wait:MS, shot:NAME, swipe:DIRECTION:TRAVEL)", token.c_str(), known.c_str());
      return false;
    }
    for (long i = 0; i < count; ++i) {
      if (i) steps_.push_back({"", kRepeatMs});
      steps_.push_back({action, 0});
    }
  }
  return true;
}

const char* InputScript::next(std::uint64_t now_ms) {
  while (index_ < steps_.size() && now_ms >= resume_) {
    const Step& step = steps_[index_++];
    if (step.action.empty()) {
      resume_ = now_ms + step.wait_ms;
      continue;
    }
    resume_ = now_ms + 1; // One action per frame.
    return step.action.c_str();
  }
  return nullptr;
}
