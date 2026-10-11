// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
#include "gl_presenter.hpp"
#include "video_api.hpp"

#ifdef __APPLE__
#include <OpenGL/gl3.h>
#elif defined(PROSPERO)
#include <GL/glcorearb.h>
#else
#define GL_GLEXT_PROTOTYPES
#include <SDL2/SDL_opengl.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdio>

namespace {
const char* vertex_source = R"(#version 410 core
out vec2 uv;
void main() {
  // Oversized triangle: one draw, no vertex buffer, no seam through the image.
  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
  uv = vec2(p.x, 1.0 - p.y);
}
)";
const char* fragment_source = R"(#version 410 core
in vec2 uv;
uniform sampler2D frame;
out vec4 color;
// The texture holds 0xAARRGGBB words uploaded as RGBA bytes; little-endian
// storage puts them in B,G,R,A order.
void main() { color = texture(frame, uv).bgra; }
)";
// One-to-one presentation reads framebuffer words straight from buffer textures, one horizontal slice
// per draw: the PS5 allows 1048576 texels per buffer texture, less than half a 4K frame.
const char* buffer_fragment_source = R"(#version 410 core
uniform samplerBuffer rows;
uniform ivec2 size;
uniform int first;
out vec4 color;
void main() {
  ivec2 p = ivec2(gl_FragCoord.xy);
  color = texelFetch(rows, (size.y - 1 - p.y - first) * size.x + p.x).bgra;
}
)";

// A layer's quad, in framebuffer pixels (dst: x0 y0 x1 y1), over the image pixels src (x0 y0 x1 y1).
const char* layer_vertex_source = R"(#version 410 core
uniform vec4 dst;
uniform vec4 src;
uniform vec2 size;
out vec2 at;
void main() {
  vec2 corner = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));
  vec2 p = mix(dst.xy, dst.zw, corner);
  gl_Position = vec4(p.x / size.x * 2.0 - 1.0, 1.0 - p.y / size.y * 2.0, 0.0, 1.0);
  at = mix(src.xy, src.zw, corner);
}
)";
// Bilinear between texel centres with clamped edges, as GL_LINEAR: one draw per slice of the image,
// each keeping the fragments whose upper tap row the slice owns.
const char* layer_fragment_source = R"(#version 410 core
in vec2 at;
uniform samplerBuffer pixels;
uniform ivec2 image;
uniform int first;
uniform int rows;
uniform float alpha;
out vec4 color;
vec4 texel(ivec2 p) { return texelFetch(pixels, (p.y - first) * image.x + p.x).bgra; }
void main() {
  vec2 p = at - 0.5, f = floor(p), t = p - f;
  ivec2 a = clamp(ivec2(f), ivec2(0), image - 1), b = clamp(ivec2(f) + 1, ivec2(0), image - 1);
  if (a.y < first || a.y >= first + rows) discard;
  vec4 top = mix(texel(a), texel(ivec2(b.x, a.y)), t.x), bottom = mix(texel(ivec2(a.x, b.y)), texel(b), t.x);
  color = mix(top, bottom, t.y) * alpha;
}
)";

GLuint compile(GLenum type, const char* source) {
  GLuint shader = glCreateShader(type);
  glShaderSource(shader, 1, &source, nullptr);
  glCompileShader(shader);
  GLint ok = 0;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[2048] = {};
    glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
    std::fprintf(stderr, "Shader compilation failed: %s\n", log);
    glDeleteShader(shader);
    return 0;
  }
  return shader;
}

GLuint link(GLuint vertex, GLuint fragment) {
  if (!vertex || !fragment) return 0;
  GLuint program = glCreateProgram();
  glAttachShader(program, vertex); glAttachShader(program, fragment);
  glLinkProgram(program);
  GLint linked = 0;
  glGetProgramiv(program, GL_LINK_STATUS, &linked);
  if (!linked) {
    char log[2048] = {};
    glGetProgramInfoLog(program, sizeof(log), nullptr, log);
    std::fprintf(stderr, "Shader link failed: %s\n", log);
    glDeleteProgram(program);
    return 0;
  }
  return program;
}

} // namespace

