// xerces_cli: the command interface for scripts and Claude (DESIGN.md 15.2).
// Commands that only need xerces_core work without the game; the ones that
// talk to a running Xerces exit with 3 until the game-side pipe exists.

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include <xerces_core/bundle.hpp>
#include <xerces_core/catalog.hpp>
#include <xerces_core/compat.hpp>
#include <xerces_core/conflicts.hpp>
#include <xerces_core/detect.hpp>
#include <xerces_core/hooks.hpp>
#include <xerces_core/ipc.hpp>
#include <xerces_core/json.hpp>
#include <xerces_core/log.hpp>
#include <xerces_core/manifest.hpp>
#include <xerces_core/pack.hpp>
#include <xerces_core/planner.hpp>
#include <xerces_core/sha256.hpp>
#include <xerces_core/sigregistry.hpp>
#include <xerces_core/sigscan.hpp>
#include <xerces_core/version.hpp>

using namespace xerces_core;
namespace fs = std::filesystem;

namespace {
enum exit_code {
  exit_ok = 0,
  exit_error = 1,
  exit_failed = 2, // validation or conflict failures
  exit_not_running = 3,
  exit_signatures = 4,
};

const std::set<std::string> flag_names = {
    "--json",    "--code",        "--help",    "-h",
    "--version", "--api-version", "--map-last"};

struct arguments {
  std::vector<std::string> words;
  std::map<std::string, std::vector<std::string>> options;
  std::set<std::string> flags;

  bool has(const std::string &flag) const { return flags.contains(flag); }
  std::optional<std::string> get(const std::string &name) const {
    const auto it = options.find(name);
    if (it == options.end() || it->second.empty()) {
      return std::nullopt;
    }
    return it->second.back();
  }
  std::vector<std::string> all(const std::string &name) const {
    const auto it = options.find(name);
    return it == options.end() ? std::vector<std::string>{} : it->second;
  }
  std::string word(std::size_t index) const {
    return index < words.size() ? words[index] : std::string{};
  }
};

arguments parse_arguments(int argc, char **argv) {
  arguments out;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (flag_names.contains(arg)) {
      out.flags.insert(arg);
    } else if (arg.starts_with("--")) {
      if (const std::size_t eq = arg.find('='); eq != std::string::npos) {
        out.options[arg.substr(0, eq)].push_back(arg.substr(eq + 1));
      } else if (i + 1 < argc) {
        out.options[arg].push_back(argv[++i]);
      } else {
        out.options[arg].push_back("");
      }
    } else {
      out.words.push_back(arg);
    }
  }
  return out;
}

std::string read_text(const fs::path &path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::string utf8(const fs::path &path) {
  const auto text = path.generic_u8string();
  return {reinterpret_cast<const char *>(text.data()), text.size()};
}

void print_diagnostics(const diagnostics &diags) {
  for (const auto &item : diags.items()) {
    std::cout << to_string(item.level) << ": " << item.message;
    if (!item.path.empty()) {
      std::cout << " [" << item.path << "]";
    }
    std::cout << " (" << item.code << ")\n";
  }
}

std::string diagnostics_json(const diagnostics &diags) {
  rapidjson::Document doc(rapidjson::kArrayType);
  auto &alloc = doc.GetAllocator();
  for (const auto &item : diags.items()) {
    rapidjson::Value object(rapidjson::kObjectType);
    json::set(object, "level", to_string(item.level), alloc);
    json::set(object, "code", item.code, alloc);
    json::set(object, "path", item.path, alloc);
    json::set(object, "message", item.message, alloc);
    doc.PushBack(std::move(object), alloc);
  }
  return json::pretty(doc);
}

int usage() {
  std::cout << R"(xerces_cli - Xerces command interface (api )"
            << cli_api_version << R"()

Work without the game:
  mod validate <folder|xerces.mod.json> [--json]   check a manifest and its files
  mod lint <folder|xerces.mod.json> [--json]       validate, plus author advice
  mod new <id> [--dir <parent>] [--name <name>] [--mode zm|mp|cp]
  mods list --mods-dir <dir> [--json]
  plan resolve --mods-dir <dir> [--mode zm] --mod <id>... [--map <id>]
               [--stock-zones <n>] [--game-build <0x...>]
               [--game-dir <folder>] [--json]      asset pool defaults from the
                                                   game's launcher_settings.json
  plan explain  (same options)                     why the order is what it is
  conflicts check --mods-dir <dir> (--mod <id>... | --pack <file|code>)
                  [--stock <dir>] [--map-last] [--json]
  packs export --mods-dir <dir> --name <name> --mod <id>... [--mode zm]
               [--out <file>] [--code]
  packs import <file|XRC1.code> [--mods-dir <dir>] [--json]   validate + dry run
  packs validate <file|XRC1.code> [--json]
  game detect [--saved <folder>] [--json]          find the game install (reads only)
  game check [--game-dir <folder>] [--builds <file>] [--json]
                                                   the launcher's pre-flight check:
                                                   is this game build supported?
  sig check --dump <file|folder> [--registry <file>] [--build <0x...>] [--json]
                                                   the signature registry against
                                                   a memory dump (newest .bin in
                                                   a folder)

Talk to the running game (started from Xerces.exe, or with -xerces-ipc), over
its pipe in <home>\run (--home <dir>, else XERCES_HOME, else
%LOCALAPPDATA%\Xerces):
  status                                           build, signatures, plan,
                                                   journal phase and mods
  load --extras <id,id,...>                        set the extra mods and reload
  reload [--extras <id,id,...>]                    reload (in the menus only)
  unload                                           no extra mods

  diag bundle [--home <dir>] [--out <file.zip>]    the diagnostics zip (logs,
                                                   journal, plan, signatures,
                                                   system), scrubbed; stays local

Not built yet (exit 3): mods enable/disable/order, plan apply, index run,
  packs apply, sig check (without --dump), bench run, test run, logs tail

Exit codes: 0 ok, 1 error, 2 validation or conflict failures,
            3 Xerces not running, 4 signature failures.
)";
  return exit_ok;
}

