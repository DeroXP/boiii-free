#include "catalog.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>

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

std::string file_sha256_hex(const std::filesystem::path &path) {
  sha256 hasher;
  std::ifstream in(path, std::ios::binary);
  std::vector<char> buffer(std::size_t{1} << 20);
  while (in) {
    in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const std::streamsize got = in.gcount();
    if (got > 0) {
      hasher.update(buffer.data(), static_cast<std::size_t>(got));
    }
  }
  return sha256::to_hex(hasher.finish());
}

std::vector<std::pair<std::string, std::string>>
hash_files(const std::filesystem::path &folder) {
  std::vector<std::pair<std::string, std::string>> digests;
  for_each_file(folder, [&](const std::filesystem::path &file) {
    digests.emplace_back(relative_name(folder, file), file_sha256_hex(file));
  });
  std::ranges::sort(digests);
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
                                  diagnostics &diags) {
  installed_content content;
  content.folder = utf8(folder.filename().u8string());
  std::error_code ec;
  content.path = std::filesystem::absolute(folder, ec);
  // Hash file by file: Workshop maps reach several gigabytes, and only the
  // manifest and workshop.json are needed as text.
  const auto digests = hash_files(folder);
  for (const auto &[path, digest] : digests) {
    content.files.push_back(path);
  }
  content.content_hash = content_hash_of_digests(digests);

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

catalog_scan scan_mods_directory(const std::filesystem::path &directory) {
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
  for (const auto &folder : folders) {
    installed_content content = load_mod_folder(folder, taken, scan.diags);
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
