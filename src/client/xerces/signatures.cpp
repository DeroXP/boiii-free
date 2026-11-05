#include <std_include.hpp>

#include "signatures.hpp"
#include "runtime.hpp"

#include <game/game.hpp>

#include <xerces_core/json.hpp>
#include <xerces_core/sigregistry.hpp>
#include <xerces_core/sigscan.hpp>

namespace xerces_signatures {
namespace {
struct state {
  bool registry_ok = false;
  xerces_core::signature_report report;
  std::uintptr_t base = 0;
  std::uint32_t checksum = 0;
  std::string problem; // why nothing could be resolved
};

std::string hex(std::uint64_t value) {
  char text[24];
  std::snprintf(text, sizeof(text), "0x%llX",
                static_cast<unsigned long long>(value));
  return text;
}

std::optional<std::filesystem::path> env_path(const wchar_t *name) {
  wchar_t buffer[32768];
  const DWORD size = GetEnvironmentVariableW(name, buffer, 32768);
  if (size == 0 || size >= 32768) {
    return std::nullopt;
  }
  return std::filesystem::path(std::wstring(buffer, size));
}

// compat/signatures.json: XERCES_COMPAT_PATH, next to the executable (an
// install), the repository a development build sits in, or next to
// XERCES_DATA_PATH (a development build started from the game folder).
std::optional<std::filesystem::path> find_registry() {
  std::error_code ec;
  const auto check = [&](const std::filesystem::path &dir)
      -> std::optional<std::filesystem::path> {
    const auto file = dir / "signatures.json";
    if (std::filesystem::exists(file, ec)) {
      return file;
    }
    return std::nullopt;
  };
  if (const auto dir = env_path(L"XERCES_COMPAT_PATH")) {
    if (auto file = check(*dir)) {
      return file;
    }
  }
  wchar_t module[32768]{};
  GetModuleFileNameW(nullptr, module, 32768);
  std::filesystem::path dir = std::filesystem::path(module).parent_path();
  for (int i = 0; i < 8 && !dir.empty(); ++i) {
    if (auto file = check(dir / "compat")) {
      return file;
    }
    const auto parent = dir.parent_path();
    if (parent == dir) {
      break;
    }
    dir = parent;
  }
  if (const auto data = env_path(L"XERCES_DATA_PATH")) {
    if (auto file = check(data->parent_path() / "compat")) {
      return file;
    }
  }
  return std::nullopt;
}

// The whole range is committed and readable: scanning a guard or no-access
// page would crash the game instead of turning a feature off.
bool readable(const std::uint8_t *start, std::size_t size) {
  const std::uint8_t *at = start;
  const std::uint8_t *end = start + size;
  while (at < end) {
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(at, &info, sizeof(info)) != sizeof(info) ||
        info.State != MEM_COMMIT ||
        (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0 ||
        (info.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                         PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                         PAGE_EXECUTE_WRITECOPY)) == 0) {
      return false;
    }
    at = static_cast<const std::uint8_t *>(info.BaseAddress) + info.RegionSize;
  }
  return true;
}

void write_report(const state &s, const std::string &registry,
                  bool from_cache) {
  rapidjson::Document doc(rapidjson::kObjectType);
  auto &alloc = doc.GetAllocator();
  namespace json = xerces_core::json;
  json::set(doc, "schema", "xerces.sigreport/1", alloc);
  json::set(doc, "game_build", hex(s.checksum), alloc);
  json::set(doc, "checked", xerces_core::json_log::now_utc(), alloc);
  json::set(doc, "registry", registry, alloc);
  json::set(doc, "from_cache", from_cache, alloc);
  if (!s.problem.empty()) {
    json::set(doc, "problem", s.problem, alloc);
  }
  rapidjson::Value entries(rapidjson::kArrayType);
  for (const auto &result : s.report.results) {
    rapidjson::Value item(rapidjson::kObjectType);
    json::set(item, "id", result.id, alloc);
    json::set(item, "name", result.symbol, alloc);
    json::set(item, "resolved", result.ok, alloc);
    if (result.ok) {
      json::set(item, "rva", hex(result.rva), alloc);
    } else {
      json::set(item, "error", result.error, alloc);
    }
    entries.PushBack(item, alloc);
  }
  json::set(doc, "entries", std::move(entries), alloc);
  rapidjson::Value disabled(rapidjson::kObjectType);
  for (const auto &[feature, ids] : s.report.disabled) {
    json::set(disabled, feature, json::string_array(ids, alloc), alloc);
  }
  json::set(doc, "disabled", std::move(disabled), alloc);
  xerces_runtime::write_file(xerces_runtime::home() / "cache" /
                                 "signatures-report.json",
                             json::pretty(doc));
}

state resolve() {
  const auto symbols_step = xerces_runtime::journal_symbols_step();
  state s;
  std::string registry_path;
  bool from_cache = false;
  try {
    const auto path = find_registry();
    if (!path) {
      s.problem = "compat/signatures.json was not found next to the game "
                  "client or in its repository";
    } else {
      registry_path = path->generic_string();
      const xerces_core::signature_registry registry =
          xerces_core::parse_signature_registry(xerces_runtime::read_file(*path));
      if (!registry.ok()) {
        s.problem = "compat/signatures.json is broken: " +
                    registry.diags.items().front().message;
      } else {
        s.registry_ok = true;
        s.base = game::get_engine_base();
        const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(s.base);
        const auto *base_bytes = reinterpret_cast<const std::uint8_t *>(s.base);
        if (!readable(base_bytes, 0x1000) || dos->e_magic != IMAGE_DOS_SIGNATURE) {
          s.problem = "the game's image header is not readable";
        } else {
          const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(
              base_bytes + dos->e_lfanew);
          const std::size_t size = nt->OptionalHeader.SizeOfImage;
          std::string error;
          const auto layout = xerces_core::read_image_layout(
              std::span<const std::uint8_t>(base_bytes, size), &error);
          if (!layout) {
            s.problem = "the game's image could not be read: " + error;
          } else {
            s.checksum = layout->checksum;
            const auto text = std::ranges::find_if(
                layout->sections,
                [](const auto &section) { return section.name == ".text"; });
            if (text == layout->sections.end() ||
                !readable(text->bytes.data(), text->bytes.size())) {
              s.problem = "the game's code section is not readable";
            } else {
              const auto cache_file =
                  xerces_runtime::home() / "cache" /
                  ("signatures-" + hex(s.checksum) + ".json");
              if (auto cached = xerces_core::load_signature_cache(
                      xerces_runtime::read_file(cache_file), s.checksum,
                      layout->sections, registry.signatures)) {
                s.report = std::move(*cached);
                from_cache = true;
              } else {
                s.report = xerces_core::resolve_signatures(
                    registry.signatures, layout->sections, s.checksum);
                xerces_runtime::write_file(
                    cache_file, xerces_core::signature_cache_to_json(
                                    s.report, s.checksum, layout->sections));
              }
            }
          }
        }
        if (!s.problem.empty()) {
          // Nothing resolved: every feature that needs a signature is off.
          for (const auto &sig : registry.signatures) {
            for (const auto &feature : sig.features) {
              s.report.disabled[feature].push_back(sig.id);
            }
            xerces_core::signature_result result;
            result.id = sig.id;
            result.symbol = sig.symbol;
            result.error = s.problem;
            s.report.results.push_back(std::move(result));
          }
        }
      }
    }
  } catch (const std::exception &e) {
    s.problem = std::string("signature resolution failed: ") + e.what();
  }

  std::size_t matched = 0;
  for (const auto &result : s.report.results) {
    matched += result.ok ? 1 : 0;
  }
  const std::string data =
      "{\"build\":" + xerces_core::json::quote(hex(s.checksum)) +
      ",\"matched\":" + std::to_string(matched) +
      ",\"total\":" + std::to_string(s.report.results.size()) +
      ",\"from_cache\":" + (from_cache ? "true" : "false") + "}";
  if (s.problem.empty() && matched == s.report.results.size()) {
    xerces_runtime::log_info("xerces.signatures.resolved",
                             "all signatures resolved", data);
  } else {
    xerces_runtime::log_warn("xerces.signatures.missing",
                             s.problem.empty() ? "some signatures did not resolve"
                                               : s.problem,
                             data);
    for (const auto &result : s.report.results) {
      if (!result.ok) {
        xerces_runtime::log_warn("xerces.signatures.miss", result.id,
                                 "{\"error\":" +
                                     xerces_core::json::quote(result.error) +
                                     "}");
      }
    }
  }
  write_report(s, registry_path, from_cache);
  return s;
}

const state &resolved() {
  static const state s = resolve();
  return s;
}
} // namespace

bool feature_enabled(std::string_view feature) {
  const state &s = resolved();
  return s.registry_ok && s.problem.empty() &&
         s.report.feature_enabled(feature);
}

std::optional<std::uintptr_t> address(std::string_view id) {
  const state &s = resolved();
  const xerces_core::signature_result *result = s.report.find(id);
  if (!result || !result->ok || s.base == 0) {
    return std::nullopt;
  }
  return s.base + static_cast<std::uintptr_t>(result->rva);
}

std::string feature_message(std::string_view feature,
                            std::string_view feature_label,
                            std::string_view still_works) {
  const state &s = resolved();
  if (feature_enabled(feature)) {
    return {};
  }
  if (!s.problem.empty()) {
    return std::string(feature_label) + " is off: " + s.problem + ". " +
           std::string(still_works);
  }
  return s.report.message(feature, feature_label, still_works);
}
} // namespace xerces_signatures