plan_request request_from(const arguments &args) {
  plan_request request;
  request.mode = args.get("--mode").value_or("zm");
  request.mods = args.all("--mod");
  request.map = args.get("--map");
  if (const auto zones = args.get("--stock-zones")) {
    // from_chars, not stoul: stoul takes "-1" as 4294967295.
    std::size_t count = 0;
    const char *end = zones->data() + zones->size();
    const auto parsed = std::from_chars(zones->data(), end, count);
    if (zones->empty() || parsed.ec != std::errc{} || parsed.ptr != end) {
      throw std::invalid_argument(
          "--stock-zones takes a number of zones, not \"" + *zones + "\".");
    }
    request.stock_zones = count;
  }
  request.game_build = args.get("--game-build").value_or("unknown");
  // The pool report's defaults: the game folder's launcher_settings.json, or
  // the client's own defaults without --game-dir (or without the file).
  if (const auto game_dir = args.get("--game-dir")) {
    request.launcher_settings =
        read_text(fs::path(*game_dir) / "boiii_players" / "user" /
                  "launcher_settings.json");
  }
  request.xerces_version =
      version::parse(xerces_version_text).value_or(version{});
  request.protocol_version = protocol_version;
  return request;
}

std::optional<catalog_scan> scan_from(const arguments &args) {
  const auto dir = args.get("--mods-dir");
  if (!dir) {
    std::cerr << "--mods-dir is required.\n";
    return std::nullopt;
  }
  scan_options options;
  options.layout = layout_of_directory(fs::path(*dir));
  catalog_scan scan = scan_mods_directory(fs::path(*dir), options);
  if (scan.diags.has_errors() && !args.has("--json")) {
    print_diagnostics(scan.diags);
  }
  return scan;
}

int cmd_mod_validate(const arguments &args, bool lint) {
  const fs::path target = args.word(2);
  if (target.empty()) {
    std::cerr << "Give a mod folder or a xerces.mod.json file.\n";
    return exit_error;
  }
  const fs::path manifest_path = fs::is_directory(target)
                                     ? target / std::string(manifest_file_name)
                                     : target;
  const fs::path root = manifest_path.parent_path();
  if (!fs::exists(manifest_path)) {
    std::cerr << utf8(manifest_path) << " does not exist.\n";
    return exit_error;
  }
  manifest_result result = parse_manifest(read_text(manifest_path));
  diagnostics &diags = result.diags;
  const manifest &m = result.value;

  bool has_fastfiles = false;
  for (const std::string &path : list_files(root)) {
    has_fastfiles =
        has_fastfiles || (path.starts_with("zone/") && path.ends_with(".ff"));
  }
  for (const auto &[file, expected] : m.integrity) {
    const fs::path path = root / file;
    if (!fs::exists(path)) {
      diags.error("mod.integrity.missing_file", "/integrity/" + file,
                  file + " is listed in integrity but missing.");
    } else if (const auto digest = try_file_sha256_hex(path); !digest) {
      // Not a mismatch: the file may be fine, it just could not be read.
      diags.error("mod.integrity.unreadable", "/integrity/" + file,
                  file + " could not be read in full (another program may "
                         "hold it open), so its integrity hash was not "
                         "checked.");
    } else if ("sha256:" + *digest != expected) {
      diags.error("mod.integrity.mismatch", "/integrity/" + file,
                  file + " does not match its integrity hash.");
    }
  }
  const auto check_script = [&](const std::string &script,
                                const std::string &where) {
    if (!fs::exists(root / script)) {
      if (has_fastfiles) {
        diags.info("mod.script.in_fastfile", where,
                   script + " is not a loose file; it is checked once the mod "
                            "is indexed.");
      } else {
        diags.error("mod.script.missing", where, script + " does not exist.");
      }
    }
  };
  for (std::size_t i = 0; i < m.entry_scripts.size(); ++i) {
    check_script(m.entry_scripts[i], "/scripts/entry/" + std::to_string(i));
  }
  for (std::size_t i = 0; i < m.hooks.size(); ++i) {
    const std::string &handler = m.hooks[i].handler;
    check_script(handler.substr(0, handler.find("::")),
                 "/scripts/hooks/" + std::to_string(i) + "/handler");
  }
  if (lint) {
    if (m.license.empty()) {
      diags.warning("lint.license", "/license",
                    "Say which licence the mod uses (an SPDX id).");
    }
    if (m.authors.empty()) {
      diags.warning("lint.authors", "/authors", "List the authors.");
    }
    if (!m.workshop_id && !m.legacy) {
      diags.info("lint.source", "/source",
                 "Add source.workshop once the mod is published.");
    }
  }
  if (args.has("--json")) {
    std::cout << diagnostics_json(diags) << "\n";
  } else {
    print_diagnostics(diags);
    std::cout << (diags.has_errors() ? "Invalid: " : "Valid: ")
              << (m.name.empty() ? utf8(manifest_path) : m.name) << "\n";
  }
  return diags.has_errors() ? exit_failed : exit_ok;
}

