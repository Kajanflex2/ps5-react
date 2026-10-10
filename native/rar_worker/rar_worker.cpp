// Copyright (C) 2026 half144
// SPDX-License-Identifier: MIT
//
// rar-extract: extracts one RAR set with RarLab's UnRAR library, in a process of its own. UnRAR's license
// does not let it be linked into the GPL engine, so the engine talks to this program over a stream
// (the PS5 payload loader's socket, or a pipe on the desktop) and never loads its code. Its own
// crashes and C++ exceptions stay in this process too.
//
// Request, one item per line: "RARX1", then "threads <n>" (0 picks from the cores), "window <bytes>",
// "limit <bytes>", "flatten <extension>"..., "reserved <text>", "password <hex>", "dest <directory>",
// "source <path>"... in volume order, "wait 1" when later volumes are still downloading (each is read once
// a file has its name), then "end". The engine can then write "cancel" at any time.
// Answers: "ready", "p <bytes written>" a few times a second, "k" every second, "f <sha256> <size>
// <path>" once a file is on disk and synced, "t <key>=<value>..." with where the time went (an engine that
// does not know a key ignores it), then "ok", "cancelled" or "fail <reason>".
#include "rar.hpp"
#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#else
#include <openssl/evp.h>
#endif
#include <chrono>
#include <condition_variable>
#include <deque>
#include <fcntl.h>
#include <mutex>
#include <poll.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <thread>
#include <time.h>
#include <unistd.h>

namespace {
constexpr size_t max_path_length = 4096, max_line = 8192, buffer_bytes = 4 << 20, buffer_count = 4;
// Each queued file holds its descriptor; empty files take no buffer, so this bounds how far ahead they open.
constexpr size_t max_queued_files = 32;
constexpr unsigned max_depth = 128;
constexpr uint64 progress_ns = 250000000;
const char* too_much_memory = "This archive needs more memory to extract than the console app can use.";

struct Options {
  unsigned threads = 0;
  uint64 window = 0, limit = 0;
  bool wait = false;
  std::vector<std::string> flatten, sources;
  std::string reserved, password, destination;
};

std::mutex say_mutex;
bool cancelled = false;  // Only the decoding thread reads and sets it.
bool output_lost = false;

void say(const std::string& line) {
  std::lock_guard lock(say_mutex);
  const std::string text = line + "\n";
  size_t done = 0;
  while (done < text.size() && !output_lost) {
    const ssize_t count = write(STDOUT_FILENO, text.data() + done, text.size() - done);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) output_lost = true;
    else done += count;
  }
}

uint64 now_ns() {
  timespec time;
  clock_gettime(CLOCK_MONOTONIC, &time);
  return uint64(time.tv_sec) * 1000000000 + time.tv_nsec;
}

// The request is read byte by byte so nothing past "end" (a later "cancel") is consumed early.
bool read_line(std::string& line) {
  line.clear();
  char c;
  while (line.size() < max_line) {
    const ssize_t count = read(STDIN_FILENO, &c, 1);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) return false;
    if (c == '\n') return true;
    line += c;
  }
  return false;
}

bool from_hex(const std::string& hex, std::string& out) {
  if (hex.size() % 2) return false;
  for (size_t i = 0; i < hex.size(); i += 2) {
    int value = 0;
    for (char c : hex.substr(i, 2)) {
      const int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
      if (digit < 0) return false;
      value = value * 16 + digit;
    }
    out += char(value);
  }
  return true;
}

bool read_request(Options& options) {
  std::string line;
  if (!read_line(line) || line != "RARX1") return false;
  while (read_line(line)) {
    const auto space = line.find(' ');
    const std::string key = line.substr(0, space), value = space == std::string::npos ? "" : line.substr(space + 1);
    if (key == "end") return !options.destination.empty() && !options.sources.empty() && options.limit && options.window;
    if (key == "threads") options.threads = unsigned(strtoul(value.c_str(), nullptr, 10));
    else if (key == "window") options.window = strtoull(value.c_str(), nullptr, 10);
    else if (key == "limit") options.limit = strtoull(value.c_str(), nullptr, 10);
    else if (key == "flatten") options.flatten.push_back(value);
    else if (key == "reserved") options.reserved = value;
    else if (key == "password" && !from_hex(value, options.password)) return false;
    else if (key == "dest") options.destination = value;
    else if (key == "wait") options.wait = value == "1";
    else if (key == "source") options.sources.push_back(value);
  }
  return false;
}

