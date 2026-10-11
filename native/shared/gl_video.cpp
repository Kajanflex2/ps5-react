// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
#include "gl_video.hpp"

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
#include <cmath>
#include <cstdio>
#include <string>

namespace {
const char* vertex_source = R"(#version 410 core
uniform vec4 dst;   // framebuffer pixels: x0 y0 x1 y1
uniform vec4 src;   // the part of the picture drawn, 0..1: x0 y0 x1 y1
uniform vec2 size;  // the framebuffer
out vec2 at;
void main() {
  vec2 corner = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));
  vec2 p = mix(dst.xy, dst.zw, corner);
  gl_Position = vec4(p.x / size.x * 2.0 - 1.0, 1.0 - p.y / size.y * 2.0, 0.0, 1.0);
  at = mix(src.xy, src.zw, corner);
}
)";

// YUV to display RGB, shared by both programs. `yuv` holds normalized samples.
const char* color_source = R"(
uniform mat3 matrix;     // Y'CbCr (range-expanded) to R'G'B'
uniform vec3 offset;     // subtracted first: black level and chroma zero
uniform vec3 gain;       // range expansion per component
uniform int transfer;    // 0 SDR, 1 PQ, 2 HLG
uniform float alpha;
out vec4 color;

vec3 pq_to_nits(vec3 e) {
  const float m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
  vec3 p = pow(clamp(e, 0.0, 1.0), vec3(1.0 / m2));
  return pow(max(p - c1, 0.0) / (c2 - c3 * p), vec3(1.0 / m1)) * 10000.0;
}
vec3 hlg_to_nits(vec3 e) {
  const float a = 0.17883277, b = 0.28466892, c = 0.55991073;
  vec3 lo = e * e / 3.0, hi = (exp((e - c) / a) + b) / 12.0;
  vec3 scene = mix(lo, hi, step(0.5, e));
  float luma = dot(scene, vec3(0.2627, 0.6780, 0.0593));
  return scene * pow(max(luma, 1e-6), 0.2) * 1000.0; // the BT.2100 OOTF for a 1000-nit display
}
// HDR to SDR: BT.2020 to BT.709 primaries, reference white (203 nits) to 1, a soft shoulder above 0.75
// on the brightest channel so hues hold, then the display's 2.4 gamma.
vec3 tone_map(vec3 nits) {
  const mat3 to709 = mat3(1.6605, -0.1246, -0.0182, -0.5876, 1.1329, -0.1006, -0.0728, -0.0083, 1.1187);
  vec3 rgb = max(to709 * (nits / 203.0), 0.0);
  float peak = max(max(rgb.r, rgb.g), rgb.b);
  const float knee = 0.75;
  if (peak > knee) rgb *= (knee + (1.0 - knee) * tanh((peak - knee) / (1.0 - knee))) / peak;
  return pow(rgb, vec3(1.0 / 2.4));
}
void finish(vec3 yuv) {
  vec3 rgb = matrix * ((yuv - offset) * gain);
  if (transfer == 1) rgb = tone_map(pq_to_nits(rgb));
  else if (transfer == 2) rgb = tone_map(hlg_to_nits(rgb));
  color = vec4(clamp(rgb, 0.0, 1.0), 1.0) * alpha;
}
)";

const char* texture_fragment = R"(#version 410 core
in vec2 at;
uniform sampler2D plane0, plane1, plane2;
uniform int interleaved;
uniform float scale;     // 10-bit samples low in 16-bit words: 65535 / 1023
)";
const char* texture_main = R"(
void main() {
  float y = texture(plane0, at).r;
  vec2 c = interleaved == 1 ? texture(plane1, at).rg : vec2(texture(plane1, at).r, texture(plane2, at).r);
  finish(vec3(y, c) * scale);
}
)";