int cmd_mod_new(const arguments &args) {
  const std::string id = args.word(2);
  if (!valid_mod_id(id)) {
    std::cerr << "\"" << id
              << "\" is not a valid id: 3 to 64 characters, lower-case "
                 "letters, digits, '.', '_' and '-'.\n";
    return exit_error;
  }
  const std::string mode = args.get("--mode").value_or("zm");
  if (!valid_mode(mode)) {
    std::cerr << "Modes are zm, mp and cp.\n";
    return exit_error;
  }
  const fs::path folder = fs::path(args.get("--dir").value_or(".")) / id;
  if (fs::exists(folder)) {
    std::cerr << utf8(folder) << " already exists.\n";
    return exit_error;
  }
  std::string short_name = id;
  std::ranges::replace(short_name, '.', '_');
  std::ranges::replace(short_name, '-', '_');
  const std::string script = "scripts/" + mode + "/" + short_name + ".gsc";

  manifest m;
  m.id = id;
  m.name = args.get("--name").value_or(id);
  m.ver = *version::parse("0.1.0");
  m.license = "CC-BY-4.0";
  m.authors = {"you"};
  m.requires_xerces_text = ">=1.0 <2";
  m.modes = {mode};
  m.provides.push_back({"scriptparsetree", script, "", {}});
  m.entry_scripts = {script};
  const auto parsed = json::parse(manifest_to_json(m));

  fs::create_directories(folder / "scripts" / mode);
  std::ofstream(folder / std::string(manifest_file_name), std::ios::binary)
      << json::pretty(parsed.document) << "\n";
  std::ofstream(folder / script, std::ios::binary)
      << "// " << m.name << " (" << id << ")\n#namespace " << short_name
      << ";\n\nfunction autoexec __init__()\n{\n}\n";
  std::ofstream(folder / "README.md", std::ios::binary)
      << "# " << m.name
      << "\n\nCheck the manifest with `xerces_cli mod validate " << id
      << "` and list conflicts with `xerces_cli conflicts check`.\n";
  std::cout << "Created " << utf8(folder) << "\n";
  return exit_ok;
}

int cmd_mods_list(const arguments &args) {
  const auto scan = scan_from(args);
  if (!scan) {
    return exit_error;
  }
  if (args.has("--json")) {
    rapidjson::Document doc(rapidjson::kArrayType);
    auto &alloc = doc.GetAllocator();
    for (const auto &content : scan->catalog) {
      rapidjson::Value object(rapidjson::kObjectType);
      json::set(object, "id", content.info.id, alloc);
      json::set(object, "name", content.info.name, alloc);
      json::set(object, "version", content.info.ver.to_string(), alloc);
      json::set(object, "kind", to_string(content.info.kind), alloc);
      json::set(object, "folder", content.folder, alloc);
      json::set(object, "legacy", content.info.legacy, alloc);
      json::set(object, "content_hash", content.content_hash, alloc);
      doc.PushBack(std::move(object), alloc);
    }
    std::cout << json::pretty(doc) << "\n";
  } else {
    for (const auto &content : scan->catalog) {
      std::cout << content.info.id << "  " << content.info.ver.to_string()
                << "  " << content.info.name
                << (content.info.legacy ? "  (legacy)" : "")
                << (content.info.kind == content_kind::map ? "  [map]" : "")
                << "\n";
    }
  }
  return scan->diags.has_errors() ? exit_failed : exit_ok;
}