// True once the engine asked to stop or went away; checked a few times a second.
bool stop_requested() {
  pollfd input{STDIN_FILENO, POLLIN, 0};
  if (poll(&input, 1, 0) > 0) {
    std::string line;
    if (!read_line(line) || line == "cancel") return true;
  }
  return output_lost;
}

// "a/./b" is "a/b", as the engine reads it.
std::string without_dots(const std::string& name) {
  std::string out;
  size_t start = 0;
  while (start <= name.size()) {
    size_t end = name.find('/', start);
    if (end == std::string::npos) end = name.size();
    const std::string part = name.substr(start, end - start);
    if (part != ".") out += (out.empty() ? "" : "/") + part;
    start = end + 1;
  }
  return out;
}

// The same rule as the engine: a relative path of plain components, no separators or controls that
// another system would read differently.
bool safe(const std::string& name) {
  if (name.empty() || name.size() > max_path_length || name[0] == '/') return false;
  for (unsigned char c : name)
    if (c < 32 || c == 127 || c == '|' || c == '\\' || c == ':') return false;
  size_t start = 0;
  unsigned depth = 0;
  while (start < name.size()) {
    size_t end = name.find('/', start);
    if (end == std::string::npos) end = name.size();
    const std::string part = name.substr(start, end - start);
    if (part.empty() || part == ".." || ++depth > max_depth) return false;
    start = end + 1;
  }
  return true;
}

bool plain_directory(const std::string& path) {
  struct stat st;
  return !lstat(path.c_str(), &st) && S_ISDIR(st.st_mode);
}

// Creates the directories of `name` under `root`, none of them a link, and returns the full path.
bool make_parents(const std::string& root, const std::string& name, std::string& target) {
  target = root;
  size_t start = 0, end;
  while ((end = name.find('/', start)) != std::string::npos) {
    target += "/" + name.substr(start, end - start);
    if (mkdir(target.c_str(), 0777) && errno != EEXIST) return false;
    if (!plain_directory(target)) { errno = ENOTDIR; return false; }
    start = end + 1;
  }
  target += "/" + name.substr(start);
  return true;
}

// UnRAR's own SHA-256 is portable C and slower than decoding; these use the CPU's SHA instructions.
class Sha256 {
public:
#ifdef __APPLE__
  void init() { CC_SHA256_Init(&context_); }
  void update(const void* data, size_t size) { CC_SHA256_Update(&context_, data, CC_LONG(size)); }
  void finish(byte* digest) { CC_SHA256_Final(digest, &context_); }
#else
  Sha256() : context_(EVP_MD_CTX_new()) {}
  ~Sha256() { EVP_MD_CTX_free(context_); }
  void init() { EVP_DigestInit_ex(context_, EVP_sha256(), nullptr); }
  void update(const void* data, size_t size) { EVP_DigestUpdate(context_, data, size); }
  void finish(byte* digest) { EVP_DigestFinal_ex(context_, digest, nullptr); }
#endif

private:
#ifdef __APPLE__
  CC_SHA256_CTX context_;
#else
  EVP_MD_CTX* context_;
#endif
};

std::string hex(const byte* data, size_t size) {
  static const char digits[] = "0123456789abcdef";
  std::string out;
  for (size_t i = 0; i < size; ++i) { out += digits[data[i] >> 4]; out += digits[data[i] & 15]; }
  return out;
}