bool GlPresenter::init(int width, int height) {
  if (width <= 0 || height <= 0 || program_) return false;
  width_ = width; height_ = height;
  synced_ = buffer_synced_ = false;
  GLuint vertex = compile(GL_VERTEX_SHADER, vertex_source);
  GLuint fragment = compile(GL_FRAGMENT_SHADER, fragment_source);
  GLuint buffer_fragment = compile(GL_FRAGMENT_SHADER, buffer_fragment_source);
  program_ = link(vertex, fragment);
  buffer_program_ = link(vertex, buffer_fragment);
  GLuint layer_vertex = compile(GL_VERTEX_SHADER, layer_vertex_source);
  GLuint layer_fragment = compile(GL_FRAGMENT_SHADER, layer_fragment_source);
  layer_program_ = link(layer_vertex, layer_fragment);
  for (GLuint shader : {vertex, fragment, buffer_fragment, layer_vertex, layer_fragment})
    if (shader) glDeleteShader(shader);
  if (!program_) {
    release();
    return false;
  }
  glGenVertexArrays(1, &vao_);
  glGenTextures(1, &texture_);
  glBindTexture(GL_TEXTURE_2D, texture_);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  if (glGetError() != GL_NO_ERROR) return false;
  GLint max_texels = 0;
  glGetIntegerv(GL_MAX_TEXTURE_BUFFER_SIZE, &max_texels);
  max_texels_ = max_texels;
  if (buffer_program_ && max_texels / width > 0) {
    // Equal slices, each as tall as the limit allows: 2 at 1080p, 8 at 2160p.
    const int slices = (height + max_texels / width - 1) / (max_texels / width);
    slice_rows_ = (height + slices - 1) / slices;
    buffers_.resize(slices);
    buffer_textures_.resize(slices);
    glGenBuffers(slices, buffers_.data());
    glGenTextures(slices, buffer_textures_.data());
    for (int i = 0; i < slices; ++i) {
      glBindBuffer(GL_TEXTURE_BUFFER, buffers_[i]);
      glBufferData(GL_TEXTURE_BUFFER, static_cast<GLsizeiptr>(width) * slice_rows_ * 4, nullptr, GL_DYNAMIC_DRAW);
      glBindTexture(GL_TEXTURE_BUFFER, buffer_textures_[i]);
      glTexBuffer(GL_TEXTURE_BUFFER, GL_RGBA8, buffers_[i]);
    }
    glBindBuffer(GL_TEXTURE_BUFFER, 0);
    glBindTexture(GL_TEXTURE_BUFFER, 0);
  }
  // Without a buffer texture every size presents through the 2D texture.
  if (glGetError() != GL_NO_ERROR) release_buffer();
  video_.init(true);
  return true;
}

bool GlPresenter::draw(const std::uint32_t* argb, int sw, int sh) {
  if (!program_ || !argb || sw <= 0 || sh <= 0) return false;
  synced_ = buffer_synced_ = false;
  if (one_to_one(sw, sh)) {
    upload_rows(argb, 0, height_);
  } else {
    glBindTexture(GL_TEXTURE_2D, texture_);
    upload(argb, {0, 0, width_, height_});
  }
  return present(sw, sh);
}

