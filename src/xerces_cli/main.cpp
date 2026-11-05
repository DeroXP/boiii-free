// xerces_cli: the command interface for scripts and Claude (DESIGN.md 15.2).
// Commands that only need xerces_core work without the game; the ones that
// talk to a running Xerces exit with 3 until the game-side pipe exists.

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <xerces_core/catalog.hpp>
#include <xerces_core/conflicts.hpp>
#include <xerces_core/hooks.hpp>
#include <xerces_core/json.hpp>
#include <xerces_core/manifest.hpp>
#include <xerces_core/pack.hpp>
#include <xerces_core/planner.hpp>
#include <xerces_core/sha256.hpp>
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
               [--stock-zones <n>] [--game-build <0x...>] [--json]
  plan explain  (same options)                     why the order is what it is
  conflicts check --mods-dir <dir> (--mod <id>... | --pack <file|code>)
                  [--stock <dir>] [--json]
  packs export --mods-dir <dir> --name <name> --mod <id>... [--mode zm]
               [--out <file>] [--code]
  packs import <file|XRC1.code> [--mods-dir <dir>] [--json]   validate + dry run
  packs validate <file|XRC1.code> [--json]

Need a running Xerces (exit 3 until the game-side pipe exists):
  status, mods enable/disable/order, plan apply, load, unload, reload,
  index run, packs apply, sig check, bench run, test run, diag bundle,
  logs tail

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
    request.stock_zones = static_cast<std::size_t>(std::stoul(*zones));
  }
  request.game_build = args.get("--game-build").value_or("unknown");
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
  catalog_scan scan = scan_mods_directory(fs::path(*dir));
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
  for (const auto &[path, contents] : read_files(root)) {
    has_fastfiles =
        has_fastfiles || (path.starts_with("zone/") && path.ends_with(".ff"));
  }
  for (const auto &[file, expected] : m.integrity) {
    const fs::path path = root / file;
    if (!fs::exists(path)) {
      diags.error("mod.integrity.missing_file", "/integrity/" + file,
                  file + " is listed in integrity but missing.");
    } else if (sha256_tagged(read_text(path)) != expected) {
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
  if (source.starts_with(share_code_prefix)) {
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
              const std::optional<std::string> &stock) {
  checked out;
  out.resolved = resolve_plan(scan.catalog, request);
  conflict_inputs inputs;
  inputs.resolved = &out.resolved;
  std::map<std::string, manifest> manifests;
  for (const auto &content : scan.catalog) {
    manifests[content.info.id] = content.info;
  }
  inputs.manifests = manifests;
  const auto add_index = [&](const plan_entry &entry) {
    if (const auto root = scan.roots.find(entry.id); root != scan.roots.end()) {
      inputs.indexes.push_back(
          index_loose_files(entry.id, read_files(root->second)));
    }
  };
  for (const auto &entry : out.resolved.stack) {
    add_index(entry);
  }
  if (out.resolved.map) {
    add_index(*out.resolved.map);
  }
  if (stock) {
    inputs.stock = index_loose_files("stock", read_files(fs::path(*stock)));
  }
  if (decisions != nullptr) {
    inputs.allow_undeclared = decisions->allow_overrides;
    inputs.decided = decisions->keep_order;
    inputs.map_first_for_scripts = decisions->map_scripts_first;
  }
  out.report = check_conflicts(inputs);
  out.hooks = plan_hooks(out.resolved, manifests);
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
            args.get("--stock"));
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
    const checked dry =
        check(*scan, request, &result.value.decisions, args.get("--stock"));
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

int not_running(const std::string &command) {
  std::cerr << "Xerces is not running. \"" << command
            << "\" talks to a running Xerces over its named pipe, which comes "
               "with the client build.\n";
  return exit_not_running;
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
