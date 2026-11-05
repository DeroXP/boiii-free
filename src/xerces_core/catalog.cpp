#include "catalog.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <exception>
#include <fstream>
#include <iterator>
#include <memory>
#include <numeric>
#include <sstream>
#include <thread>

#include "file_hash.hpp"
#include "manifest.hpp"
#include "sha256.hpp"

namespace xerces_core {
namespace {
std::string read_text(const std::filesystem::path &path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::string utf8(const std::u8string &text) {
  return {reinterpret_cast<const char *>(text.data()), text.size()};
}

std::string relative_name(const std::filesystem::path &root,
                          const std::filesystem::path &file) {
  return utf8(std::filesystem::relative(file, root).generic_u8string());
}

// Calls visit(entry) for every regular file under folder, in directory
// order. Junctions and folder links are not entered.
template <typename Visit>
void for_each_file_entry(const std::filesystem::path &folder, Visit &&visit) {
  std::error_code ec;
  for (auto it = std::filesystem::recursive_directory_iterator(
           folder, std::filesystem::directory_options::skip_permission_denied,
           ec);
       !ec && it != std::filesystem::recursive_directory_iterator();
       it.increment(ec)) {
    if (it->is_regular_file(ec)) {
      visit(*it);
    }
  }
}

// Calls visit(path) for every regular file under folder, in directory order.
template <typename Visit>
void for_each_file(const std::filesystem::path &folder, Visit &&visit) {
  for_each_file_entry(folder,
                      [&](const std::filesystem::directory_entry &entry) {
                        visit(entry.path());
                      });
}

bool has_file(const std::vector<std::string> &files, std::string_view name) {
  return std::ranges::find(files, name) != files.end();
}
} // namespace

std::vector<std::pair<std::string, std::string>>
read_files(const std::filesystem::path &folder) {
  std::vector<std::pair<std::string, std::string>> files;
  for_each_file(folder, [&](const std::filesystem::path &file) {
    files.emplace_back(relative_name(folder, file), read_text(file));
  });
  std::ranges::sort(files);
  return files;
}

std::vector<std::string> list_files(const std::filesystem::path &folder) {
  std::vector<std::string> names;
  for_each_file(folder, [&](const std::filesystem::path &file) {
    names.push_back(relative_name(folder, file));
  });
  std::ranges::sort(names);
  return names;
}

std::optional<std::string>
try_file_sha256_hex(const std::filesystem::path &path) {
  std::error_code ec;
  const std::uint64_t size = std::filesystem::file_size(path, ec);
  if (ec) {
    return std::nullopt;
  }
  return sha256_file_of_size(path, size);
}

std::string file_sha256_hex(const std::filesystem::path &path) {
  return try_file_sha256_hex(path).value_or(std::string());
}

namespace {
// The key of a path already in UTF-8 text. The generic form of a Windows
// path is its text with '\' turned into '/' (what generic_u8string does), so
// a cache line's path gives its key without becoming a path: building one
// from text that is not valid UTF-8 throws.
std::string key_of_text(std::string key) {
  for (char &c : key) {
    if (c == '\\') {
      c = '/';
    } else if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  return key;
}

// Valid UTF-8 (no overlong forms, surrogates or values past U+10FFFF) and
// no control characters, which no Windows path holds: a cache line cut
// inside a character, or ending in the zeros of a file cut short, fails.
bool path_text_ok(std::string_view text) {
  std::size_t i = 0;
  while (i < text.size()) {
    const auto lead = static_cast<unsigned char>(text[i]);
    if (lead < 0x20) {
      return false;
    }
    if (lead < 0x80) {
      ++i;
      continue;
    }
    std::size_t length = 0;
    std::uint32_t value = 0, min_value = 0;
    if (lead >= 0xC2 && lead <= 0xDF) {
      length = 2, value = lead & 0x1Fu, min_value = 0x80;
    } else if (lead >= 0xE0 && lead <= 0xEF) {
      length = 3, value = lead & 0x0Fu, min_value = 0x800;
    } else if (lead >= 0xF0 && lead <= 0xF4) {
      length = 4, value = lead & 0x07u, min_value = 0x10000;
    } else {
      return false;
    }
    if (i + length > text.size()) {
      return false;
    }
    for (std::size_t k = 1; k < length; ++k) {
      const auto next = static_cast<unsigned char>(text[i + k]);
      if ((next & 0xC0) != 0x80) {
        return false;
      }
      value = (value << 6) | (next & 0x3Fu);
    }
    if (value < min_value || value > 0x10FFFF ||
        (value >= 0xD800 && value <= 0xDFFF)) {
      return false;
    }
    i += length;
  }
  return true;
}

bool lower_hex(std::string_view text) {
  return std::ranges::all_of(text, [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
  });
}
} // namespace

std::string hash_cache_key(const std::filesystem::path &path) {
  return key_of_text(utf8(path.generic_u8string()));
}

int hash_cache::read_file(const std::filesystem::path &file, entry_map &into) {
  std::ifstream in(file, std::ios::binary);
  std::string line;
  if (!in || !std::getline(in, line)) {
    return 0;
  }
  const int version = line == hash_cache_header      ? 2
                      : line == hash_cache_header_v1 ? 1
                                                     : 0;
  if (version == 0) {
    return 0;
  }
  // What a failed read of a file used to give. No file with bytes in it has
  // this hash, so such an entry is a failure written down, never a hash.
  static const std::string nothing_read = sha256::hex("");
  while (std::getline(in, line)) {
    // size, write time, sha256 and path, tab-separated; Windows paths hold no
    // tabs.
    const std::size_t a = line.find('\t');
    const std::size_t b = a == std::string::npos ? a : line.find('\t', a + 1);
    const std::size_t c = b == std::string::npos ? b : line.find('\t', b + 1);
    if (c == std::string::npos || c - b - 1 != 64 || c + 1 >= line.size()) {
      continue;
    }
    entry value;
    const char *begin = line.data();
    if (std::from_chars(begin, begin + a, value.size).ec != std::errc{} ||
        std::from_chars(begin + a + 1, begin + b, value.write_time).ec !=
            std::errc{}) {
      continue;
    }
    value.sha256 = line.substr(b + 1, 64);
    if (!lower_hex(value.sha256) ||
        (value.size != 0 && value.sha256 == nothing_read)) {
      continue;
    }
    value.path = line.substr(c + 1);
    if (!path_text_ok(value.path)) {
      continue; // cut short: it names no file
    }
    // A path written twice (two spellings of one file) keeps its last line.
    std::string key = key_of_text(value.path);
    into[std::move(key)] = std::move(value);
  }
  return version;
}

bool hash_cache::load(const std::filesystem::path &file) {
  entry_map loaded;
  const int version = read_file(file, loaded);
  std::lock_guard lock(mutex_);
  entries_ = std::move(loaded);
  loaded_version_ = version;
  return version != 0;
}

bool hash_cache::load_seed(const std::filesystem::path &file) {
  entry_map loaded;
  const int version = read_file(file, loaded);
  std::lock_guard lock(mutex_);
  seed_ = std::move(loaded);
  return version != 0;
}

bool hash_cache::write_file(
    const std::filesystem::path &file,
    const std::vector<std::string> &pruned_prefixes) const {
  std::error_code ec;
  std::filesystem::create_directories(file.parent_path(), ec);
  std::filesystem::path temporary = file;
  temporary += ".tmp";
  {
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    if (!out) {
      return false;
    }
    out << hash_cache_header << '\n';
    std::lock_guard lock(mutex_);
    for (const auto &[key, value] : entries_) {
      if (!value.used &&
          std::ranges::any_of(pruned_prefixes, [&](const std::string &prefix) {
            return key.starts_with(prefix);
          })) {
        continue;
      }
      out << value.size << '\t' << value.write_time << '\t' << value.sha256
          << '\t' << value.path << '\n';
    }
    if (!out.flush()) {
      return false;
    }
  }
  std::filesystem::rename(temporary, file, ec);
  return !ec;
}

bool hash_cache::save(const std::filesystem::path &file) const {
  return write_file(file, {});
}

bool hash_cache::save(
    const std::filesystem::path &file,
    const std::vector<std::filesystem::path> &pruned_roots) const {
  std::vector<std::string> prefixes;
  for (const auto &root : pruned_roots) {
    std::error_code ec;
    const std::filesystem::path absolute = std::filesystem::absolute(root, ec);
    std::string prefix = hash_cache_key(ec ? root : absolute);
    while (!prefix.empty() && prefix.back() == '/') {
      prefix.pop_back();
    }
    if (prefix.empty()) {
      continue; // never "everything"
    }
    prefixes.push_back(prefix + '/');
  }
  return write_file(file, prefixes);
}

std::optional<std::string>
hash_cache::file_sha256(const std::filesystem::path &path,
                        const std::atomic<bool> *cancel, stats *sink) {
  const std::string shown = utf8(path.generic_u8string());
  const std::string key = key_of_text(shown);
  // Counts in the cache's stats and the caller's; call with mutex_ held.
  const auto count = [&](auto &&update) {
    update(stats_);
    if (sink != nullptr) {
      update(*sink);
    }
  };
  std::error_code ec;
  const std::uint64_t size = std::filesystem::file_size(path, ec);
  const auto time = ec ? std::filesystem::file_time_type{}
                       : std::filesystem::last_write_time(path, ec);
  if (ec) {
    std::lock_guard lock(mutex_);
    entries_.erase(key);
    return std::nullopt;
  }
  const std::int64_t write_time = time.time_since_epoch().count();
  const auto matches = [&](const entry &value) {
    return !value.sha256.empty() && value.size == size &&
           value.write_time == write_time;
  };
  {
    std::lock_guard lock(mutex_);
    if (const auto found = entries_.find(key);
        found != entries_.end() && matches(found->second)) {
      count([](stats &s) { ++s.hits; });
      found->second.used = true;
      return found->second.sha256;
    }
    if (const auto seeded = seed_.find(key);
        seeded != seed_.end() && matches(seeded->second)) {
      count([](stats &s) {
        ++s.hits;
        ++s.seed_hits;
      });
      entry &value = entries_[key];
      value = seeded->second;
      value.used = true;
      return value.sha256;
    }
    count([](stats &s) { ++s.misses; });
  }
  // Read outside the lock: the other threads go on looking up and hashing.
  const auto started = std::chrono::steady_clock::now();
  const std::optional<std::string> digest =
      sha256_file_of_size(path, size, cancel);
  const double seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - started)
          .count();
  std::lock_guard lock(mutex_);
  if (!digest) {
    // Never remembered: a file that is unreadable now (Steam holding a
    // Workshop file while it updates it) would otherwise keep a wrong hash
    // across scans and restarts until the file changed again. Any older
    // entry is stale anyway, since the size or time no longer match it.
    entries_.erase(key);
    return std::nullopt;
  }
  // Only bytes that were read in full and hashed: with a failed or
  // cancelled read counted, the MB/s and "Hashed" figures would include
  // bytes never read.
  count([&](stats &s) {
    s.bytes_hashed += size;
    s.hash_seconds += seconds;
    if (seconds > s.slowest_seconds) {
      s.slowest_seconds = seconds;
      s.slowest_bytes = size;
      s.slowest_file = shown;
    }
  });
  entry &value = entries_[key];
  value.size = size;
  value.write_time = write_time;
  value.sha256 = *digest;
  value.path = shown;
  value.used = true;
  return digest;
}

std::size_t hash_cache::hits() const {
  std::lock_guard lock(mutex_);
  return stats_.hits;
}

std::size_t hash_cache::misses() const {
  std::lock_guard lock(mutex_);
  return stats_.misses;
}

std::size_t hash_cache::size() const {
  std::lock_guard lock(mutex_);
  return entries_.size();
}

std::uint64_t hash_cache::bytes_hashed() const {
  std::lock_guard lock(mutex_);
  return stats_.bytes_hashed;
}

std::size_t hash_cache::seed_size() const {
  std::lock_guard lock(mutex_);
  return seed_.size();
}

int hash_cache::loaded_version() const {
  std::lock_guard lock(mutex_);
  return loaded_version_;
}

hash_cache::stats hash_cache::get_stats() const {
  std::lock_guard lock(mutex_);
  return stats_;
}

namespace {
// One file of a scan, hashed on whichever thread takes it.
struct file_job {
  std::filesystem::path path;
  std::string name;            // relative '/' path in its mod folder
  std::uint64_t size_hint = 0; // from the folder listing; only orders the work
  std::size_t folder = 0;      // index of its mod folder
  std::optional<std::string> digest;
  bool done = false; // read (or found unreadable); false if cancel stopped it
};

// Adds every regular file under base (a mod folder, absolute: the cache is
// keyed by absolute paths) to jobs.
void list_jobs(const std::filesystem::path &base, std::size_t folder,
               std::vector<file_job> &jobs) {
  for_each_file_entry(base, [&](const std::filesystem::directory_entry &entry) {
    file_job job;
    job.path = entry.path();
    job.name = relative_name(base, job.path);
    std::error_code ec;
    job.size_hint = entry.file_size(ec);
    job.folder = folder;
    jobs.push_back(std::move(job));
  });
}

std::optional<std::string> hash_job(const file_job &job, hash_cache *cache,
                                    const std::atomic<bool> *cancel,
                                    hash_cache::stats *stats) {
  if (cache != nullptr) {
    return cache->file_sha256(job.path, cancel, stats);
  }
  std::error_code ec;
  const std::uint64_t size = std::filesystem::file_size(job.path, ec);
  if (ec) {
    return std::nullopt;
  }
  return sha256_file_of_size(job.path, size, cancel);
}

// Hashes every job, `threads` files at once, the largest first: the biggest
// file is the longest single read, so it starts at once instead of last.
// finished(job) is called after each file that was read, from the thread
// that read it. Once cancel is set, the files not yet read stay !done.
// Lookups through cache also count in stats when it is given.
void hash_jobs(std::vector<file_job> &jobs, hash_cache *cache, unsigned threads,
               const std::atomic<bool> *cancel,
               const std::function<void(const file_job &)> &finished,
               hash_cache::stats *stats = nullptr) {
  std::vector<std::size_t> order(jobs.size());
  std::iota(order.begin(), order.end(), std::size_t{0});
  std::ranges::stable_sort(order, [&](std::size_t a, std::size_t b) {
    return jobs[a].size_hint > jobs[b].size_hint;
  });
  std::atomic<std::size_t> next{0};
  std::mutex failure_mutex;
  std::exception_ptr failure;
  const auto work = [&] {
    try {
      for (std::size_t at = next++; at < order.size(); at = next++) {
        if (cancel != nullptr && cancel->load()) {
          return;
        }
        file_job &job = jobs[order[at]];
        job.digest = hash_job(job, cache, cancel, stats);
        if (cancel != nullptr && cancel->load()) {
          job.digest.reset(); // it may have been cut short
          return;
        }
        job.done = true;
        if (finished) {
          finished(job);
        }
      }
    } catch (...) {
      std::lock_guard lock(failure_mutex);
      if (!failure) {
        failure = std::current_exception();
      }
      next = order.size(); // the others stop before their next file
    }
  };
  const std::size_t count = std::clamp<std::size_t>(
      threads, 1, std::max<std::size_t>(1, jobs.size()));
  std::vector<std::thread> pool;
  pool.reserve(count - 1);
  for (std::size_t i = 1; i < count; ++i) {
    pool.emplace_back(work);
  }
  work();
  for (std::thread &thread : pool) {
    thread.join();
  }
  if (failure) {
    std::rethrow_exception(failure);
  }
}

// One folder's jobs as hash_files returns them.
std::vector<std::pair<std::string, std::string>>
digests_of(const std::vector<const file_job *> &jobs,
           std::vector<std::string> *unreadable) {
  std::vector<std::pair<std::string, std::string>> digests;
  digests.reserve(jobs.size());
  for (const file_job *job : jobs) {
    if (!job->digest && unreadable) {
      unreadable->push_back(job->name);
    }
    digests.emplace_back(job->name, job->digest.value_or(std::string()));
  }
  std::ranges::sort(digests);
  if (unreadable) {
    std::ranges::sort(*unreadable);
  }
  return digests;
}

std::vector<const file_job *> pointers(const std::vector<file_job> &jobs) {
  std::vector<const file_job *> out;
  out.reserve(jobs.size());
  for (const file_job &job : jobs) {
    out.push_back(&job);
  }
  return out;
}
} // namespace

std::vector<std::pair<std::string, std::string>>
hash_files(const std::filesystem::path &folder, hash_cache *cache,
           std::vector<std::string> *unreadable) {
  std::vector<file_job> jobs;
  list_jobs(folder, 0, jobs);
  hash_jobs(jobs, cache, 1, nullptr, {});
  return digests_of(pointers(jobs), unreadable);
}

std::string content_hash_of_digests(
    const std::vector<std::pair<std::string, std::string>> &digests) {
  sha256 hasher;
  for (const auto &[path, digest] : digests) {
    const std::string name = normalize_asset_name(path);
    if (name == "workshop.json" || name == "zone/workshop.json") {
      continue;
    }
    hasher.update(name);
    hasher.update(std::string_view("\0", 1));
    hasher.update(digest);
    hasher.update(std::string_view("\n", 1));
  }
  return "sha256:" + sha256::to_hex(hasher.finish());
}

std::string
content_hash_of(const std::vector<std::pair<std::string, std::string>> &files) {
  std::vector<std::pair<std::string, std::string>> digests;
  digests.reserve(files.size());
  for (const auto &[path, contents] : files) {
    digests.emplace_back(path, sha256::hex(contents));
  }
  return content_hash_of_digests(digests);
}

namespace {
// Where a mod folder's files are listed and hashed from: its absolute path,
// as the cache is keyed by absolute paths.
std::filesystem::path base_of(const std::filesystem::path &folder) {
  std::error_code ec;
  const std::filesystem::path absolute = std::filesystem::absolute(folder, ec);
  return ec || absolute.empty() ? folder : absolute;
}

// The manifest (or a legacy one) from the files, hashed already. Hashing is
// file by file: Workshop maps reach several gigabytes, and only the manifest
// and workshop.json are needed as text.
installed_content
load_content(const std::filesystem::path &folder,
             const std::vector<std::pair<std::string, std::string>> &digests,
             const std::vector<std::string> &unreadable,
             const std::set<std::string> &taken_ids, diagnostics &diags) {
  installed_content content;
  content.folder = utf8(folder.filename().u8string());
  std::error_code ec;
  content.path = std::filesystem::absolute(folder, ec);
  for (const auto &[path, digest] : digests) {
    content.files.push_back(path);
  }
  content.content_hash = content_hash_of_digests(digests);
  // The content hash then covers an unknown file, so plans and pins built
  // on it are wrong until a rescan reads the file; nothing is cached for it.
  for (const std::string &name : unreadable) {
    diags.warning("catalog.file.unreadable", content.folder + "/" + name,
                  content.folder + ": " + name +
                      " could not be read in full (another program may "
                      "hold it open), so this mod's content hash is not "
                      "its real one. Close that program and scan again.");
  }

  if (has_file(content.files, manifest_file_name)) {
    manifest_result parsed =
        parse_manifest(read_text(folder / std::string(manifest_file_name)));
    if (parsed.value.legacy) {
      // Only manifests Xerces synthesizes are legacy; a mod cannot opt out
      // of rule 3 by claiming it.
      parsed.diags.warning("manifest.legacy.ignored", "/legacy",
                           "\"legacy\" is for synthesized manifests; ignored.");
      parsed.value.legacy = false;
    }
    for (const auto &item : parsed.diags.items()) {
      diags.add(item.level, item.code, content.folder + item.path,
                content.folder + ": " + item.message);
    }
    content.valid_manifest = parsed.ok();
    content.info = std::move(parsed.value);
    return content;
  }

  legacy_input input;
  input.folder = content.folder;
  input.content_hash = content.content_hash;
  input.files = content.files;
  for (const char *name : {"workshop.json", "zone/workshop.json"}) {
    if (has_file(content.files, name)) {
      input.workshop_json = read_text(folder / name);
      break;
    }
  }
  content.info = synthesize_legacy(input, taken_ids, &diags);
  return content;
}

// The file's bytes, when exactly `size` of them could be read: the file could
// be opened, did not end early and did not grow meanwhile.
std::optional<std::string> read_text_of_size(const std::filesystem::path &path,
                                             std::uintmax_t size) {
  std::ifstream in(path, std::ios::binary);
  if (!in.is_open()) {
    return std::nullopt;
  }
  std::string text(static_cast<std::size_t>(size), '\0');
  in.read(text.data(), static_cast<std::streamsize>(size));
  if (static_cast<std::uintmax_t>(in.gcount()) != size ||
      in.peek() != std::char_traits<char>::eof()) {
    return std::nullopt;
  }
  return text;
}

// The pool file the client reads when it loads this mod or map
// (UGC_LoadItem_PrepareAssetPool: UGC_LoadModByPublisherId_Impl calls it for
// the main mod and each stacked one, UGC_LoadUsermapByPublisherId_Impl for a
// custom map). It is small and read whole; its hash is in the content hash
// like every other file's. The lookup is the client's own walk from its
// search root, never a look at content.files: hash_files does not enter a
// junction or a folder link, but the client's walk starts inside zone\ when
// zone\ is one.
void read_pool_file(installed_content &content,
                    const std::filesystem::path &folder, content_layout layout,
                    diagnostics &diags) {
  const std::filesystem::path base =
      content.path.empty() ? folder : content.path;
  const auto file = find_pool_file(pool_search_root(base, layout));
  if (!file) {
    return;
  }
  // Lexically: the walk built the path under base, and std::filesystem::
  // relative would resolve a zone\ junction to its target first.
  content.pool_file = utf8(file->lexically_relative(base).generic_u8string());
  const std::string where = content.folder + "/" + content.pool_file;
  std::error_code ec;
  const std::uintmax_t size = std::filesystem::file_size(*file, ec);
  if (!ec && size > max_pool_file_bytes) {
    content.pool_file_unread = true;
    diags.warning("catalog.pools.too_big", where,
                  content.folder + ": " + content.pool_file + " holds " +
                      std::to_string(size) +
                      " bytes, more than a pool file needs, so Xerces did "
                      "not read it and does not know which pools it raises; "
                      "the game reads it whole.");
    return;
  }
  std::optional<std::string> text =
      ec ? std::nullopt : read_text_of_size(*file, size);
  if (!text) {
    content.pool_file_unread = true;
    diags.warning("catalog.pools.unreadable", where,
                  content.folder + ": " + content.pool_file +
                      " could not be read (another program may hold it "
                      "open), so Xerces does not know which pools it "
                      "raises. Close that program and scan again.");
    return;
  }
  content.pool_limits = parse_pool_limits(std::move(*text));
}
} // namespace

installed_content load_mod_folder(const std::filesystem::path &folder,
                                  const std::set<std::string> &taken_ids,
                                  diagnostics &diags, hash_cache *cache,
                                  content_layout layout) {
  std::vector<std::string> unreadable;
  const auto digests = hash_files(base_of(folder), cache, &unreadable);
  installed_content content =
      load_content(folder, digests, unreadable, taken_ids, diags);
  // Mods and maps alike: the client reads a map's file when the map loads.
  read_pool_file(content, folder, layout, diags);
  return content;
}

content_layout layout_of_directory(const std::filesystem::path &directory) {
  // Steam keeps the game's Workshop items in workshop\content\311210.
  const std::filesystem::path name = directory.filename().empty()
                                         ? directory.parent_path().filename()
                                         : directory.filename();
  return name == "311210" ? content_layout::workshop
                          : content_layout::game_folder;
}

catalog_scan scan_mods_directory(const std::filesystem::path &directory,
                                 const scan_options &options) {
  catalog_scan scan;
  std::error_code ec;
  std::vector<std::filesystem::path> folders;
  for (const auto &entry : std::filesystem::directory_iterator(directory, ec)) {
    if (entry.is_directory(ec)) {
      folders.push_back(entry.path());
    }
  }
  if (ec) {
    scan.diags.error("catalog.read", utf8(directory.u8string()),
                     "Cannot read the folder: " + ec.message());
    return scan;
  }
  scan.listed = true;
  std::ranges::sort(folders);

  // Every file of every folder first, so the files can be hashed several at
  // a time across folders, the largest first; then the folders are read in
  // order from the hashes, as one at a time would.
  std::vector<file_job> jobs;
  std::vector<std::size_t> first(folders.size() + 1, 0);
  for (std::size_t index = 0; index < folders.size(); ++index) {
    first[index] = jobs.size();
    list_jobs(base_of(folders[index]), index, jobs);
  }
  first[folders.size()] = jobs.size();

  std::unique_ptr<std::atomic<std::size_t>[]> left(
      new std::atomic<std::size_t>[folders.size()]);
  std::size_t empty = 0;
  for (std::size_t index = 0; index < folders.size(); ++index) {
    left[index] = first[index + 1] - first[index];
    empty += left[index] == 0 ? 1 : 0;
  }
  std::mutex progress_mutex;
  std::size_t finished = empty; // folders with every file read
  const auto folder_done = [&](const file_job &job) {
    if (--left[job.folder] != 0 || !options.progress) {
      return;
    }
    std::lock_guard lock(progress_mutex);
    options.progress(finished++, folders.size(),
                     utf8(folders[job.folder].filename().u8string()));
  };
  const unsigned threads = options.hash_threads != 0
                               ? options.hash_threads
                               : default_hash_threads(directory);
  hash_jobs(jobs, options.cache, threads, options.cancel, folder_done,
            options.stats);

  std::set<std::string> taken;
  for (std::size_t index = 0; index < folders.size(); ++index) {
    const std::filesystem::path &folder = folders[index];
    std::vector<const file_job *> mine;
    for (std::size_t at = first[index]; at < first[index + 1]; ++at) {
      mine.push_back(&jobs[at]);
    }
    if (std::ranges::any_of(mine,
                            [](const file_job *job) { return !job->done; })) {
      scan.cancelled = true; // cancel stopped the hashing before this folder
      break;
    }
    std::vector<std::string> unreadable;
    const auto digests = digests_of(mine, &unreadable);
    installed_content content =
        load_content(folder, digests, unreadable, taken, scan.diags);
    read_pool_file(content, folder, options.layout, scan.diags);
    if (content.info.id.empty()) {
      continue;
    }
    // Two versions of one id may sit side by side; the planner picks.
    taken.insert(content.info.id);
    scan.roots[content.info.id] = folder;
    scan.catalog.push_back(std::move(content));
  }
  return scan;
}
} // namespace xerces_core