// The asset pools the stack's pool files raise above the client's default
// (DESIGN.md 5.4 step 4), and the ones they ask for that cannot grow.
void print_pools(const plan &p) {
  const auto default_text = [](const pool_need &need) {
    return need.default_size ? std::to_string(*need.default_size)
                             : std::string("the game's own size");
  };
  std::vector<const pool_need *> raised, fixed;
  for (const auto &need : p.pools.stack) {
    if (need.raises) {
      raised.push_back(&need);
    } else if (need.fixed) {
      fixed.push_back(&need);
    }
  }
  if (raised.empty()) {
    std::cout << "  asset pools: the stack raises none\n";
  } else {
    std::cout << "  asset pools the stack raises"
              << (p.pools.enabled
                      ? ""
                      : " (launcher_settings.json turns the client's own "
                        "pool sizes off)")
              << ":\n";
  }
  for (const pool_need *need : raised) {
    const asset_pool_info *pool = find_asset_pool(need->type);
    std::cout << "    " << pool->label << " (" << pool->setting_key
              << "): " << need->size << ", default " << default_text(*need)
              << "  [" << need->mod << "]\n";
  }
  for (const pool_need *need : fixed) {
    std::cout << "  " << find_asset_pool(need->type)->label << ": " << need->mod
              << " asks for " << need->size
              << ", but the engine sizes this pool itself; the client "
                 "cannot grow it\n";
  }
  std::size_t installed = 0;
  for (const auto &need : p.pools.installed) {
    installed += need.raises ? 1 : 0;
  }
  if (installed > 0) {
    std::cout << "  every installed mod together raises " << installed
              << " asset pool" << (installed == 1 ? "" : "s")
              << " (asset_pools.installed in --json)\n";
  }
}

void print_plan(const plan &p, bool explain) {
  std::cout << "Plan for " << p.mode << ": " << p.stack.size() << " mod"
            << (p.stack.size() == 1 ? "" : "s") << "\n";
  for (std::size_t i = 0; i < p.stack.size(); ++i) {
    const auto &entry = p.stack[i];
    std::cout << "  " << (i + 1) << ". " << entry.name << " (" << entry.id
              << ") " << entry.ver.to_string() << (i == 0 ? "  [main mod]" : "")
              << (entry.requested ? "" : "  [dependency]") << "\n";
  }
  if (p.map) {
    std::cout << "  map: " << p.map->name << " (" << p.map->id << ")\n";
  }
  if (!p.stack.empty()) {
    std::cout << "  +set fs_game " << p.fs_game();
    if (!p.fs_extramods().empty()) {
      std::cout << " +set fs_extramods \"" << p.fs_extramods() << "\"";
    }
    std::cout << "\n";
  }
  std::cout << "  zones added: " << p.zones_added;
  if (p.zones_total) {
    std::cout << " (" << *p.zones_total << " of " << zone_slot_limit << ")";
  }
  std::cout << "\n";
  print_pools(p);
  if (explain) {
    for (const auto &item : p.explanations) {
      std::cout << "  - " << item.message << "\n";
    }
  }
  print_diagnostics(p.diags);
  for (const auto &fix : p.fixes) {
    std::cout << "  fix: " << fix.kind << " " << fix.id
              << (fix.detail.empty() ? "" : " " + fix.detail) << "\n";
  }
  std::cout << "plan_id " << p.plan_id << "\ncontent_id " << p.content_id
            << "\n";
}

int cmd_plan(const arguments &args, bool explain) {
  const auto scan = scan_from(args);
  if (!scan) {
    return exit_error;
  }
  const plan p = resolve_plan(scan->catalog, request_from(args));
  if (args.has("--json")) {
    std::cout << plan_to_json(p, true) << "\n";
  } else {
    print_plan(p, explain);
  }
  return p.ok() ? exit_ok : exit_failed;
}

pack_result load_pack(const std::string &source) {
  // A code pasted with a space or line break around it is still a code.
  const std::size_t start = source.find_first_not_of(" \t\r\n");
  if (start != std::string::npos &&
      std::string_view(source).substr(start).starts_with(share_code_prefix)) {
    return decode_share_code(source);
  }
  if (!fs::exists(source)) {
    pack_result missing;
    missing.diags.error("pack.file", "", source + " does not exist.");
    return missing;
  }
  return parse_pack(read_text(source));
}

struct checked {
  plan resolved;
  conflict_report report;
  hook_plan hooks;
};

checked check(const catalog_scan &scan, const plan_request &request,
              const pack_decisions *decisions,
              const std::optional<std::string> &stock, bool map_last) {
  checked out;
  out.resolved = resolve_plan(scan.catalog, request);
  conflict_inputs inputs;
  inputs.resolved = &out.resolved;
  // The manifest and files of the version the plan picked, also when two
  // versions of an id are installed.
  std::map<std::string, manifest> manifests;
  const auto add = [&](const plan_entry &entry) {
    const auto content =
        std::ranges::find_if(scan.catalog, [&](const installed_content &c) {
          return c.path == entry.path;
        });
    if (content != scan.catalog.end()) {
      manifests[entry.id] = content->info;
      inputs.indexes.push_back(
          index_loose_files(entry.id, read_files(entry.path)));
    }
  };
  for (const auto &entry : out.resolved.stack) {
    add(entry);
  }
  if (out.resolved.map) {
    add(*out.resolved.map);
  }
  inputs.manifests = manifests;
  if (stock) {
    inputs.stock = index_loose_files(std::string(stock_owner),
                                     read_files(fs::path(*stock)));
  }
  if (decisions != nullptr) {
    inputs.allow_undeclared = decisions->allow_overrides;
    inputs.decided = decisions->keep_order;
    inputs.map_first_for_scripts = decisions->map_scripts_first;
  }
  if (map_last) {
    inputs.map_first_for_scripts = false;
  }
  out.report = check_conflicts(inputs);
  out.hooks = plan_hooks(out.resolved, manifests, inputs.map_first_for_scripts);
  return out;
}