// Writes, hashes, syncs and closes files on a thread of its own, so decoding never waits on the disk.
// Memory is the fixed set of buffers; the decoder waits for a free one when the disk falls behind.
class Writer {
public:
  Writer() {
    for (auto& buffer : buffers_) {
      buffer.reset(new byte[buffer_bytes]);
      free_.push_back(buffer.get());
    }
    thread_ = std::thread([this] { loop(); });
  }
  ~Writer() {
    {
      std::lock_guard lock(mutex_);
      quit_ = true;
    }
    changed_.notify_all();
    thread_.join();
    if (fd_ >= 0) close(fd_);
  }
  // Takes the descriptor; the file is reported as `name` once synced.
  void begin(int fd, const std::string& name) {
    current_ = nullptr;
    {
      const uint64 started = now_ns();
      std::unique_lock lock(mutex_);
      changed_.wait(lock, [this] { return queued_files_ < max_queued_files; });
      ++queued_files_;
      wait_ns += now_ns() - started;
    }
    push({Command::begin, fd, name, nullptr, 0});
  }
  bool append(const byte* data, size_t size) {
    while (size) {
      if (!current_ && !(current_ = take())) return false;
      const size_t count = std::min(size, buffer_bytes - used_);
      memcpy(current_ + used_, data, count);
      used_ += count;
      data += count;
      size -= count;
      if (used_ == buffer_bytes) flush();
    }
    return error().empty();
  }
  void end(uint64 size) {
    if (current_) flush();
    push({Command::end, -1, {}, nullptr, size});
  }
  // Waits until every queued file is closed; the first error, if any.
  std::string finish() {
    std::unique_lock lock(mutex_);
    changed_.wait(lock, [this] { return queue_.empty() && !busy_; });
    return error_;
  }
  std::string error() {
    std::lock_guard lock(mutex_);
    return error_;
  }
  // Time in pwrite and fsync on the writer thread, and the decoder's waits for a buffer or a file slot.
  // Read once finish() returned.
  uint64 write_ns = 0, sync_ns = 0, wait_ns = 0;
  unsigned writes = 0, syncs = 0;

private:
  struct Command {
    enum Type { begin, data, end } type;
    int fd;
    std::string name;
    byte* buffer;
    uint64 size;
  };
  byte* take() {
    const uint64 started = now_ns();
    std::unique_lock lock(mutex_);
    changed_.wait(lock, [this] { return !free_.empty() || !error_.empty(); });
    wait_ns += now_ns() - started;
    if (!error_.empty()) return nullptr;
    byte* buffer = free_.front();
    free_.pop_front();
    used_ = 0;
    return buffer;
  }
  void flush() {
    push({Command::data, -1, {}, current_, used_});
    current_ = nullptr;
  }
  void push(Command command) {
    {
      std::lock_guard lock(mutex_);
      queue_.push_back(std::move(command));
    }
    changed_.notify_all();
  }
  void fail(const std::string& reason) {
    std::lock_guard lock(mutex_);
    if (error_.empty()) error_ = reason;
  }
  void run(Command& command) {
    if (command.type == Command::begin) {
      fd_ = command.fd;
      name_ = command.name;
      written_ = 0;
      hash_.init();
    } else if (command.type == Command::data) {
      const uint64 started = now_ns();
      size_t done = 0;
      while (done < command.size && error().empty()) {
        const ssize_t count = pwrite(fd_, command.buffer + done, command.size - done, written_ + done);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) fail(strerror(count < 0 ? errno : ENOSPC));
        else done += count;
      }
      write_ns += now_ns() - started;
      ++writes;
      hash_.update(command.buffer, command.size);
      written_ += command.size;
    } else {
      if (written_ != command.size) {
        fail("Extracted file size mismatch.");
      } else {
        const uint64 started = now_ns();
        if (fsync(fd_)) fail(strerror(errno));
        sync_ns += now_ns() - started;
        ++syncs;
      }
      const bool closed = !close(fd_);
      fd_ = -1;
      if (!closed) fail(strerror(errno));
      if (error().empty()) {
        byte digest[SHA256_DIGEST_SIZE];
        hash_.finish(digest);
        say("f " + hex(digest, sizeof digest) + " " + std::to_string(written_) + " " + name_);
      }
    }
  }
  void loop() {
    std::unique_lock lock(mutex_);
    while (true) {
      changed_.wait(lock, [this] { return quit_ || !queue_.empty(); });
      if (queue_.empty()) return;
      Command command = std::move(queue_.front());
      queue_.pop_front();
      queued_files_ -= command.type == Command::begin;
      busy_ = true;
      lock.unlock();
      // After an error, commands only give their buffers and descriptors back.
      if (error().empty()) run(command);
      else if (command.type == Command::begin) close(command.fd);
      lock.lock();
      busy_ = false;
      if (command.buffer) free_.push_back(command.buffer);
      changed_.notify_all();
    }
  }

  std::unique_ptr<byte[]> buffers_[buffer_count];
  std::deque<byte*> free_;
  std::deque<Command> queue_;
  std::mutex mutex_;
  std::condition_variable changed_;
  std::string error_;
  bool quit_ = false, busy_ = false;
  size_t queued_files_ = 0;
  std::thread thread_;
  byte* current_ = nullptr;
  size_t used_ = 0;
  // Writer thread only.
  int fd_ = -1;
  std::string name_;
  uint64 written_ = 0;
  Sha256 hash_;
};