// The PS5 path: the frame's planes, as uploaded, in one buffer the shader reads samples from.
[[maybe_unused]] const char* storage_fragment = R"(#version 430 core
in vec2 at;
layout(std430, binding = 0) readonly buffer Planes { uint words[]; };
uniform ivec3 offsets;   // byte offset of each plane
uniform ivec3 pitches;   // bytes per row
uniform ivec2 luma;      // plane sizes in samples
uniform ivec2 chroma;
uniform int wide;        // 16-bit samples
uniform int interleaved;
uniform float scale;
float sample_at(int at) {
  uint word = words[at >> 2];
  if (wide == 1) return float((word >> uint((at & 2) * 8)) & 0xFFFFu) / 65535.0;
  return float((word >> uint((at & 3) * 8)) & 0xFFu) / 255.0;
}
// Bilinear between sample centres with clamped edges, as GL_LINEAR. `step` is bytes per sample pair
// position: 1 or 2 for a plane of its own, 2 or 4 for interleaved chroma, whose second component
// sits `second` bytes on.
float bilinear(int base, int pitch, ivec2 size, int step, int second, vec2 pos) {
  vec2 p = pos * vec2(size) - 0.5, f = floor(p), t = p - f;
  ivec2 a = clamp(ivec2(f), ivec2(0), size - 1), b = clamp(ivec2(f) + 1, ivec2(0), size - 1);
  float s00 = sample_at(base + a.y * pitch + a.x * step + second), s10 = sample_at(base + a.y * pitch + b.x * step + second);
  float s01 = sample_at(base + b.y * pitch + a.x * step + second), s11 = sample_at(base + b.y * pitch + b.x * step + second);
  return mix(mix(s00, s10, t.x), mix(s01, s11, t.x), t.y);
}
)";
[[maybe_unused]] const char* storage_main = R"(
void main() {
  int bytes = wide + 1;
  float y = bilinear(offsets.x, pitches.x, luma, bytes, 0, at);
  vec2 c;
  if (interleaved == 1) {
    c = vec2(bilinear(offsets.y, pitches.y, chroma, bytes * 2, 0, at), bilinear(offsets.y, pitches.y, chroma, bytes * 2, bytes, at));
  } else {
    c = vec2(bilinear(offsets.y, pitches.y, chroma, bytes, 0, at), bilinear(offsets.z, pitches.z, chroma, bytes, 0, at));
  }
  finish(vec3(y, c) * scale);
}
)";

GLuint compile(GLenum type, const std::string& source) {
  GLuint shader = glCreateShader(type);
  const char* text = source.c_str();
  glShaderSource(shader, 1, &text, nullptr);
  glCompileShader(shader);
  GLint ok = 0;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[2048] = {};
    glGetShaderInfoLog(shader, sizeof log, nullptr, log);
    std::fprintf(stderr, "Video shader compilation failed: %s\n", log);
    glDeleteShader(shader);
    return 0;
  }
  return shader;
}

GLuint program(const std::string& fragment_source) {
  GLuint vertex = compile(GL_VERTEX_SHADER, vertex_source), fragment = compile(GL_FRAGMENT_SHADER, fragment_source);
  GLuint linked = 0;
  if (vertex && fragment) {
    linked = glCreateProgram();
    glAttachShader(linked, vertex);
    glAttachShader(linked, fragment);
    glLinkProgram(linked);
    GLint ok = 0;
    glGetProgramiv(linked, GL_LINK_STATUS, &ok);
    if (!ok) {
      char log[2048] = {};
      glGetProgramInfoLog(linked, sizeof log, nullptr, log);
      std::fprintf(stderr, "Video shader link failed: %s\n", log);
      glDeleteProgram(linked);
      linked = 0;
    }
  }
  if (vertex) glDeleteShader(vertex);
  if (fragment) glDeleteShader(fragment);
  return linked;
}

using video::interleaved;
bool wide(video::Format format) { return video::deep(format); }

std::uint32_t micros_since(std::chrono::steady_clock::time_point start) {
  return static_cast<std::uint32_t>(
    std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
}

// Kr and Kb of each matrix (BT.601, BT.709, BT.2020).
void set_color(GLuint program, const video::Frame& frame, float alpha) {
  const float kr[] = {0.299f, 0.2126f, 0.2627f}, kb[] = {0.114f, 0.0722f, 0.0593f};
  const int m = static_cast<int>(frame.matrix);
  const float r = kr[m], b = kb[m], g = 1 - r - b;
  // Column-major: R' = Y + 2(1-Kr) Cr; B' = Y + 2(1-Kb) Cb; G' from the luma equation.
  const float matrix[9] = {1, 1, 1,
                           0, -2 * (1 - b) * b / g, 2 * (1 - b),
                           2 * (1 - r), -2 * (1 - r) * r / g, 0};
  glUniformMatrix3fv(glGetUniformLocation(program, "matrix"), 1, GL_FALSE, matrix);
  if (frame.full_range) {
    glUniform3f(glGetUniformLocation(program, "offset"), 0, 0.5f, 0.5f);
    glUniform3f(glGetUniformLocation(program, "gain"), 1, 1, 1);
  } else {
    glUniform3f(glGetUniformLocation(program, "offset"), 16 / 255.0f, 128 / 255.0f, 128 / 255.0f);
    glUniform3f(glGetUniformLocation(program, "gain"), 255 / 219.0f, 255 / 224.0f, 255 / 224.0f);
  }
  glUniform1i(glGetUniformLocation(program, "transfer"), static_cast<int>(frame.transfer));
  glUniform1f(glGetUniformLocation(program, "alpha"), alpha);
  glUniform1i(glGetUniformLocation(program, "interleaved"), interleaved(frame.format) ? 1 : 0);
  glUniform1f(glGetUniformLocation(program, "scale"), video::sample_scale(frame.format));
}
} // namespace

