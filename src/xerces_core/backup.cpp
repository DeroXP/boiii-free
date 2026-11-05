#include "backup.hpp"

#include <algorithm>
#include <cctype>

namespace xerces_core {
namespace fs = std::filesystem;

namespace {
std::string lower(std::string text) {
  std::ranges::transform(text, text.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return text;
}

std::string utf8(const fs::path &path) {
  const auto text = path.generic_u8string();
  return {reinterpret_cast<const char *>(text.data()), text.size()};
}

void prune(const fs::path &backups_dir, std::size_t keep) {
  auto backups = list_backups(backups_dir);
  std::error_code ec;
  while (backups.size() > keep) {
    fs::remove_all(backups.back(), ec);
    backups.pop_back();
  }
}
} // namespace

bool is_credential_file(const fs::path &relative) {
  const std::string name = lower(utf8(relative.filename()));
  for (const char *word :
       {"credential", "token", "secret", "password", "refresh"}) {
    if (name.find(word) != std::string::npos) {
      return true;
    }
  }
  const std::string ext = lower(utf8(relative.extension()));
  return ext == ".key" || ext == ".pem" || ext == ".pfx";
}

std::optional<fs::path> create_backup(const fs::path &backups_dir,
                                      const std::vector<backup_source> &sources,
                                      const std::string &timestamp,
                                      const backup_options &options,
                                      diagnostics &diags) {
  std::error_code ec;
  // The size cap is checked before anything is copied.
  std::uintmax_t total = 0;
  for (const auto &source : sources) {
    if (!fs::exists(source.folder, ec)) {
      continue;
    }
    for (const auto &entry :
         fs::recursive_directory_iterator(source.folder, ec)) {
      if (entry.is_regular_file(ec) &&
          !is_credential_file(fs::relative(entry.path(), source.folder, ec))) {
        total += entry.file_size(ec);
      }
    }
  }
  if (total > options.size_cap) {
    diags.error("backup.too_large", "",
                "The files to back up (" + std::to_string(total / 1024) +
                    " KiB) exceed the backup size cap.");
    return std::nullopt;
  }

  fs::path target = backups_dir / timestamp;
  for (int suffix = 2; fs::exists(target, ec); ++suffix) {
    target = backups_dir / (timestamp + "-" + std::to_string(suffix));
  }
  fs::create_directories(target, ec);
  if (ec) {
    diags.error("backup.write", "",
                "Cannot create the backup folder: " + ec.message());
    return std::nullopt;
  }
  for (const auto &source : sources) {
    const fs::path destination = target / source.label;
    fs::create_directories(destination, ec);
    if (!fs::exists(source.folder, ec)) {
      continue; // an empty folder records that there was nothing
    }
    for (const auto &entry :
         fs::recursive_directory_iterator(source.folder, ec)) {
      const fs::path relative = fs::relative(entry.path(), source.folder, ec);
      if (entry.is_directory(ec)) {
        fs::create_directories(destination / relative, ec);
      } else if (entry.is_regular_file(ec) && !is_credential_file(relative)) {
        fs::create_directories((destination / relative).parent_path(), ec);
        fs::copy_file(entry.path(), destination / relative,
                      fs::copy_options::overwrite_existing, ec);
        if (ec) {
          diags.error("backup.write", utf8(relative),
                      "Cannot copy " + utf8(relative) + ": " + ec.message());
          fs::remove_all(target, ec);
          return std::nullopt;
        }
      }
    }
  }
  prune(backups_dir, options.keep);
  return target;
}

std::vector<fs::path> list_backups(const fs::path &backups_dir) {
  std::vector<fs::path> out;
  std::error_code ec;
  for (const auto &entry : fs::directory_iterator(backups_dir, ec)) {
    if (entry.is_directory(ec)) {
      out.push_back(entry.path());
    }
  }
  std::ranges::sort(out, std::greater<>{}); // timestamps sort as text
  return out;
}

bool restore_backup(const fs::path &backup, const fs::path &backups_dir,
                    const std::vector<backup_source> &sources,
                    const std::string &timestamp, const backup_options &options,
                    diagnostics &diags) {
  std::error_code ec;
  if (!fs::is_directory(backup, ec)) {
    diags.error("backup.missing", "", "That backup does not exist.");
    return false;
  }
  // The restore is itself backed up first, and that one is never pruned away
  // by this call: keep one extra.
  backup_options keep_more = options;
  keep_more.keep = options.keep + 1;
  if (!create_backup(backups_dir, sources, timestamp + "-before-restore",
                     keep_more, diags)) {
    return false;
  }
  for (const auto &source : sources) {
    const fs::path saved = backup / source.label;
    if (!fs::is_directory(saved, ec)) {
      continue; // this backup did not include the folder
    }
    // Keep credentials that live in the folder; replace everything else.
    if (fs::exists(source.folder, ec)) {
      for (const auto &entry : fs::directory_iterator(source.folder, ec)) {
        if (!is_credential_file(entry.path().filename())) {
          fs::remove_all(entry.path(), ec);
        }
      }
    }
    fs::create_directories(source.folder, ec);
    fs::copy(saved, source.folder,
             fs::copy_options::recursive | fs::copy_options::overwrite_existing,
             ec);
    if (ec) {
      diags.error("backup.restore", source.label,
                  "Restoring " + source.label + " failed: " + ec.message());
      return false;
    }
  }
  return true;
}
} // namespace xerces_core