struct Session {
  explicit Session(const Options& options) : options(options) {}
  const Options& options;
  Writer writer;
  size_t next_volume = 1;
  bool writing = false, missing_volume = false, missing_password = false;
  const Archive* archive = nullptr;
  uint64 written = 0, last_report = 0;
  std::string error;
};

// Progress, and a look for "cancel", a few times a second.
void heartbeat(Session& session) {
  const uint64 now = now_ns();
  if (now - session.last_report < progress_ns) return;
  session.last_report = now;
  say("p " + std::to_string(session.written));
  if (stop_requested()) cancelled = true;
}

// The app renames a volume to its final name once it is complete; until then, progress and a look for
// "cancel" go on as while decoding. Without hard links (the console, exFAT) the download reserves the final
// name with an empty file just before renaming the volume over it: an empty file is not the volume yet.
bool await_volume(Session& session, const std::string& path) {
  for (;;) {
    struct stat st;
    if (!lstat(path.c_str(), &st)) {
      if (!S_ISREG(st.st_mode)) return false;
      if (st.st_size) return true;
    } else if (errno != ENOENT) {
      return false;
    }
    heartbeat(session);
    if (cancelled) return false;
    usleep(250000);
  }
}

int CALLBACK callback(UINT message, LPARAM user, LPARAM p1, LPARAM p2) {
  Session& session = *reinterpret_cast<Session*>(user);
  switch (message) {
    case UCM_PROCESSDATA: {
      // Service data (NTFS streams and such) after a file is read and dropped.
      if (!session.writing) return 1;
      const size_t size = size_t(p2);
      if (size > session.options.limit - session.written) {
        session.error = "Archive exceeds the extraction size limit.";
        return -1;
      }
      session.written += size;
      if (!session.writer.append(reinterpret_cast<const byte*>(p1), size)) return -1;
      heartbeat(session);
      return cancelled ? -1 : 1;
    }
    case UCM_NEXTVOLUMEW: {
      // Volumes are exactly the engine's list, in order, whatever their names.
      if (session.next_volume >= session.options.sources.size()) {
        session.missing_volume = true;
        return -1;
      }
      const std::string& path = session.options.sources[session.next_volume++];
      if (session.options.wait && !await_volume(session, path)) {
        if (!cancelled) session.missing_volume = true;
        return -1;
      }
      UtfToWide(path.c_str(), reinterpret_cast<wchar*>(p1), size_t(p2));
      return 1;
    }
    case UCM_CHANGEVOLUME:
      return p2 == RAR_VOL_NOTIFY ? 1 : -1;
    case UCM_CHANGEVOLUMEW:
      // A RAR5 volume names its place in the set; one out of place means a volume between is missing.
      if (p2 == RAR_VOL_NOTIFY && session.archive->Format == RARFMT50 && session.archive->VolNumber &&
          session.archive->VolNumber != session.next_volume - 1)
        session.missing_volume = true;
      return p2 == RAR_VOL_NOTIFY && !session.missing_volume ? 1 : -1;
    case UCM_NEEDPASSWORD:
    case UCM_NEEDPASSWORDW:  // Asked only when none was given.
      session.missing_password = true;
      return -1;
    default:  // No dictionary beyond the limit.
      return -1;
  }
}

std::string describe(int code) {
  switch (code) {
    case ERAR_EOPEN: return "Incomplete RAR archive or missing volume.";
    case ERAR_BAD_DATA: return "RAR checksum mismatch; the archive is damaged.";
    case ERAR_EREAD: return "Cannot read the RAR archive; it may be truncated.";
    case ERAR_MISSING_PASSWORD: return "RAR archive password is missing.";
    case ERAR_BAD_PASSWORD: return "Incorrect RAR archive password.";
    case ERAR_NO_MEMORY:
    case ERAR_LARGE_DICT: return too_much_memory;
    case ERAR_BAD_ARCHIVE:
    case ERAR_UNKNOWN_FORMAT: return "Unsupported or damaged RAR archive.";
    default: return "RAR extraction failed.";
  }
}

