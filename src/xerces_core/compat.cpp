#include "compat.hpp"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <fstream>
#include <set>

#include "catalog.hpp"
#include "json.hpp"

namespace xerces_core {
namespace {
const std::set<std::string, std::less<>> known_keys = {
    "name",      "exe",    "steam_build", "checksum", "size",
    "link_time", "sha256", "status",      "note"};

std::optional<std::uint64_t> hex_number(std::string_view text) {
  if (!text.starts_with("0x") || text.size() < 3 || text.size() > 18) {
    return std::nullopt;
  }
  std::uint64_t value = 0;
  const char *end = text.data() + text.size();
  const auto parsed = std::from_chars(text.data() + 2, end, value, 16);
  if (parsed.ec != std::errc{} || parsed.ptr != end) {
    return std::nullopt;
  }
  return value;
}

std::optional<std::uint32_t> hex32(const rapidjson::Value *value) {
  if (value == nullptr || !value->IsString()) {
    return std::nullopt;
  }
  const auto number = hex_number(
      std::string_view(value->GetString(), value->GetStringLength()));
  if (!number || *number > 0xFFFFFFFFu) {
    return std::nullopt;
  }
  return static_cast<std::uint32_t>(*number);
}

bool is_sha256_hex(std::string_view text) {
  return text.size() == 64 && std::ranges::all_of(text, [](char c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
         });
}

std::string join_builds(const std::vector<std::uint64_t> &builds) {
  std::string out;
  for (std::size_t i = 0; i < builds.size(); ++i) {
    if (i > 0) {
      out += i + 1 == builds.size() ? " and " : ", ";
    }
    out += std::to_string(builds[i]);
  }
  return out;
}

void parse_entry(const rapidjson::Value &entry, const std::string &path,
                 build_registry &out) {
  diagnostics &diags = out.diags;
  if (!entry.IsObject()) {
    diags.error("build.type", path, "Each build is an object.");
    return;
  }
  for (auto it = entry.MemberBegin(); it != entry.MemberEnd(); ++it) {
    const std::string key(it->name.GetString(), it->name.GetStringLength());
    if (!known_keys.contains(key)) {
      diags.warning("build.key.unknown", json::pointer(path, key),
                    "Unknown key \"" + key + "\"; ignored.");
    }
  }
  known_build build;
  build.name = json::get_string(entry, "name").value_or("");
  if (build.name.empty()) {
    diags.error("build.name", json::pointer(path, "name"), "name is required.");
  }
  build.exe = json::get_string(entry, "exe").value_or("BlackOps3.exe");

  const auto checksum = hex32(json::member(entry, "checksum"));
  if (!checksum) {
    diags.error("build.checksum", json::pointer(path, "checksum"),
                "checksum is required, as a \"0x\" hex string.");
  } else {
    build.checksum = *checksum;
  }
  if (const rapidjson::Value *link = json::member(entry, "link_time")) {
    build.link_time = hex32(link);
    if (!build.link_time) {
      diags.error("build.link_time", json::pointer(path, "link_time"),
                  "link_time is a \"0x\" hex string.");
    }
  }
  for (const char *name : {"steam_build", "size"}) {
    if (const rapidjson::Value *value = json::member(entry, name)) {
      if (!value->IsUint64()) {
        diags.error("build.number", json::pointer(path, name),
                    std::string(name) + " is a whole number.");
      } else if (std::strcmp(name, "size") == 0) {
        build.size = value->GetUint64();
      } else {
        build.steam_build = value->GetUint64();
      }
    }
  }
  if (const auto sha = json::get_string(entry, "sha256")) {
    if (!is_sha256_hex(*sha)) {
      diags.error("build.sha256", json::pointer(path, "sha256"),
                  "sha256 is 64 lower-case hex digits.");
    } else {
      build.sha256 = *sha;
    }
  }
  const std::string status = json::get_string(entry, "status").value_or("");
  if (status == "supported") {
    build.status = build_status::supported;
  } else if (status == "upstream-only") {
    build.status = build_status::upstream_only;
  } else if (status == "unsupported") {
    build.status = build_status::unsupported;
  } else {
    diags.error("build.status", json::pointer(path, "status"),
                "status is supported, upstream-only or unsupported.");
  }
  out.builds.push_back(std::move(build));
}
} // namespace

const char *to_string(build_status status) {
  switch (status) {
  case build_status::supported:
    return "supported";
  case build_status::upstream_only:
    return "upstream-only";
  case build_status::unsupported:
    return "unsupported";
  }
  return "unsupported";
}

const char *to_string(preflight_verdict verdict) {
  switch (verdict) {
  case preflight_verdict::supported:
    return "supported";
  case preflight_verdict::upstream_only:
    return "upstream-only";
  case preflight_verdict::unsupported:
    return "unsupported";
  case preflight_verdict::newer:
    return "newer";
  case preflight_verdict::unknown:
    return "unknown";
  }
  return "unknown";
}

const char *preflight_label(preflight_verdict verdict) {
  switch (verdict) {
  case preflight_verdict::supported:
    return "Supported";
  case preflight_verdict::upstream_only:
    return "Upstream only";
  case preflight_verdict::unsupported:
    return "Unsupported build";
  case preflight_verdict::newer:
    return "Newer than supported";
  case preflight_verdict::unknown:
    return "Unknown or modified";
  }
  return "Unknown or modified";
}

std::optional<exe_facts> read_exe_facts(const std::filesystem::path &exe,
                                        std::string *error, hash_cache *cache) {
  const auto fail = [&](std::string why) -> std::optional<exe_facts> {
    if (error != nullptr) {
      *error = std::move(why);
    }
    return std::nullopt;
  };
  std::error_code ec;
  const std::uintmax_t size = std::filesystem::file_size(exe, ec);
  if (ec) {
    return fail("cannot read the file: " + ec.message());
  }
  std::ifstream in(exe, std::ios::binary);
  std::vector<char> header(4096);
  in.read(header.data(), static_cast<std::streamsize>(header.size()));
  const auto got = static_cast<std::size_t>(in.gcount());
  const auto read_u32 = [&](std::size_t at, std::uint32_t &value) {
    if (at > got || got - at < 4) {
      return false;
    }
    std::memcpy(&value, header.data() + at, 4);
    return true;
  };
  std::uint32_t nt = 0;
  std::uint32_t signature = 0;
  exe_facts facts;
  if (got < 2 || header[0] != 'M' || header[1] != 'Z' || !read_u32(0x3C, nt) ||
      !read_u32(nt, signature) || signature != 0x00004550 ||
      !read_u32(std::size_t{nt} + 8, facts.link_time) ||
      !read_u32(std::size_t{nt} + 24 + 64, facts.checksum)) {
    return fail("not a Windows executable (PE header missing)");
  }
  facts.size = static_cast<std::uint64_t>(size);
  // An empty hash (the file could not be read in full) matches no build.
  facts.sha256 = cache != nullptr ? cache->file_sha256(exe).value_or("")
                                  : file_sha256_hex(exe);
  return facts;
}

build_registry parse_build_registry(std::string_view text) {
  build_registry out;
  const json::parse_result parsed = json::parse(text);
  if (!parsed.ok) {
    out.diags.error("build.json", "",
                    "Not JSON (line " + std::to_string(parsed.line) +
                        ", column " + std::to_string(parsed.column) +
                        "): " + parsed.error);
    return out;
  }
  const rapidjson::Value &doc = parsed.document;
  if (!doc.IsObject() ||
      json::get_string(doc, "schema") != build_registry_schema) {
    out.diags.error("build.schema", "/schema",
                    "schema must be \"" + std::string(build_registry_schema) +
                        "\".");
    return out;
  }
  const rapidjson::Value *list = json::member(doc, "builds");
  if (list == nullptr || !list->IsArray()) {
    out.diags.error("build.list", "/builds", "builds is a list.");
    return out;
  }
  for (rapidjson::SizeType i = 0; i < list->Size(); ++i) {
    parse_entry((*list)[i], json::pointer("/builds", i), out);
  }
  return out;
}

preflight_result preflight_check(const exe_facts &facts,
                                 std::optional<std::uint64_t> steam_build,
                                 const build_registry &registry,
                                 std::string_view exe_name) {
  preflight_result result;
  const known_build *best = nullptr;
  int best_score = -1;
  std::vector<std::uint64_t> supported;
  std::optional<std::uint64_t> newest_known;
  for (const known_build &build : registry.builds) {
    if (build.exe != exe_name) {
      continue;
    }
    if (build.steam_build) {
      newest_known = std::max(newest_known.value_or(0), *build.steam_build);
      if (build.status == build_status::supported) {
        supported.push_back(*build.steam_build);
      }
    }
    if (build.checksum != facts.checksum ||
        (build.size && *build.size != facts.size) ||
        (build.link_time && *build.link_time != facts.link_time) ||
        (build.sha256 && *build.sha256 != facts.sha256)) {
      continue;
    }
    const int score = (build.size ? 1 : 0) + (build.link_time ? 1 : 0) +
                      (build.sha256 ? 1 : 0);
    if (score > best_score) {
      best = &build;
      best_score = score;
    }
  }
  std::ranges::sort(supported);

  const std::string supports =
      supported.empty()
          ? std::string("Xerces does not list a supported build yet.")
          : "Xerces supports build" +
                std::string(supported.size() > 1 ? "s " : " ") +
                join_builds(supported) + ".";
  const std::string this_build =
      steam_build ? " (build " + std::to_string(*steam_build) + ")" : "";

  if (best != nullptr) {
    result.build = *best;
    switch (best->status) {
    case build_status::supported:
      result.verdict = preflight_verdict::supported;
      return result;
    case build_status::upstream_only:
      result.verdict = preflight_verdict::upstream_only;
      result.message = "Black Ops III" + this_build + " (" + best->name +
                       ") runs with Xerces, but some Xerces features can be "
                       "off on it. Xerces names them when it starts.";
      return result;
    case build_status::unsupported:
      result.verdict = preflight_verdict::unsupported;
      result.message = "Xerces does not support this Black Ops III build" +
                       this_build + " (" + best->name + "). " + supports +
                       " Play through Steam.";
      return result;
    }
  }
  if (steam_build && newest_known && *steam_build > *newest_known) {
    result.verdict = preflight_verdict::newer;
    result.message = "Steam updated Black Ops III" + this_build + ". " +
                     supports +
                     " Play through Steam until a Xerces update supports the "
                     "new build. Xerces will tell you when it does.";
    return result;
  }
  result.verdict = preflight_verdict::unknown;
  result.message = "This copy of Black Ops III" + this_build +
                   " is not a build Xerces knows, or its files have changed. " +
                   supports +
                   " Verify the game files in Steam, then start Xerces again.";
  return result;
}
} // namespace xerces_core
