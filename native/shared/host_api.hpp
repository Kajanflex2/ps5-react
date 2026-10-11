// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
// Native API contract (ABI v7) between the hosts and `@ps5-react/core`. The input actions hosts
// pass to `__ps5ReactDispatch` are a separate contract with its own version (runtime/js/input.js).
//
// host_api.cpp (shared) installs `globalThis.__ps5ReactNative` through
// ErRuntimeConfig.install_host_globals and implements the filesystem with POSIX
// (mount listing excepted).
// Each platform implements the `host::` functions below. Everything runs on the
// render thread; the functions are synchronous and must return quickly.
//
// JavaScript shape (runtime/js/native.js wraps it):
//   platform                       'ps5' | 'desktop'
//   fs.readDir(path)               [{name, isDirectory, isFile, size, modified, device, inode}]
//   fs.stat(path)                  {name, isDirectory, isFile, size, modified, device, inode} | null
//                                  device/inode are decimal strings, preserving 64-bit IDs
//   fs.readFile(path)              string (UTF-8; at most kMaxReadBytes)
//   fs.writeFile(path, text, append, sync)  sync: fsync before close
//   fs.mkdir(path, recursive)
//   fs.remove(path)                file or empty directory
//   fs.removeTree(path)            a file, or a directory and everything in it; symbolic links are
//                                  unlinked, never followed; synchronous, for small app-owned trees
//   fs.rename(from, to)
//   fs.chmod(path, mode)           sets permission bits
//   fs.mounts()                    [{device, path, type}], visible to the process
//                                  console access is opt-in via app.json
//   fs.diskUsage(path)             {total, free} in bytes; free available to the app
//   device.info()                  {model, firmware, cpuTemperature, socTemperature,
//                                   cpuFrequency, freeMemory, processTime, language}; unknown = null
//                                  language: the system language as a BCP 47 tag ('pt-BR')
//   users.foreground()             {id, name} | null
//   users.loggedIn()               [{id, name}]
//   notify(message, subMessage)    boolean
//   openURL(url)                   boolean
//   pad.setLightBar(r, g, b)       0..255 each
//   pad.resetLightBar()
//   pad.vibrate(strength, seconds) strength 0..1
//   pad.state()                    {connected, leftX, leftY, rightX, rightY, l2, r2, buttons}
//   power.keepAwake(enabled)       ABI v4; while true, the console does not enter rest mode
//                                  for inactivity (the desktop display does not sleep)
//   network.download(url, path, options) task ID; queues native binary file I/O
//     options: expectedBytes, storageRoot, pieces[{url, offset, size, sha1}],
//              sha256, connections, adaptive, rangeBytes, resume, recoverCompleted, headers, rejectHtml
//   network.request(url, options)  task ID; queues bounded HTTP text I/O
//     options: method, body, maxBytes, headers, followRedirects (default true)
//     terminal snapshots: status, body, effective url, allowlisted lowercase headers
//   network.cancel(id)             requests cancellation
//   network.poll()                 progress/results; consumes terminal snapshots
//   archives.extract(sources, destination, maxBytes) task ID; bounded native archive worker
//   archives.inspect(sources)      {kind, refusal}; file type from leading bytes and, for an archive,
//                                  a decoder-memory refusal its headers so far justify; partial files ok
//   archives.cancel(id)            requests cancellation; source volumes remain intact
//   archives.poll()                state/error/written/entries/artifacts; consumes terminal results
//   browser.capture(prefix, suffix, timeoutSeconds) ABI v7; Vikingfile capture task bound to a filename, or to any file when suffix is "/"
//   browser.cancel(id)            stops the read-only payload by closing its connection
//   browser.poll()                state/error/url; consumes terminal results
//   packages.install(path, name)   task ID; installs a .pkg under /data, /mnt/usb or /mnt/ext through the
//                                  console's install service (PS5 only, needs the payload loader on 9021)
//   packages.cancel(id)            stops reporting; an install the console accepted carries on
//   packages.poll()                state/error/contentId/status/written/total; consumes terminal results
//   network.version()              transport version string
//   image.load(url, width, height, fit, prefetch) {id, state, name, width, height, error, color}; takes a
//                                  reference; fit: 0 cover, 1 contain, 2 stretch, 3 none; prefetch
//                                  (optional) loads after drawn images; state 'loading' |
//                                  'ready' | 'failed'; name is the engine image name when ready;
//                                  color is '#rrggbb', the art's most prominent vivid hue, or null
//   image.release(id, prefetch)    drops a reference taken by a load with the same prefetch;
//                                  cancels unfinished work without references
//   image.warm(urls)               fills the disk cache with `urls` in the background, after loads;
//                                  replaces the previous list
//   image.poll()                   [{id, state, name, width, height, error, color}] finished since last poll
//   sound.play(name, volume, pan)  ABI v5; boolean; posts an imported WAV to the mixer (volume
//                                  0..1, pan -1..1); false without audio or while it still plays
//   sound.setVolume(volume)        master gain 0..1 for every sound
//   text.setLanguage(tag)          ABI v6; BCP 47 language for runtime text (Han glyph forms,
//                                  shaping); defaults to the system language
//   exit(relaunch)                 asks the host to close after this frame; with relaunch true it
//                                  opens the app again once closed and returns true, or the reason
//                                  it cannot (PS5: needs the payload loader on 127.0.0.1:9021)
// Failures throw a JS Error whose message names the call, the path, and strerror.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace hui::audio {
class Mixer;
}