int dll_error(CommandData& command) {
  if (command.DllError) return command.DllError;
  switch (ErrHandler.GetErrorCode()) {
    case RARX_SUCCESS: return ERAR_SUCCESS;
    case RARX_FATAL:
    case RARX_READ: return ERAR_EREAD;
    case RARX_CRC: return ERAR_BAD_DATA;
    case RARX_OPEN: return ERAR_EOPEN;
    case RARX_MEMORY: return ERAR_NO_MEMORY;
    case RARX_BADPWD: return ERAR_BAD_PASSWORD;
    case RARX_BADARC: return ERAR_BAD_ARCHIVE;
    default: return ERAR_UNKNOWN;
  }
}

long cores() { return sysconf(_SC_NPROCESSORS_ONLN); }

// The engine asks for fewer while downloads run. One core always stays with the app's interface; when the
// count is unknown, the cap alone applies.
unsigned pick_threads(unsigned requested) {
  const long known = cores();
  if (!requested) return unsigned(std::clamp(known - 2, 1L, 4L));
  return unsigned(std::min<long>(requested, known > 0 ? std::clamp(known - 1, 1L, 6L) : 6));
}

uint64 process_cpu_us() {
  rusage usage;
  if (getrusage(RUSAGE_SELF, &usage)) return 0;
  return (uint64(usage.ru_utime.tv_sec) + usage.ru_stime.tv_sec) * 1000000 + usage.ru_utime.tv_usec + usage.ru_stime.tv_usec;
}

bool flattened(const Options& options, const std::string& name) {
  for (const auto& extension : options.flatten)
    if (name.size() >= extension.size() && !name.compare(name.size() - extension.size(), extension.size(), extension)) return true;
  return false;
}

// Throws RAR_EXIT or std::bad_alloc from UnRAR; returns the reason extraction stopped, empty on success.
std::string extract(Session& session, CommandData& command, Archive& archive, CmdExtract& extractor) {
  const Options& options = session.options;
  uint64 declared = 0;
  while (true) {
    heartbeat(session);
    if (cancelled) return {};
    const size_t header_size = archive.SearchBlock(HEAD_FILE);
    if (header_size == 0) {
      if (archive.Volume && archive.GetHeaderType() == HEAD_ENDARC && archive.EndArcHead.NextVolume) {
        if (!MergeArchive(archive, nullptr, false, 'L')) return describe(ERAR_EOPEN);
        archive.Seek(archive.CurBlockPos, SEEK_SET);
        continue;
      }
      if (archive.BrokenHeader) return describe(ERAR_BAD_DATA);
      if (archive.FailedHeaderDecryption) return describe(ERAR_BAD_PASSWORD);
      if (const int code = dll_error(command)) return describe(code);
      return {};
    }
    const FileHeader& header = archive.FileHead;
    if (header.SplitBefore) return describe(ERAR_EOPEN);
    std::string name;
    WideToUtf(header.FileName, name);
    while (!name.empty() && name.back() == '/') name.pop_back();
    if (!name.empty() && name[0] != '/') name = without_dots(name);
    if (!safe(name) || header.RedirType != FSREDIR_NONE || (!options.reserved.empty() && name.find(options.reserved) != std::string::npos))
      return "Archive contains an unsafe path, link or special file.";
    if (header.Method != 0 && header.WinSize > options.window) return too_much_memory;
    if (!header.Dir) {
      if (header.UnpSize < 0 || uint64(header.UnpSize) > options.limit - declared) return "Archive exceeds the extraction size limit.";
      declared += header.UnpSize;
    }
    // ShadowMount's default scan reaches one subdirectory, so images are published at the set root.
    if (!header.Dir && flattened(options, name)) name = name.substr(name.rfind('/') + 1);
    std::string target;
    if (!make_parents(options.destination, name, target)) return strerror(errno);
    if (header.Dir) {
      if (mkdir(target.c_str(), 0777) && (errno != EEXIST || !plain_directory(target))) return strerror(errno);
    } else {
      const int fd = open(target.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0666);
      if (fd < 0) return strerror(errno);
      session.writer.begin(fd, name);
      session.writing = true;
    }
    // Reading the service headers below may load the next file's header.
    const uint64 size = uint64(header.UnpSize);
    const bool directory = header.Dir;
    command.DllError = 0;
    command.DllOpMode = RAR_TEST;
    command.Command = L"T";
    command.Test = true;
    bool repeat = false;
    extractor.ExtractCurrentFile(archive, header_size, repeat);
    session.writing = false;
    // As unrar.dll does: the file's service headers are handled in the same step.
    while (archive.IsOpened() && archive.ReadHeader() != 0 && archive.GetHeaderType() == HEAD_SERVICE) {
      extractor.ExtractCurrentFile(archive, header_size, repeat);
      archive.SeekToNext();
    }
    archive.Seek(archive.CurBlockPos, SEEK_SET);
    if (!session.error.empty()) return session.error;
    if (const auto error = session.writer.error(); !error.empty()) return error;
    if (cancelled) return {};
    if (const int code = dll_error(command)) return describe(code);
    if (!directory) session.writer.end(size);
  }
}