bool GlVideo::init(bool storage_allowed) {
  release();
  texture_program_ = program(std::string(texture_fragment) + color_source + texture_main);
#ifdef GL_SHADER_STORAGE_BUFFER
  GLint major = 0, minor = 0;
  glGetIntegerv(GL_MAJOR_VERSION, &major);
  glGetIntegerv(GL_MINOR_VERSION, &minor);
  if (storage_allowed && (major > 4 || (major == 4 && minor >= 3)))
    storage_program_ = program(std::string(storage_fragment) + color_source + storage_main);
#else
  (void)storage_allowed;
#endif
  mode_ = storage_program_ ? Upload::storage : Upload::textures;
  glGenVertexArrays(1, &vao_);
  while (glGetError() != GL_NO_ERROR) {}
  return texture_program_ || storage_program_;
}

void GlVideo::release() {
  for (Entry& entry : entries_) free_entry(entry);
  entries_.clear();
  if (texture_program_) glDeleteProgram(texture_program_);
  if (storage_program_) glDeleteProgram(storage_program_);
  if (vao_) glDeleteVertexArrays(1, &vao_);
  texture_program_ = storage_program_ = vao_ = 0;
}

void GlVideo::free_entry(Entry& entry) {
  for (unsigned int& texture : entry.textures)
    if (texture) glDeleteTextures(1, &texture);
  if (entry.buffer) glDeleteBuffers(1, &entry.buffer);
  entry = {};
}

GlVideo::Entry& GlVideo::entry(std::uint64_t id) {
  for (Entry& entry : entries_)
    if (entry.id == id) return entry;
  entries_.push_back({});
  entries_.back().id = id;
  return entries_.back();
}

void GlVideo::upload_textures(Entry& entry, const video::Frame& frame) {
  const bool deep = wide(frame.format), pairs = interleaved(frame.format);
  const int cw = (frame.width + 1) / 2, ch = (frame.height + 1) / 2;
  const int planes = pairs ? 2 : 3;
  if (entry.format != frame.format || entry.width != frame.width || entry.height != frame.height || !entry.textures[0]) {
    for (unsigned int& texture : entry.textures)
      if (texture) glDeleteTextures(1, &texture);
    glGenTextures(planes, entry.textures);
    for (int i = 0; i < planes; ++i) {
      glBindTexture(GL_TEXTURE_2D, entry.textures[i]);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      const bool two = pairs && i == 1;
      const GLenum internal = deep ? (two ? GL_RG16 : GL_R16) : (two ? GL_RG8 : GL_R8);
      glTexImage2D(GL_TEXTURE_2D, 0, internal, i ? cw : frame.width, i ? ch : frame.height, 0, two ? GL_RG : GL_RED,
                   deep ? GL_UNSIGNED_SHORT : GL_UNSIGNED_BYTE, nullptr);
    }
    entry.format = frame.format;
    entry.width = frame.width;
    entry.height = frame.height;
  }
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  for (int i = 0; i < planes; ++i) {
    const bool two = pairs && i == 1;
    const int element = (deep ? 2 : 1) * (two ? 2 : 1);
    glBindTexture(GL_TEXTURE_2D, entry.textures[i]);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, frame.pitch[i] / element);
    const int w = i ? cw : frame.width, h = i ? ch : frame.height;
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, two ? GL_RG : GL_RED, deep ? GL_UNSIGNED_SHORT : GL_UNSIGNED_BYTE,
                    frame.planes[i]);
    upload_bytes_ += static_cast<std::uint64_t>(frame.pitch[i]) * h;
  }
  glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
}

void GlVideo::upload_storage(Entry& entry, const video::Frame& frame) {
#ifdef GL_SHADER_STORAGE_BUFFER
  const int ch = (frame.height + 1) / 2;
  const int planes = interleaved(frame.format) ? 2 : 3;
  std::size_t total = 0;
  for (int i = 0; i < planes; ++i) {
    entry.offsets[i] = static_cast<int>(total);
    entry.pitches[i] = frame.pitch[i];
    total += (static_cast<std::size_t>(frame.pitch[i]) * (i ? ch : frame.height) + 3) & ~std::size_t(3);
  }
  if (!entry.buffer) glGenBuffers(1, &entry.buffer);
  glBindBuffer(GL_SHADER_STORAGE_BUFFER, entry.buffer);
  // A new store each frame: the GPU may still read the previous one.
  glBufferData(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(total), nullptr, GL_STREAM_DRAW);
  for (int i = 0; i < planes; ++i)
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, entry.offsets[i],
                    static_cast<GLsizeiptr>(frame.pitch[i]) * (i ? ch : frame.height), frame.planes[i]);
  glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
  entry.buffer_size = total;
  entry.format = frame.format;
  entry.width = frame.width;
  entry.height = frame.height;
  upload_bytes_ += total;
