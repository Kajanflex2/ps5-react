// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
#pragma once

#include "video_frame.hpp"

#include <cstdint>
#include <utility>
#include <vector>

// Draws decoded video frames for the presenter: YUV to RGB in a shader, HDR tone-mapped to SDR. Frames
// go up either as plane textures, or (GL 4.3 and later, the PS5's default) as one storage buffer the
// shader reads bytes from, which avoids the PS5's slow 2D texture uploads. The caller owns the context.
class GlVideo {
public:
  enum class Upload : std::uint8_t { textures, storage };
  enum class Fit : std::uint8_t { contain, cover, stretch };

  bool init(bool storage_allowed);
  void release();
  Upload upload_mode() const { return mode_; }

  struct Target {
    int x0, y0, x1, y1;          // the element's box, in framebuffer pixels
    int clip_x0, clip_y0, clip_x1, clip_y1;
    float alpha;
  };
  // Draws `frame` of video `id` into the target, in the viewport (x, y, w, h) that shows the
  // framebuffer of `fb_w` x `fb_h` pixels. Uploads a frame once however often it is drawn.
  void draw(std::uint64_t id, const video::Frame& frame, Fit fit, const Target& target, int x, int y, int w, int h,
            int fb_w, int fb_h);
  // Frees what no video drew since the last call.
  void end_present();

  // Bytes uploaded and microseconds spent uploading since the last call, for the frame log.
  std::uint64_t take_upload_bytes() { return std::exchange(upload_bytes_, 0); }
  std::uint32_t take_upload_us() { return std::exchange(upload_us_, 0); }

private:
  struct Entry {
    std::uint64_t id = 0, sequence = 0;
    bool drawn = false;
    video::Format format = video::Format::i420;
    int width = 0, height = 0;
    unsigned int textures[3] = {};
    unsigned int buffer = 0;
    std::size_t buffer_size = 0;
    int offsets[3] = {}, pitches[3] = {};
  };
  Entry& entry(std::uint64_t id);
  void upload_textures(Entry& entry, const video::Frame& frame);
  void upload_storage(Entry& entry, const video::Frame& frame);
  void free_entry(Entry& entry);

  Upload mode_ = Upload::textures;
  unsigned int texture_program_ = 0, storage_program_ = 0, vao_ = 0;
  std::vector<Entry> entries_;
  std::uint64_t upload_bytes_ = 0;
  std::uint32_t upload_us_ = 0;
};