int cmd_conflicts(const arguments &args) {
  const auto scan = scan_from(args);
  if (!scan) {
    return exit_error;
  }
  plan_request request = request_from(args);
  std::optional<pack> loaded;
  if (const auto source = args.get("--pack")) {
    pack_result result = load_pack(*source);
    if (!result.ok()) {
      print_diagnostics(result.diags);
      return exit_failed;
    }
    loaded = result.value;
    request =
        pack_to_request(*loaded, request.game_build, request.xerces_version);
  }
  const checked result =
      check(*scan, request, loaded ? &loaded->decisions : nullptr,
            args.get("--stock"), args.has("--map-last"));
  if (args.has("--json")) {
    std::cout << conflict_report_to_json(result.report, true) << "\n";
  } else {
    if (!result.resolved.ok()) {
      print_diagnostics(result.resolved.diags);
    }
    std::cout << conflict_report_to_text(result.report);
    for (const auto &chain : result.hooks.chains) {
      std::cout << "hooks on " << chain.target << ":";
      for (const auto &link : chain.links) {
        std::cout << " " << to_string(link.kind) << ":" << link.mod;
      }
      std::cout << "\n";
    }
    print_diagnostics(result.hooks.diags);
  }
  const bool failed = !result.resolved.ok() || result.report.needs_decision() ||
                      result.hooks.diags.has_errors();
  return failed ? exit_failed : exit_ok;
}

int cmd_packs(const arguments &args) {
  const std::string action = args.word(1);
  if (action == "export") {
    const auto scan = scan_from(args);
    const auto name = args.get("--name");
    if (!scan || !name) {
      std::cerr << "packs export needs --mods-dir and --name.\n";
      return exit_error;
    }
    const plan_request request = request_from(args);
    const plan p = resolve_plan(scan->catalog, request);
    if (!p.ok()) {
      print_diagnostics(p.diags);
      return exit_failed;
    }
    pack out = pack_from_plan(p, *name, new_pack_id(), {});
    out.game_build = request.game_build == "unknown" ? "" : request.game_build;
    out.requires_xerces = ">=1.0";
    const std::string text = pack_to_json(out);
    if (const auto file = args.get("--out")) {
      std::ofstream(fs::path(*file), std::ios::binary) << text << "\n";
      std::cout << "Wrote " << *file << "\n";
    }
    if (args.has("--code") || !args.get("--out")) {
      std::cout << encode_share_code(out) << "\n";
    }
    return exit_ok;
  }
  if (action == "import" || action == "validate") {
    const std::string source = args.word(2);
    pack_result result = load_pack(source);
    if (action == "validate" || !result.ok() || !args.get("--mods-dir")) {
      if (args.has("--json")) {
        std::cout << diagnostics_json(result.diags) << "\n";
      } else {
        print_diagnostics(result.diags);
        if (result.ok()) {
          std::cout << "Valid pack: " << result.value.name << " ("
                    << result.value.mods.size() << " mods, "
                    << result.value.mode << ")\n";
        }
      }
      return result.ok() ? exit_ok : exit_failed;
    }
    // Import is a dry run (8.6): the plan, the content check and conflicts.
    const auto scan = scan_from(args);
    const plan_request request = pack_to_request(
        result.value, args.get("--game-build").value_or("unknown"),
        version::parse(xerces_version_text).value_or(version{}));
    const checked dry = check(*scan, request, &result.value.decisions,
                              args.get("--stock"), args.has("--map-last"));
    const diagnostics content = verify_pack_content(result.value, dry.resolved);
    if (args.has("--json")) {
      std::cout << "{\"plan\":" << plan_to_json(dry.resolved)
                << ",\"conflicts\":" << conflict_report_to_json(dry.report)
                << ",\"content\":" << diagnostics_json(content) << "}\n";
    } else {
      std::cout << "Pack: " << result.value.name << "\n";
      print_plan(dry.resolved, false);
      print_diagnostics(content);
      std::cout << conflict_report_to_text(dry.report);
    }
    const bool failed = !dry.resolved.ok() || content.has_errors() ||
                        dry.report.needs_decision();
    return failed ? exit_failed : exit_ok;
  }
  if (action == "apply") {
    std::cerr
        << "Xerces is not running (applying a pack needs the launcher).\n";
    return exit_not_running;
  }
  return usage();
}

// Finds the game the way the launcher's first run does (DESIGN.md 4.7);
// reads only.
int cmd_game_detect(const arguments &args) {
  std::optional<fs::path> saved;
  if (const auto path = args.get("--saved")) {
    saved = fs::path(*path);
  }
  const detection result =
      detect_game(saved, steam_roots_from_registry(), real_filesystem());
  if (args.has("--json")) {
    rapidjson::Document doc(rapidjson::kObjectType);
    auto &alloc = doc.GetAllocator();
    json::set(doc, "found", result.game_dir.has_value(), alloc);
    json::set(doc, "game_dir", result.game_dir ? utf8(*result.game_dir) : "",
              alloc);
    json::set(doc, "tried", json::string_array(result.tried, alloc), alloc);
    std::cout << json::pretty(doc) << "\n";
  } else if (result.game_dir) {
    std::cout << "Black Ops III: " << utf8(*result.game_dir) << "\n";
  } else {
    std::cout << "Black Ops III was not found. Looked in:\n";
    for (const auto &place : result.tried) {
      std::cout << "  " << place << "\n";
    }
  }
  return result.game_dir ? exit_ok : exit_failed;
}

