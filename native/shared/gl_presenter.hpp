// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
#pragma once

#include "damage_tracker.hpp"
#include "er_scene.h"
#include <cstdint>
#include <span>
#include <string>
#include <vector>

// Presents the software framebuffer, over the engine's layers (er_get_layers) when it lists any.
// The caller owns the GL context.
// No components, fonts, layout, platform calls or dependencies on the UI kit.
class GlPresenter {
public:
  GlPresenter() = default;
  GlPresenter(const GlPresenter&) = delete;
  GlPresenter& operator=(const GlPresenter&) = delete;
  bool init(int width, int height);
  // Uploads the whole buffer. Afterwards the GPU copy no longer mirrors the
  // engine framebuffer, so the next damage draw uploads everything again.
  bool draw(const std::uint32_t* argb, int surface_width, int surface_height);
  // Uploads what changed (damage_tracker's contract): at one-to-one size the rows of
  // `damage` and of the moved rects; otherwise `moves` are replayed on the texture and
  // then `damage` uploaded. The first draw and any surface size change upload everything.
  bool draw(const std::uint32_t* argb, std::span<const ERRect> damage, std::span<const DamageMove> moves,
            int surface_width, int surface_height);
  // Whether set_layers() can draw: hosts enable engine layers only then (er_set_layers_enabled).
  bool layers_supported() const { return layer_program_ != 0 && max_texels_ > 0; }
  // The layers the next present draws beneath the framebuffer, bottom first; valid until that present.
  void set_layers(std::span<const ERLayer> layers) { layers_ = layers; }
  void release();
  // Where the last draw spent its CPU time: copying pixels to the GPU (framebuffer rows and new layer
  // images, which can block until the GPU is done with the previous frame's buffers), and issuing the
  // draws. Bytes count what was handed to GL.
  struct Timing { std::uint32_t upload_us = 0, draw_us = 0; std::uint64_t upload_bytes = 0; };
  const Timing& last_timing() const { return timing_; }

private:
  unsigned int program_ = 0, texture_ = 0, vao_ = 0;
  // A second texture and two framebuffers, created on the first move: GL cannot blit a texture onto itself.
  unsigned int scratch_ = 0, read_fbo_ = 0, draw_fbo_ = 0;
  int width_ = 0, height_ = 0, surface_width_ = 0, surface_height_ = 0;
  bool synced_ = false;
  // Buffer textures of whole framebuffer rows for one-to-one presentation, slice i holding rows
  // [i * slice_rows_, (i + 1) * slice_rows_). Empty when unsupported.
  unsigned int buffer_program_ = 0;
  std::vector<unsigned int> buffers_, buffer_textures_;
  int slice_rows_ = 0;
  bool buffer_synced_ = false;
  // Layer images by engine image name, which the engine never reuses for other pixels, in buffer
  // textures like the framebuffer: on the PS5 a 2D texture upload of one took 10-15 ms. Slice i holds
  // rows [i * rows, (i + 1) * rows] (one row of overlap, for the bilinear pair). Images no layer drew
  // are kept, least recently drawn freed first past kKeptLayerImages, so a page shown again does not
  // upload its backdrop again.
  struct LayerImage {
    std::string name;
    int width = 0, height = 0, rows = 0;
    std::vector<unsigned int> buffers, textures;
    std::uint64_t drawn = 0;
  };
  static constexpr int kKeptLayerImages = 4;
  unsigned int layer_program_ = 0;
  int max_texels_ = 0;
  std::vector<LayerImage> layer_images_;
  std::uint64_t presents_ = 0;
  std::span<const ERLayer> layers_;
  Timing timing_;

  bool one_to_one(int surface_width, int surface_height) const;
  void upload_changed_rows(const std::uint32_t* argb, std::span<const ERRect> damage,
                           std::span<const DamageMove> moves);
  void upload_rows(const std::uint32_t* argb, int y0, int y1);
  void release_buffer();
  void upload(const std::uint32_t* argb, const ERRect& rect);
  bool move(const DamageMove& move);
  bool present(int surface_width, int surface_height);
  const LayerImage* layer_image(const ERLayer& layer);
  void free_layer_images(std::size_t keep);
  void draw_layers(int x, int y, int w, int h);
};
