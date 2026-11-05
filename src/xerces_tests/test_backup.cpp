#include "harness.hpp"

#include <fstream>

#include <xerces_core/backup.hpp>

using namespace xerces_core;
namespace fs = std::filesystem;

namespace {
struct temp_dir {
  fs::path path;
  explicit temp_dir(const char *name) : path(fs::temp_directory_path() / name) {
    fs::remove_all(path);
    fs::create_directories(path);
  }
  ~temp_dir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};

void write(const fs::path &file, const std::string &text) {
  fs::create_directories(file.parent_path());
  std::ofstream(file, std::ios::binary) << text;
}
} // namespace

TEST_CASE("backup: credentials never go in") {
  CHECK(is_credential_file("refresh_token.json"));
  CHECK(is_credential_file("account.key"));
  CHECK(is_credential_file("Credentials.dat"));
  CHECK(!is_credential_file("config.json"));
  CHECK(!is_credential_file("packs/weekend.xerces-pack.json"));
}

TEST_CASE("backup: snapshot, prune and restore") {
  temp_dir root("xerces_tests_backup");
  const fs::path config = root.path / "config";
  const fs::path backups = root.path / "backups";
  write(config / "config.json", "v1");
  write(config / "packs" / "a.xerces-pack.json", "pack");
  write(config / "session_token.txt", "secret");
  const std::vector<backup_source> sources = {{"config", config}};
  backup_options options;
  options.keep = 3;

  diagnostics diags;
  const auto first =
      create_backup(backups, sources, "20261104-100000", options, diags);
  REQUIRE(first.has_value());
  CHECK(fs::exists(*first / "config" / "config.json"));
  CHECK(fs::exists(*first / "config" / "packs" / "a.xerces-pack.json"));
  CHECK(!fs::exists(*first / "config" / "session_token.txt"));

  write(config / "config.json", "v2");
  write(config / "new.json", "added later");
  REQUIRE(restore_backup(*first, backups, sources, "20261104-110000", options,
                         diags));
  CHECK_EQ(xerces_tests::read_file(config / "config.json"), "v1");
  CHECK(!fs::exists(config / "new.json"));
  CHECK(fs::exists(config / "session_token.txt")); // kept in place
  // The state before the restore was saved too.
  const auto all = list_backups(backups);
  REQUIRE_EQ(all.size(), 2u);
  CHECK_EQ(xerces_tests::read_file(all[0] / "config" / "config.json"), "v2");

  for (int i = 0; i < 5; ++i) {
    REQUIRE(create_backup(backups, sources,
                          "20261105-00000" + std::to_string(i), options, diags)
                .has_value());
  }
  CHECK_EQ(list_backups(backups).size(), 3u);
  CHECK(list_backups(backups).front().filename().string() == "20261105-000004");
  CHECK(!diags.has_errors());
}

TEST_CASE("backup: the size cap is checked before copying") {
  temp_dir root("xerces_tests_backup_cap");
  write(root.path / "config" / "big.bin", std::string(4096, 'x'));
  backup_options options;
  options.size_cap = 1024;
  diagnostics diags;
  CHECK(!create_backup(root.path / "backups",
                       {{"config", root.path / "config"}}, "t", options, diags)
             .has_value());
  CHECK(diags.has("backup.too_large"));
  CHECK(list_backups(root.path / "backups").empty());
}