std::string hex(std::uint64_t value);

// The launcher's pre-flight check (DESIGN.md 4.2): compares BlackOps3.exe and
// Steam's build number with compat/builds.json. Reads only.
int cmd_game_check(const arguments &args) {
  fs::path game_dir;
  if (const auto given = args.get("--game-dir")) {
    game_dir = fs::path(*given);
  } else {
    const detection found = detect_game(
        std::nullopt, steam_roots_from_registry(), real_filesystem());
    if (!found.game_dir) {
      std::cerr << "Black Ops III was not found; give --game-dir.\n";
      return exit_error;
    }
    game_dir = *found.game_dir;
  }
  const fs::path builds_path =
      args.get("--builds").value_or("compat/builds.json");
  if (!fs::exists(builds_path)) {
    std::cerr << utf8(builds_path)
              << " does not exist. Run from the repository root or give "
                 "--builds.\n";
    return exit_error;
  }
  const build_registry registry = parse_build_registry(read_text(builds_path));
  if (!registry.ok()) {
    print_diagnostics(registry.diags);
    return exit_failed;
  }
  const fs::path exe = game_dir / std::string(bo3_exe);
  std::string error;
  const auto facts = read_exe_facts(exe, &error);
  if (!facts) {
    std::cerr << utf8(exe) << ": " << error << "\n";
    return exit_error;
  }
  // <library>\steamapps\common\<game> -> <library>\steamapps\appmanifest_*.acf
  const fs::path manifest = game_dir.parent_path().parent_path() /
                            ("appmanifest_" + std::string(bo3_app_id) + ".acf");
  std::optional<std::uint64_t> steam_build;
  if (fs::exists(manifest)) {
    steam_build = appmanifest_buildid(read_text(manifest));
  }
  const preflight_result result =
      preflight_check(*facts, steam_build, registry, bo3_exe);

  if (args.has("--json")) {
    rapidjson::Document doc(rapidjson::kObjectType);
    auto &alloc = doc.GetAllocator();
    json::set(doc, "verdict", to_string(result.verdict), alloc);
    json::set(doc, "can_launch", result.can_launch(), alloc);
    json::set(doc, "build", result.build ? result.build->name : "", alloc);
    json::set(doc, "steam_build",
              steam_build ? std::to_string(*steam_build) : "", alloc);
    json::set(doc, "checksum", hex(facts->checksum), alloc);
    json::set(doc, "link_time", hex(facts->link_time), alloc);
    json::set(doc, "size", static_cast<std::int64_t>(facts->size), alloc);
    json::set(doc, "sha256", facts->sha256, alloc);
    json::set(doc, "message", result.message, alloc);
    std::cout << json::pretty(doc) << "\n";
  } else {
    std::cout << "Black Ops III: " << utf8(exe) << "\n"
              << "  Steam build "
              << (steam_build ? std::to_string(*steam_build) : "unknown")
              << ", checksum " << hex(facts->checksum) << ", link time "
              << hex(facts->link_time) << ", " << facts->size << " bytes\n"
              << "  " << to_string(result.verdict)
              << (result.build ? " (" + result.build->name + ")" : "") << "\n";
    if (!result.message.empty()) {
      std::cout << result.message << "\n";
    }
  }
  return result.can_launch() ? exit_ok : exit_failed;
}

int not_running(const std::string &command) {
  std::cerr << "Xerces is not running. \"" << command
            << "\" talks to a running Xerces over its named pipe, which comes "
               "with the client build.\n";
  return exit_not_running;
}

// <home> as the launcher and the game find it (DESIGN.md 3.5).
fs::path xerces_home(const arguments &args) {
  if (const auto given = args.get("--home")) {
    return fs::path(*given);
  }
  if (const char *set = std::getenv("XERCES_HOME"); set && *set) {
    return fs::path(set);
  }
  if (const char *local = std::getenv("LOCALAPPDATA"); local && *local) {
    return fs::path(local) / "Xerces";
  }
  return fs::path("xerces_home");
}

// diag bundle (5.10): the scrubbed zip a bug report needs, from <home>.
int cmd_diag_bundle(const arguments &args) {
  const fs::path home = xerces_home(args);
  fs::path out;
  if (const auto given = args.get("--out")) {
    out = fs::path(*given);
  } else {
    std::string stamp = json_log::now_utc().substr(0, 19);
    std::ranges::replace(stamp, ':', '-');
    out = home / "crash" / ("xerces-bundle-" + stamp + ".zip");
  }
  const char *profile = std::getenv("USERPROFILE");
  const bundle_result result =
      make_bundle(home, out, profile ? profile : "", system_info_json());
  if (!result.ok) {
    std::cerr << result.error << "\n";
    return exit_error;
  }
  std::cout << "Wrote " << utf8(result.file) << " with:\n";
  for (const auto &name : result.contents) {
    std::cout << "  " << name << "\n";
  }
  std::cout << "Nothing was sent anywhere.\n";
  return exit_ok;
}