extern "C" {
#include "quickjs.h"
}

constexpr std::size_t kMaxReadBytes = 8 * 1024 * 1024;

// Shared (host_api.cpp).
void ps5_react_install_host_api(JSContext* ctx);
bool ps5_react_exit_requested();
// Calls `globalThis.__ps5ReactFrame(elapsedMs)` (runtime/js/frame.js) once per frame, after
// er_runtime_pump() and before er_commit(): per-frame motion steps by the time the frame advances
// on screen. False when it threw.
bool ps5_react_frame(JSContext* ctx, double elapsed_ms);

// Latest controller state, set by the host once per frame before er_runtime_pump().
struct GamepadState {
  bool connected = false;
  float left_x = 0, left_y = 0, right_x = 0, right_y = 0; // -1..1, deadzone applied
  float l2 = 0, r2 = 0;                                   // 0..1
  std::uint32_t buttons = 0;                              // bit i = kButtonNames[i] held
};
inline constexpr const char* kButtonNames[] = {
  "up", "down", "left", "right", "cross", "circle", "triangle", "square",
  "l1", "r1", "l2", "r2", "l3", "r3", "options", "touchpad",
};
void ps5_react_set_gamepad(const GamepadState& state);

// Per platform (native/ps5/host_platform.cpp, native/desktop/host_platform.cpp).
namespace host {
const char* platform_name();
// Maps an app path (/app0, /download0, /temp0, ...) to a real path. False rejects it.
bool resolve_path(const char* path, char* out, std::size_t size);

struct MountEntry {
  char device[128] = "";
  char path[128] = "";
  char type[16] = "";
};
// Fills at most max entries; returns the count, or -1 with errno set.
int list_mounts(MountEntry* mounts, int max);
// Calls visit for each entry name of a directory, skipping "." and "..";
// false with errno set when the directory cannot be read.
bool read_dir(const char* real_path, void (*visit)(const char* name, void* user), void* user);
// Total and free bytes of the filesystem holding real_path; false with errno set.
bool disk_usage(const char* real_path, double& total, double& free);
// Records the first use of each native call (PS5: kernel log), to locate native faults.
void trace(const char* call);
// The native call running on the render thread, or nullptr: the stall watchdog names it.
extern std::atomic<const char*> current_call;
struct CallScope {
  const char* outer;
  explicit CallScope(const char* name) : outer(current_call.exchange(name)) {}
  ~CallScope() { current_call = outer; }
};
// Opens the app again once it has closed: PS5 sends relauncher.elf to the payload loader, the desktop
// preview runs itself again. False with a reason in `error` (at most `size` bytes) when it cannot.
bool arrange_relaunch(char* error, std::size_t size);

struct DeviceInfo {
  char model[64] = "";
  char firmware[32] = "";
  int cpu_temperature = -1;      // °C, -1 unknown
  int soc_temperature = -1;      // °C, -1 unknown
  std::int64_t cpu_frequency = -1; // Hz
  std::int64_t free_memory = -1;   // bytes available to the app
  std::int64_t process_time = -1;  // µs of CPU time used by this process
  char language[16] = "";          // system language, BCP 47 ("en-US", "zh-Hans")
};
void device_info(DeviceInfo& info);

struct User {
  int id = -1;
  char name[32] = "";
};
bool foreground_user(User& user);
int logged_in_users(User* users, int max);

bool notify(const char* message, const char* sub_message);
bool open_url(const char* url);

void set_light_bar(std::uint8_t r, std::uint8_t g, std::uint8_t b);
void reset_light_bar();
void vibrate(float strength, float seconds);
// While enabled, keeps the system from sleeping for inactivity; the app's last call wins.
void keep_awake(bool enabled);

// Renders the mixer on an audio thread (PS5: sceAudioOut, desktop: SDL); false leaves the app silent.
bool start_audio(hui::audio::Mixer& mixer);
void stop_audio();
} // namespace host

// ABI v2: synchronous native task submission/polling; I/O runs off-thread.
JSValue ps5_react_network_api(JSContext* ctx);
// ABI v3: remote images fetched and decoded off-thread, registered with the engine on poll.
JSValue ps5_react_image_api(JSContext* ctx);
// Starts the image workers (after network::start) with the disk cache in the app's data directory.
bool ps5_react_start_images();
// Unregisters every remote image from the engine, then stops the image workers and frees them.
void ps5_react_stop_images();
// ABI v5: sounds the app imports (baked by tools/bundle.mjs), mixed on an audio thread.
JSValue ps5_react_sound_api(JSContext* ctx);
// Decodes the baked sounds and opens the audio output; false (silent) when the app has no sounds or
// the output fails. Start before the bundle runs, stop after the runtime shuts down.
bool ps5_react_start_sound();
void ps5_react_stop_sound();

// ABI v5 additive archive tasks: extract(sources, destination, maxBytes, password?), inspect(sources), cancel(id), poll().
JSValue ps5_react_archive_api(JSContext* ctx);
// ABI v5 additive package installs: install(path, name), cancel(id), poll().
JSValue ps5_react_package_api(JSContext* ctx);

// ABI v7: filename-bound browser capture tasks (PS5 only).
JSValue ps5_react_browser_capture_api(JSContext* ctx);
