#include "catalog.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <fstream>
#include <iterator>
#include <sstream>

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

// Calls visit(path) for every regular file under folder, in directory order.
template <typename Visit>
void for_each_file(const std::filesystem::path &folder, Visit &&visit) {
  std::error_code ec;
  for (auto it = std::filesystem::recursive_directory_iterator(
           folder, std::filesystem::directory_options::skip_permission_denied,
           ec);
       !ec && it != std::filesystem::recursive_directory_iterator();
       it.increment(ec)) {
    if (it->is_regular_file(ec)) {
      visit(it->path());
    }
  }
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

namespace {
// The hash of exactly `size` bytes of the file, or nothing. Reading fewer or
// more bytes than the size taken before the read means the file could not be
// read or changed meanwhile; either way the result is not that file's hash.
std::optional<std::string> hash_file_of_size(const std::filesystem::path &path,
                                             std::uint64_t size) {
  std::ifstream in(path, std::ios::binary);
  if (!in.is_open()) {
    return std::nullopt; // gone, or held open by a program that shares no reads
  }
  sha256 hasher;
  std::vector<char> buffer(std::size_t{1} << 20);
  std::uint64_t total = 0;
  while (in) {
    in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const std::streamsize got = in.gcount();
    if (got > 0) {
      hasher.update(buffer.data(), static_cast<std::size_t>(got));
      total += static_cast<std::uint64_t>(got);
    }
  }
  if (in.bad() || total != size) {
    return std::nullopt;
  }
  return sha256::to_hex(hasher.finish());
}
} // namespace

std::optional<std::string>
try_file_sha256_hex(const std::filesystem::path &path) {
  std::error_code ec;
  const std::uint64_t size = std::filesystem::file_size(path, ec);
  if (ec) {
    return std::nullopt;
  }
  return hash_file_of_size(path, size);
}

std::string file_sha256_hex(const std::filesystem::path &path) {
  return try_file_sha256_hex(path).value_or(std::string());
}

bool hash_cache::load(const std::filesystem::path &file) {
  entries_.clear();
  std::ifstream in(file, std::ios::binary);
  std::string line;
  if (!in || !std::getline(in, line) || line != hash_cache_header) {
    return false;
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
    if (value.size != 0 && value.sha256 == nothing_read) {
      continue;
    }
    entries_[line.substr(c + 1)] = std::move(value);
  }
  return true;
}

bool hash_cache::save(const std::filesystem::path &file, bool prune) const {
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
    for (const auto &[path, value] : entries_) {
      if (prune && !value.used) {
        continue;
      }
      out << value.size << '\t' << value.write_time << '\t' << value.sha256
          << '\t' << path << '\n';
    }
    if (!out.flush()) {
      return false;
    }
  }
  std::filesystem::rename(temporary, file, ec);
  return !ec;
}

std::optional<std::string>
hash_cache::file_sha256(const std::filesystem::path &path) {
  const std::string key = utf8(path.generic_u8string());
  std::error_code ec;
  const std::uint64_t size = std::filesystem::file_size(path, ec);
  if (ec) {
    entries_.erase(key);
    return std::nullopt;
  }
  const auto time = std::filesystem::last_write_time(path, ec);
  if (ec) {
    entries_.erase(key);
    return std::nullopt;
  }
  const std::int64_t write_time = time.time_since_epoch().count();
  const auto found = entries_.find(key);
  if (found != entries_.end() && !found->second.sha256.empty() &&
      found->second.size == size && found->second.write_time == write_time) {
    ++hits_;
    found->second.used = true;
    return found->second.sha256;
  }
  ++misses_;
  bytes_hashed_ += size;
  const std::optional<std::string> digest = hash_file_of_size(path, size);
  if (!digest) {
    // Never remembered: a file that is unreadable now (Steam holding a
    // Workshop file while it updates it) would otherwise keep a wrong hash
    // across scans and restarts until the file changed again. Any older
    // entry is stale anyway, since the size or time no longer match it.
    entries_.erase(key);
    return std::nullopt;
  }
  entry &value = entries_[key];
  value.size = size;
  value.write_time = write_time;
  value.sha256 = *digest;
  value.used = true;
  return digest;
}

std::vector<std::pair<std::string, std::string>>
hash_files(const std::filesystem::path &folder, hash_cache *cache,
           std::vector<std::string> *unreadable) {
  std::vector<std::pair<std::string, std::string>> digests;
  for_each_file(folder, [&](const std::filesystem::path &file) {
    std::string name = relative_name(folder, file);
    const std::optional<std::string> digest =
        cache ? cache->file_sha256(file) : try_file_sha256_hex(file);
    if (!digest && unreadable) {
      unreadable->push_back(name);
    }
    digests.emplace_back(std::move(name), digest.value_or(std::string()));
  });
  std::ranges::sort(digests);
  if (unreadable) {
    std::ranges::sort(*unreadable);
  }
  return digests;
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

installed_content load_mod_folder(const std::filesystem::path &folder,
                                  const std::set<std::string> &taken_ids,
                                  diagnostics &diags, hash_cache *cache) {
  installed_content content;
  content.folder = utf8(folder.filename().u8string());
  std::error_code ec;
  content.path = std::filesystem::absolute(folder, ec);
  // Hash file by file: Workshop maps reach several gigabytes, and only the
  // manifest and workshop.json are needed as text.
  // The cache is keyed by absolute paths.
  std::vector<std::string> unreadable;
  const auto digests = hash_files(content.path.empty() ? folder : content.path,
                                  cache, &unreadable);
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
  std::ranges::sort(folders);
  std::set<std::string> taken;
  for (std::size_t index = 0; index < folders.size(); ++index) {
    const std::filesystem::path &folder = folders[index];
    if (options.cancel && options.cancel->load()) {
      scan.cancelled = true;
      break;
    }
    if (options.progress) {
      options.progress(index, folders.size(),
                       utf8(folder.filename().u8string()));
    }
    installed_content content =
        load_mod_folder(folder, taken, scan.diags, options.cache);
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