// The live commands (15.2): talk to the running game over its pipe.
//   status [--json]                     build, signatures, plan, phase, mods
//   load --extras a,b | reload [--extras a,b] | unload   front end only
int cmd_live(const arguments &args, const std::string &command) {
  const fs::path run = xerces_home(args) / "run";
  const auto entries = ipc::live_run_entries(run);
  const auto game = std::ranges::find_if(
      entries, [](const ipc::run_entry &e) { return e.role == "game"; });
  if (game == entries.end()) {
    std::cerr << "No running game found in " << utf8(run)
              << ". Start it from Xerces.exe (or with -xerces-ipc) first.\n";
    return exit_not_running;
  }
  std::string method = command;
  std::string params = "{}";
  if (command == "load" || command == "reload") {
    method = "reload";
    if (const auto extras = args.get("--extras")) {
      std::vector<std::string> list;
      std::string item;
      for (const char c : *extras + ",") {
        if (c == ',') {
          if (!item.empty()) {
            list.push_back(item);
          }
          item.clear();
        } else {
          item.push_back(c);
        }
      }
      rapidjson::Document doc(rapidjson::kObjectType);
      json::set(doc, "extras", json::string_array(list, doc.GetAllocator()),
                doc.GetAllocator());
      params = json::canonical(doc);
    } else if (command == "load") {
      std::cerr << "load needs --extras <id,id,...> (the extra mods, highest "
                   "priority first).\n";
      return exit_error;
    }
  }
  const ipc::call_result called = ipc::call(*game, method, params);
  if (!called.ok) {
    std::cerr << "The game did not answer: " << called.error << "\n";
    return exit_not_running;
  }
  const auto response = ipc::parse_response(called.response);
  if (!response) {
    std::cerr << "The game sent a message that is not JSON-RPC.\n";
    return exit_error;
  }
  if (!response->ok) {
    std::cerr << "error " << response->code << ": " << response->message
              << "\n";
    return response->code == ipc::refused ? exit_failed : exit_error;
  }
  const auto parsed = json::parse(response->result);
  std::cout << (parsed.ok ? json::pretty(parsed.document) : response->result)
            << "\n";
  return exit_ok;
}

std::string hex(std::uint64_t value) {
  char digits[17] = {};
  const auto result = std::to_chars(digits, digits + 16, value, 16);
  std::string out = "0x" + std::string(digits, result.ptr);
  std::transform(out.begin() + 2, out.end(), out.begin() + 2,
                 [](char c) { return c >= 'a' && c <= 'f' ? c - 32 : c; });
  return out;
}

std::optional<std::vector<std::uint8_t>> read_binary(const fs::path &path) {
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in) {
    return std::nullopt;
  }
  const std::streamoff size = in.tellg();
  if (size < 0) {
    return std::nullopt;
  }
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
  in.seekg(0);
  in.read(reinterpret_cast<char *>(bytes.data()), size);
  if (!in) {
    return std::nullopt;
  }
  return bytes;
}

// A dump file, or the newest .bin dump in a folder.
std::optional<fs::path> dump_file(const fs::path &given) {
  std::error_code ec;
  if (!fs::is_directory(given, ec)) {
    return fs::exists(given, ec) ? std::optional<fs::path>(given)
                                 : std::nullopt;
  }
  std::optional<fs::path> newest;
  fs::file_time_type newest_time{};
  for (const auto &entry : fs::directory_iterator(given, ec)) {
    if (entry.path().extension() != ".bin" || !entry.is_regular_file(ec)) {
      continue;
    }
    const auto time = entry.last_write_time(ec);
    if (!newest || time > newest_time) {
      newest = entry.path();
      newest_time = time;
    }
  }
  return newest;
}