#else
  (void)entry;
  (void)frame;
#endif
}

void GlVideo::draw(std::uint64_t id, const video::Frame& frame, Fit fit, const Target& target, int x, int y, int w,
                   int h, int fb_w, int fb_h) {
  if (!frame.planes[0] || frame.width <= 0 || frame.height <= 0) return;
  Entry& e = entry(id);
  e.drawn = true;
  if (e.sequence != frame.sequence) {
    const auto start = std::chrono::steady_clock::now();
    if (mode_ == Upload::storage) upload_storage(e, frame);
    else upload_textures(e, frame);
    e.sequence = frame.sequence;
    upload_us_ += micros_since(start);
  }
  // The picture's shape, fitted into the element's box.
  const float bw = float(target.x1 - target.x0), bh = float(target.y1 - target.y0);
  if (bw <= 0 || bh <= 0) return;
  const float picture = frame.width * frame.aspect / frame.height, box = bw / bh;
  float dx0 = target.x0, dy0 = target.y0, dx1 = target.x1, dy1 = target.y1;
  float sx0 = 0, sy0 = 0, sx1 = 1, sy1 = 1;
  if (fit == Fit::contain) {
    if (picture > box) {
      const float hh = bw / picture;
      dy0 = target.y0 + (bh - hh) / 2;
      dy1 = dy0 + hh;
    } else {
      const float ww = bh * picture;
      dx0 = target.x0 + (bw - ww) / 2;
      dx1 = dx0 + ww;
    }
  } else if (fit == Fit::cover) {
    if (picture > box) {
      const float part = box / picture;
      sx0 = (1 - part) / 2;
      sx1 = sx0 + part;
    } else {
      const float part = picture / box;
      sy0 = (1 - part) / 2;
      sy1 = sy0 + part;
    }
  }
  const GLuint used = mode_ == Upload::storage ? storage_program_ : texture_program_;
  glUseProgram(used);
  glBindVertexArray(vao_);
  glUniform4f(glGetUniformLocation(used, "dst"), dx0, dy0, dx1, dy1);
  glUniform4f(glGetUniformLocation(used, "src"), sx0, sy0, sx1, sy1);
  glUniform2f(glGetUniformLocation(used, "size"), float(fb_w), float(fb_h));
  set_color(used, frame, target.alpha);
  if (mode_ == Upload::storage) {
#ifdef GL_SHADER_STORAGE_BUFFER
    glUniform3i(glGetUniformLocation(used, "offsets"), e.offsets[0], e.offsets[1], e.offsets[2]);
    glUniform3i(glGetUniformLocation(used, "pitches"), e.pitches[0], e.pitches[1], e.pitches[2]);
    glUniform2i(glGetUniformLocation(used, "luma"), frame.width, frame.height);
    glUniform2i(glGetUniformLocation(used, "chroma"), (frame.width + 1) / 2, (frame.height + 1) / 2);
    glUniform1i(glGetUniformLocation(used, "wide"), wide(frame.format) ? 1 : 0);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, e.buffer);
#endif
  } else {
    const int planes = interleaved(frame.format) ? 2 : 3;
    for (int i = 0; i < planes; ++i) {
      glActiveTexture(GL_TEXTURE0 + i);
      glBindTexture(GL_TEXTURE_2D, e.textures[i]);
    }
    glUniform1i(glGetUniformLocation(used, "plane0"), 0);
    glUniform1i(glGetUniformLocation(used, "plane1"), 1);
    glUniform1i(glGetUniformLocation(used, "plane2"), 2);
  }
  // GL counts scissor rows from the bottom of the window.
  const float sx = float(w) / fb_w, sy = float(h) / fb_h;
  const int cx0 = x + int(target.clip_x0 * sx + 0.5f), cx1 = x + int(target.clip_x1 * sx + 0.5f);
  const int cy0 = y + h - int(target.clip_y1 * sy + 0.5f), cy1 = y + h - int(target.clip_y0 * sy + 0.5f);
  glEnable(GL_SCISSOR_TEST);
  glScissor(cx0, cy0, std::max(cx1 - cx0, 0), std::max(cy1 - cy0, 0));
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  if (mode_ == Upload::textures) {
    for (int i = 2; i >= 0; --i) {
      glActiveTexture(GL_TEXTURE0 + i);
      glBindTexture(GL_TEXTURE_2D, 0);
    }
  }
}

void GlVideo::end_present() {
  std::erase_if(entries_, [&](Entry& entry) {
    if (entry.drawn) {
      entry.drawn = false;
      return false;
    }
    free_entry(entry);
    return true;
  });
}