namespace {
std::uint32_t micros_since(std::chrono::steady_clock::time_point start) {
  return static_cast<std::uint32_t>(
    std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
}
} // namespace

bool GlPresenter::draw(const std::uint32_t* argb, std::span<const ERRect> damage, std::span<const DamageMove> moves,
                       int sw, int sh) {
  if (!program_ || !argb || sw <= 0 || sh <= 0) return false;
  timing_ = {};
  if (one_to_one(sw, sh)) {
    synced_ = false;
    const auto start = std::chrono::steady_clock::now();
    if (!buffer_synced_) {
      upload_rows(argb, 0, height_);
      buffer_synced_ = true;
    } else {
      upload_changed_rows(argb, damage, moves);
    }
    timing_.upload_us = micros_since(start);
    const auto drawing = std::chrono::steady_clock::now();
    const bool ok = present(sw, sh);
    timing_.draw_us = micros_since(drawing);
    return ok;
  }
  buffer_synced_ = false;
  const auto start = std::chrono::steady_clock::now();
  glBindTexture(GL_TEXTURE_2D, texture_);
  // A new surface size also covers fullscreen changes and lost drawables.
  if (!synced_ || sw != surface_width_ || sh != surface_height_) {
    upload(argb, {0, 0, width_, height_});
    synced_ = true;
    surface_width_ = sw; surface_height_ = sh;
  } else {
    bool moved = true;
    for (const DamageMove& m : moves)
      if (!move(m)) {
        upload(argb, {0, 0, width_, height_});
        moved = false;
        break;
      }
    if (moved) {
      glBindTexture(GL_TEXTURE_2D, texture_);
      for (const ERRect& rect : damage) upload(argb, rect);
    }
  }
  timing_.upload_us = micros_since(start);
  const auto drawing = std::chrono::steady_clock::now();
  const bool ok = present(sw, sh);
  timing_.draw_us = micros_since(drawing);
  return ok;
}

bool GlPresenter::one_to_one(int sw, int sh) const { return !buffer_textures_.empty() && sw == width_ && sh == height_; }

// The buffer holds whole framebuffer rows, so a move needs no GPU copy: the rows it wrote are uploaded
// like damage. Close row ranges go up as one call.
void GlPresenter::upload_changed_rows(const std::uint32_t* argb, std::span<const ERRect> damage,
                                      std::span<const DamageMove> moves) {
  struct Rows { int y0, y1; };
  Rows rows[ER_DAMAGE_RECTS_MAX + 8];
  int count = 0;
  for (const ERRect& r : damage) rows[count++] = {r.y, r.y + r.h};
  for (const DamageMove& m : moves) rows[count++] = {m.src.y + m.dy, m.src.y + m.dy + m.src.h};
  std::sort(rows, rows + count, [](const Rows& a, const Rows& b) { return a.y0 < b.y0; });
  constexpr int gap = 16;
  for (int i = 0; i < count;) {
    int y0 = rows[i].y0, y1 = rows[i].y1;
    for (++i; i < count && rows[i].y0 <= y1 + gap; ++i) y1 = std::max(y1, rows[i].y1);
    upload_rows(argb, std::max(y0, 0), std::min(y1, height_));
  }
}

void GlPresenter::upload_rows(const std::uint32_t* argb, int y0, int y1) {
  const GLsizeiptr row = static_cast<GLsizeiptr>(width_) * 4;
  for (int i = 0; i < int(buffers_.size()); ++i) {
    const int first = i * slice_rows_, a = std::max(y0, first), b = std::min(y1, first + slice_rows_);
    if (b <= a) continue;
    glBindBuffer(GL_TEXTURE_BUFFER, buffers_[i]);
    glBufferSubData(GL_TEXTURE_BUFFER, (a - first) * row, (b - a) * row, argb + static_cast<std::size_t>(a) * width_);
    timing_.upload_bytes += static_cast<std::uint64_t>(b - a) * row;
  }
  glBindBuffer(GL_TEXTURE_BUFFER, 0);
}

void GlPresenter::upload(const std::uint32_t* argb, const ERRect& rect) {
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, width_);
  glTexSubImage2D(GL_TEXTURE_2D, 0, rect.x, rect.y, rect.w, rect.h, GL_RGBA, GL_UNSIGNED_BYTE,
                  argb + static_cast<std::size_t>(rect.y) * width_ + rect.x);
  timing_.upload_bytes += static_cast<std::uint64_t>(rect.w) * rect.h * 4;
}

// Texture rows are framebuffer rows, so framebuffer coordinates address both blits directly.
bool GlPresenter::move(const DamageMove& m) {
  if (!scratch_) {
    glGenTextures(1, &scratch_);
    glBindTexture(GL_TEXTURE_2D, scratch_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width_, height_, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glGenFramebuffers(1, &read_fbo_);
    glGenFramebuffers(1, &draw_fbo_);
  }
  const ERRect& r = m.src;
  const auto blit = [&](unsigned int from, unsigned int to, int dx, int dy) {
    glBindFramebuffer(GL_READ_FRAMEBUFFER, read_fbo_);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, from, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw_fbo_);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, to, 0);
    glBlitFramebuffer(r.x, r.y, r.x + r.w, r.y + r.h, r.x + dx, r.y + dy, r.x + dx + r.w, r.y + dy + r.h,
                      GL_COLOR_BUFFER_BIT, GL_NEAREST);
  };
  blit(texture_, scratch_, 0, 0);
  blit(scratch_, texture_, m.dx, m.dy);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  return glGetError() == GL_NO_ERROR;
}