std::string run(Session& session) {
  const Options& options = session.options;
  CommandData command;
  command.FileArgs.AddString(L"*");
  command.Overwrite = OVERWRITE_ALL;
  command.VersionControl = 1;
  command.Callback = callback;
  command.UserData = LPARAM(&session);
  command.Threads = pick_threads(options.threads);
  command.WinSizeLimit = options.window;
  if (!options.password.empty()) {
    std::wstring password;
    UtfToWide(options.password.c_str(), password);
    command.Password.Set(password.c_str());
  }
  std::wstring name;
  UtfToWide(options.sources.front().c_str(), name);
  command.AddArcName(name);
  Archive archive(&command);
  session.archive = &archive;
  if (!archive.Open(name)) return describe(ERAR_EOPEN);
  if (!archive.IsArchive(true)) return describe(command.DllError ? command.DllError : dll_error(command) ? dll_error(command) : ERAR_BAD_ARCHIVE);
  CmdExtract extractor(&command);
  extractor.ExtractArchiveInit(archive);
  return extract(session, command, archive, extractor);
}
}

int main() {
  signal(SIGPIPE, SIG_IGN);
  // Directories and files stay usable by the app, which may run as another user than this program.
  umask(0);
  Options options;
  if (!read_request(options)) {
    say("fail Invalid RAR worker request.");
    return 1;
  }
  say("ready");
  // Progress comes from the decoder's callback only, so one long fsync on a slow drive would look like a
  // dead worker to the engine. "k" every second says this process is still alive whatever it is doing.
  std::mutex alive_mutex;
  std::condition_variable alive_wake;
  bool done = false;
  std::thread alive;
  try {
    alive = std::thread([&] {
      std::unique_lock lock(alive_mutex);
      while (!alive_wake.wait_for(lock, std::chrono::seconds(1), [&] { return done; })) say("k");
    });
  } catch (...) {
  }
  std::string error;
  {
    Session session{options};
    try {
      ErrHandler.Clean();
      error = run(session);
    } catch (RAR_EXIT code) {
      error = !session.error.empty() ? session.error : session.writer.error();
      if (error.empty() && !cancelled) error = code == RARX_MEMORY ? too_much_memory : describe(ERAR_UNKNOWN);
    } catch (std::bad_alloc&) {
      error = too_much_memory;
    } catch (...) {
      error = describe(ERAR_UNKNOWN);
    }
    const auto written = session.writer.finish();
    if (error.empty()) error = written;
    const Writer& w = session.writer;
    const auto ms = [](uint64 ns) { return std::to_string(ns / 1000000) + "ms"; };
    std::string timings = "t threads=" + std::to_string(pick_threads(options.threads)) + " cores=" + std::to_string(cores())
        + " write=" + ms(w.write_ns) + "/" + std::to_string(w.writes) + " fsync=" + ms(w.sync_ns) + "/" + std::to_string(w.syncs)
        + " waited=" + ms(w.wait_ns);
    if (const uint64 cpu = process_cpu_us()) timings += " cpu=" + ms(cpu * 1000);
    say(timings);
    // The precise cause, rather than the checksum or header failure it leads to.
    if (!error.empty() && session.error.empty() && !cancelled) {
      if (session.missing_password) error = describe(ERAR_MISSING_PASSWORD);
      else if (session.missing_volume) error = describe(ERAR_EOPEN);
    }
  }
  {
    std::lock_guard lock(alive_mutex);
    done = true;
  }
  alive_wake.notify_all();
  if (alive.joinable()) alive.join();
  if (cancelled) say("cancelled");
  else say(error.empty() ? "ok" : "fail " + error);
  return error.empty() ? 0 : 1;
}