// Runs the signature registry against a memory dump (DESIGN.md 4.5): what a
// new game build breaks shows up here before anyone starts the game.
int cmd_sig(const arguments &args, const std::string &action) {
  if (action != "check") {
    return usage();
  }
  const auto given = args.get("--dump");
  if (!given) {
    return not_running("sig check");
  }
  const auto dump = dump_file(fs::path(*given));
  if (!dump) {
    std::cerr << *given << " is not a dump file or a folder with one.\n";
    return exit_error;
  }
  const fs::path registry_path =
      args.get("--registry").value_or("compat/signatures.json");
  if (!fs::exists(registry_path)) {
    std::cerr << utf8(registry_path)
              << " does not exist. Run from the repository root or give "
                 "--registry.\n";
    return exit_error;
  }
  const signature_registry registry =
      parse_signature_registry(read_text(registry_path));
  if (!registry.ok()) {
    print_diagnostics(registry.diags);
    return exit_failed;
  }
  const auto image = read_binary(*dump);
  if (!image) {
    std::cerr << "Cannot read " << utf8(*dump) << ".\n";
    return exit_error;
  }
  std::string error;
  const auto layout = read_image_layout(*image, &error);
  if (!layout) {
    std::cerr << utf8(*dump) << ": " << error << "\n";
    return exit_error;
  }
  std::uint32_t build = layout->checksum;
  if (const auto forced = args.get("--build")) {
    std::uint32_t value = 0;
    const std::string_view text = *forced;
    const char *end = text.data() + text.size();
    if (text.size() <= 2 || !text.starts_with("0x") ||
        std::from_chars(text.data() + 2, end, value, 16).ptr != end) {
      throw std::invalid_argument(
          "--build takes a checksum such as 0x6531394.");
    }
    build = value;
  }

  const signature_report report =
      resolve_signatures(registry.signatures, layout->sections, build);
  const bool all_ok = std::ranges::all_of(
      report.results, [](const signature_result &r) { return r.ok; });

  if (args.has("--json")) {
    rapidjson::Document doc(rapidjson::kObjectType);
    auto &alloc = doc.GetAllocator();
    json::set(doc, "dump", utf8(*dump), alloc);
    json::set(doc, "build", hex(build), alloc);
    json::set(doc, "ok", all_ok, alloc);
    rapidjson::Value results(rapidjson::kArrayType);
    for (const auto &result : report.results) {
      rapidjson::Value item(rapidjson::kObjectType);
      json::set(item, "id", result.id, alloc);
      json::set(item, "symbol", result.symbol, alloc);
      json::set(item, "ok", result.ok, alloc);
      json::set(item, "rva", result.ok ? hex(result.rva) : "", alloc);
      json::set(item, "matches", static_cast<std::int64_t>(result.matches),
                alloc);
      json::set(item, "error", result.error, alloc);
      results.PushBack(std::move(item), alloc);
    }
    json::set(doc, "results", std::move(results), alloc);
    rapidjson::Value disabled(rapidjson::kObjectType);
    for (const auto &[feature, ids] : report.disabled) {
      json::set(disabled, feature, json::string_array(ids, alloc), alloc);
    }
    json::set(doc, "features_off", std::move(disabled), alloc);
    std::cout << json::pretty(doc) << "\n";
  } else {
    std::cout << "Build " << hex(build) << ", " << report.results.size()
              << " signatures from " << utf8(registry_path) << ", dump "
              << utf8(dump->filename()) << "\n";
    for (const auto &result : report.results) {
      if (result.ok) {
        std::cout << "  ok    " << result.id << "  " << result.symbol << "  "
                  << hex(result.rva) << "\n";
      } else {
        std::cout << "  MISS  " << result.id << "  " << result.symbol << ": "
                  << result.error << "\n";
      }
    }
    for (const auto &[feature, ids] : report.disabled) {
      std::cout << "Off on this build: " << feature << " (";
      for (std::size_t i = 0; i < ids.size(); ++i) {
        std::cout << (i ? ", " : "") << ids[i];
      }
      std::cout << ")\n";
    }
  }
  return all_ok ? exit_ok : exit_signatures;
}
} // namespace

int main(int argc, char **argv) {
  const arguments args = parse_arguments(argc, argv);
  if (args.has("--api-version")) {
    std::cout << cli_api_version << "\n";
    return exit_ok;
  }
  if (args.has("--version")) {
    std::cout << "xerces_cli " << xerces_version_text << "\n";
    return exit_ok;
  }
  const std::string group = args.word(0);
  const std::string action = args.word(1);
  try {
    if (group.empty() || args.has("--help") || args.has("-h")) {
      return usage();
    }
    if (group == "mod" && (action == "validate" || action == "lint")) {
      return cmd_mod_validate(args, action == "lint");
    }
    if (group == "mod" && action == "new") {
      return cmd_mod_new(args);
    }
    if (group == "mods" && action == "list") {
      return cmd_mods_list(args);
    }
    if (group == "plan" && (action == "resolve" || action == "explain")) {
      return cmd_plan(args, action == "explain");
    }
    if (group == "conflicts" && action == "check") {
      return cmd_conflicts(args);
    }
    if (group == "packs") {
      return cmd_packs(args);
    }
    if (group == "game" && action == "detect") {
      return cmd_game_detect(args);
    }
    if (group == "game" && action == "check") {
      return cmd_game_check(args);
    }
    if (group == "sig") {
      return cmd_sig(args, action);
    }
    if (group == "diag" && action == "bundle") {
      return cmd_diag_bundle(args);
    }
    if (group == "status" || group == "load" || group == "reload" ||
        group == "unload") {
      return cmd_live(args, group);
    }
    const std::set<std::string> live = {"status", "mods",   "plan",  "load",
                                        "unload", "reload", "index", "sig",
                                        "bench",  "test",   "diag",  "logs"};
    if (live.contains(group)) {
      return not_running(group + (action.empty() ? "" : " " + action));
    }
    std::cerr << "Unknown command \"" << group << "\". Try --help.\n";
    return exit_error;
  } catch (const std::exception &e) {
    std::cerr << "error: " << e.what() << "\n";
    return exit_error;
  }
}