const GlPresenter::LayerImage* GlPresenter::layer_image(const ERLayer& layer) {
  for (LayerImage& image : layer_images_)
    if (image.name == layer.image) {
      image.drawn = presents_;
      return &image;
    }
  // A slice holds its rows and the next one, within the texel limit per buffer texture.
  const int rows = max_texels_ / std::max(layer.image_w, 1) - 1;
  if (rows < 1 || !layer.pixels) return nullptr;
  LayerImage image{layer.image, layer.image_w, layer.image_h, rows, {}, {}, presents_};
  const int slices = (layer.image_h + rows - 1) / rows;
  image.buffers.resize(slices);
  image.textures.resize(slices);
  glGenBuffers(slices, image.buffers.data());
  glGenTextures(slices, image.textures.data());
  for (int i = 0; i < slices; ++i) {
    const int first = i * rows, count = std::min(rows + 1, layer.image_h - first);
    glBindBuffer(GL_TEXTURE_BUFFER, image.buffers[i]);
    timing_.upload_bytes += static_cast<std::uint64_t>(layer.image_w) * count * 4;
    glBufferData(GL_TEXTURE_BUFFER, static_cast<GLsizeiptr>(layer.image_w) * count * 4,
                 layer.pixels + static_cast<std::size_t>(first) * layer.image_w, GL_STATIC_DRAW);
    glBindTexture(GL_TEXTURE_BUFFER, image.textures[i]);
    glTexBuffer(GL_TEXTURE_BUFFER, GL_RGBA8, image.buffers[i]);
  }
  glBindBuffer(GL_TEXTURE_BUFFER, 0);
  layer_images_.push_back(std::move(image));
  return &layer_images_.back();
}

// Frees the least recently drawn images until at most `keep` that this present did not draw remain.
void GlPresenter::free_layer_images(std::size_t keep) {
  std::sort(layer_images_.begin(), layer_images_.end(),
            [](const LayerImage& a, const LayerImage& b) { return a.drawn > b.drawn; });
  std::size_t idle = 0;
  std::erase_if(layer_images_, [&](LayerImage& image) {
    if (image.drawn == presents_ || ++idle <= keep) return false;
    glDeleteTextures(int(image.textures.size()), image.textures.data());
    glDeleteBuffers(int(image.buffers.size()), image.buffers.data());
    return true;
  });
}

// Draws the layers into the viewport (x, y, w, h), which shows the whole framebuffer, then leaves
// premultiplied blending on for the framebuffer drawn over them. Textures no layer used are freed.
void GlPresenter::use_layer_program() {
  glBindVertexArray(vao_);
  glActiveTexture(GL_TEXTURE0);
  glUseProgram(layer_program_);
  glUniform1i(glGetUniformLocation(layer_program_, "pixels"), 0);
  glUniform2f(glGetUniformLocation(layer_program_, "size"), float(width_), float(height_));
}

void GlPresenter::draw_layers(int x, int y, int w, int h) {
  glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
  glEnable(GL_BLEND);
  glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
  use_layer_program();
  const GLint dst = glGetUniformLocation(layer_program_, "dst"), src = glGetUniformLocation(layer_program_, "src"),
              alpha = glGetUniformLocation(layer_program_, "alpha"), size = glGetUniformLocation(layer_program_, "image"),
              first = glGetUniformLocation(layer_program_, "first"), rows = glGetUniformLocation(layer_program_, "rows");
  const float sx = float(w) / width_, sy = float(h) / height_;
  glEnable(GL_SCISSOR_TEST);
  for (const ERLayer& layer : layers_) {
    if (layer.clip.w <= 0 || layer.clip.h <= 0 || layer.alpha == 0 || !layer.pixels) continue;
    // A <Video> element's placeholder: the player's frame is drawn in its place.
    if (video_layers::Layer playing; video_layers::find(layer.image, playing)) {
      if (playing.frame) {
        const GlVideo::Target target{layer.dst.x, layer.dst.y, layer.dst.x + layer.dst.w, layer.dst.y + layer.dst.h,
                                     layer.clip.x, layer.clip.y, layer.clip.x + layer.clip.w,
                                     layer.clip.y + layer.clip.h, layer.alpha / 255.0f};
        video_.draw(playing.id, *playing.frame, playing.fit, target, x, y, w, h, width_, height_, playing.upscale);
        use_layer_program();
      }
      continue;
    }
    // GL counts scissor rows from the bottom of the window.
    const int x0 = x + int(layer.clip.x * sx + 0.5f), x1 = x + int((layer.clip.x + layer.clip.w) * sx + 0.5f);
    const int y0 = y + h - int((layer.clip.y + layer.clip.h) * sy + 0.5f), y1 = y + h - int(layer.clip.y * sy + 0.5f);
    const LayerImage* image = layer_image(layer);
    if (!image) continue;
    glScissor(x0, y0, x1 - x0, y1 - y0);
    glUniform4f(dst, float(layer.dst.x), float(layer.dst.y), float(layer.dst.x + layer.dst.w),
                float(layer.dst.y + layer.dst.h));
    glUniform4f(src, float(layer.src_x), float(layer.src_y), float(layer.src_x + layer.src_w),
                float(layer.src_y + layer.src_h));
    glUniform2i(size, image->width, image->height);
    glUniform1i(rows, image->rows);
    glUniform1f(alpha, layer.alpha / 255.0f);
    for (int i = 0; i < int(image->textures.size()); ++i) {
      glBindTexture(GL_TEXTURE_BUFFER, image->textures[i]);
      glUniform1i(first, i * image->rows);
      glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
  }
  glDisable(GL_SCISSOR_TEST);
  glBindTexture(GL_TEXTURE_BUFFER, 0);
  free_layer_images(kKeptLayerImages);
}

bool GlPresenter::present(int sw, int sh) {
  struct VideoDone {
    GlVideo& video;
    ~VideoDone() { video.end_present(); }
  } video_done{video_};
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glDisable(GL_SCISSOR_TEST);
  glDisable(GL_FRAMEBUFFER_SRGB);
  glBindVertexArray(vao_);
  glActiveTexture(GL_TEXTURE0);
  ++presents_;
  const bool layered = !layers_.empty() && layers_supported();
  if (one_to_one(sw, sh)) {
    glViewport(0, 0, sw, sh);
    if (layered) draw_layers(0, 0, sw, sh);
    glUseProgram(buffer_program_);
    glUniform1i(glGetUniformLocation(buffer_program_, "rows"), 0);
    glUniform2i(glGetUniformLocation(buffer_program_, "size"), width_, height_);
    const GLint first = glGetUniformLocation(buffer_program_, "first");
    // The scissor keeps each slice's draw to its own rows; GL counts them from the bottom.
    glEnable(GL_SCISSOR_TEST);
    for (int i = 0; i < int(buffer_textures_.size()); ++i) {
      const int y0 = i * slice_rows_, y1 = std::min(y0 + slice_rows_, height_);
      glScissor(0, height_ - y1, width_, y1 - y0);
      glBindTexture(GL_TEXTURE_BUFFER, buffer_textures_[i]);
      glUniform1i(first, y0);
      glDrawArrays(GL_TRIANGLES, 0, 3);
    }
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glBindTexture(GL_TEXTURE_BUFFER, 0);
    return glGetError() == GL_NO_ERROR;
  }
  glViewport(0, 0, sw, sh);
  glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
  const float scale = std::min(float(sw) / width_, float(sh) / height_);
  const int vw = int(width_ * scale), vh = int(height_ * scale);
  glViewport((sw-vw)/2, (sh-vh)/2, vw, vh);
  if (layered) draw_layers((sw - vw) / 2, (sh - vh) / 2, vw, vh);
  glBindVertexArray(vao_);
  glBindTexture(GL_TEXTURE_2D, texture_);
  glUseProgram(program_);
  glUniform1i(glGetUniformLocation(program_, "frame"), 0);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  glDisable(GL_BLEND);
  return glGetError() == GL_NO_ERROR;
}

void GlPresenter::release_buffer() {
  if (!buffer_textures_.empty()) glDeleteTextures(int(buffer_textures_.size()), buffer_textures_.data());
  if (!buffers_.empty()) glDeleteBuffers(int(buffers_.size()), buffers_.data());
  buffer_textures_.clear(); buffers_.clear();
  buffer_synced_ = false;
}

void GlPresenter::release() {
  video_.release();
  release_buffer();
  for (const LayerImage& image : layer_images_) {
    glDeleteTextures(int(image.textures.size()), image.textures.data());
    glDeleteBuffers(int(image.buffers.size()), image.buffers.data());
  }
  layer_images_.clear();
  layers_ = {};
  if (layer_program_) glDeleteProgram(layer_program_);
  layer_program_ = 0;
  if (buffer_program_) glDeleteProgram(buffer_program_);
  buffer_program_ = 0;
  if (texture_) glDeleteTextures(1, &texture_);
  if (scratch_) glDeleteTextures(1, &scratch_);
  if (read_fbo_) glDeleteFramebuffers(1, &read_fbo_);
  if (draw_fbo_) glDeleteFramebuffers(1, &draw_fbo_);
  if (vao_) glDeleteVertexArrays(1, &vao_);
  if (program_) glDeleteProgram(program_);
  texture_ = vao_ = program_ = scratch_ = read_fbo_ = draw_fbo_ = 0;
  synced_ = false;
}
