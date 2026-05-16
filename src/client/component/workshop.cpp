#include <cstring>
#include <std_include.hpp>
#include "loader/component_loader.hpp"
#include "workshop.hpp"

#include "game/game.hpp"
#include "game/utils.hpp"
#include "command.hpp"

#include <utils/hook.hpp>
#include <utils/string.hpp>
#include <utils/io.hpp>
#include <utils/http.hpp>
#include <utils/thread.hpp>
#include "steamcmd.hpp"
#include "fastdl.hpp"
#include "party.hpp"
#include "scheduler.hpp"
#include "download_overlay.hpp"
#include "toast.hpp"

#include <condition_variable>
#include <fstream>
#include <mutex>
#include <regex>
#include <set>
#include <unordered_map>
#include <shellapi.h>

namespace workshop {
namespace {
std::thread download_thread{};
std::atomic_bool downloading{false};

// bo3-bundle: when set, every begin_load_scripts call drains the
// g_pending_bundle_scripts set (populated by bundle_load_ff's pool-diff).
// Each new script gets Scr_LoadScript so its autoexec fires at the right
// phase. Toggled via `bundle_auto_activate_scripts <0|1>`.
std::atomic<bool> g_auto_activate_scripts{false};

// bo3-bundle: set of script_parse_tree asset NAMES (with .gsc/.csc ext)
// captured during bundle_load_ff via the Com_PrintWarning hook on the
// "Redundant %s asset" call site. These are DS4C scripts that share names
// with Solo's pool entries -- they DID enter the asset linked list at head
// (so DB_FindXAssetHeader returns DS4C's version), but Scr_LoadScript was
// never called for them. drain_pending_bundle_activations replays the calls.
std::mutex g_pending_scripts_mutex;
std::set<std::string> g_pending_bundle_scripts;

// bo3-bundle: gates capture in the redundant-warning hook so we only collect
// during a bundle_load_ff invocation (not during normal BO3 FF loads).
std::atomic<bool> g_inside_bundle_load{false};

// bo3-bundle: hook on the "Redundant ... asset" Com_PrintWarning call site.
// The original is called by the asset linker when a same-named asset is
// being loaded -- the new asset IS still added to the linked list (head
// insert), but DB_EnumXAssets dedupes by name so the user-facing pool count
// doesn't change. We snag the asset name here so we can later call
// Scr_LoadScript on it (DB_FindXAssetHeader will resolve to DS4C's version
// at head).
//
// Signature matches the Com_PrintWarning variadic call:
//   void Com_PrintWarning(int channel, int subsystem, const char *fmt, ...);
// At the call site (after redundancy detected), the varargs are:
//   r9  = first vararg  = asset type name (e.g. "scriptparsetree")
//   stack[0x20] = second vararg = asset name (e.g. "scripts/zm/_zm.gsc")
//   stack[0x28] = third vararg  = source zone name
//   stack[0x30] = fourth vararg = loading zone name
//
// Windows x64 calling convention: rcx, rdx, r8, r9 first four; remainder
// on stack. For variadic functions, all named + unnamed args follow the
// same scheme. So:
//   arg0 = channel = ecx
//   arg1 = subsystem = edx
//   arg2 = fmt = r8
//   arg3 = type_name = r9
//   arg4 = asset_name = [rsp+0x20]
//   arg5 = zone1 = [rsp+0x28]
//   arg6 = zone2 = [rsp+0x30]
void __cdecl bundle_redundant_warning_hook(int channel, int subsystem,
                                            const char *fmt,
                                            const char *type_name,
                                            const char *asset_name,
                                            const char *zone1,
                                            const char *zone2) {
  if (g_inside_bundle_load.load(std::memory_order_relaxed) && fmt && type_name
      && asset_name && std::strcmp(type_name, "scriptparsetree") == 0) {
    std::lock_guard lock(g_pending_scripts_mutex);
    g_pending_bundle_scripts.insert(asset_name);
  }
  // Forward to original Com_PrintWarning so the log line still appears.
  // Static addr 0x142148F60; runtime offset relative to BlackOps3.exe base
  // handles ASLR.
  using fn_t = void(__cdecl *)(int, int, const char *, ...);
  auto *original = reinterpret_cast<fn_t>(
      reinterpret_cast<uintptr_t>(GetModuleHandleA("BlackOps3.exe"))
      + (0x142148F60ULL - 0x140000000ULL));
  if (original) {
    original(channel, subsystem, fmt, type_name, asset_name, zone1, zone2);
  }
}

utils::hook::detour setup_server_map_hook;
utils::hook::detour load_usermap_hook;

// bo3-bundle: observation detour on Scr_EndLoadScripts (0x1412C8020).
// Logs every native call so we can learn when BO3 fires autoexec.
utils::hook::detour scr_end_load_scripts_hook;
void scr_end_load_scripts_stub(int inst, int unused, char secondary) {
  const auto base = reinterpret_cast<uintptr_t>(
      GetModuleHandleA("BlackOps3.exe"));
  const auto flag_addr =
      base + 0x4d4e5cc + static_cast<uintptr_t>(inst) * 0x820;
  const auto flag_val = *reinterpret_cast<const uint8_t *>(flag_addr);
  printf("[ bo3-bundle ] Scr_EndLoadScripts(inst=%d, unused=%d, "
         "secondary=%d) -- flag at [base+0x4d4e5cc+inst*0x820]=%d\n",
         inst, unused, secondary, flag_val);
  scr_end_load_scripts_hook.invoke<void>(inst, unused, secondary);
}

// bo3-bundle: SEH-wrapped helpers for the dump_autoexec_list command
// (separate function because __try can't coexist with C++ try in same fn).
void dump_autoexec_list_seh(uintptr_t list_head, int byte_count, int stride) {
  __try {
    printf("[ bo3-bundle ] === raw dump @ 0x%llx (%d bytes) ===\n",
           static_cast<unsigned long long>(list_head), byte_count);
    const auto *bytes = reinterpret_cast<const uint8_t *>(list_head);
    for (int i = 0; i < byte_count; i += 16) {
      char line[100];
      char ascii[20] = {};
      int p = std::snprintf(line, sizeof(line), "+%04x: ", i);
      for (int j = 0; j < 16; j++) {
        p += std::snprintf(line + p, sizeof(line) - p, "%02x ",
                           bytes[i + j]);
        const auto c = bytes[i + j];
        ascii[j] = (c >= 0x20 && c < 0x7f) ? static_cast<char>(c) : '.';
      }
      printf("[ bo3-bundle ] %s | %s\n", line, ascii);
    }

    printf("[ bo3-bundle ] === structured (stride=0x%x) ===\n", stride);
    const int entries_to_show = byte_count / stride;
    for (int i = 0; i < entries_to_show && i < 30; i++) {
      const auto *e = bytes + i * stride;
      const auto flags = *reinterpret_cast<const uint32_t *>(e + 8);
      const auto index = *reinterpret_cast<const uint32_t *>(e + 0xC);
      printf("[ bo3-bundle ]   entry[%d] @ +0x%x: flags=0x%08x "
             "(&0x30000=%s) index=0x%x\n",
             i, i * stride, flags,
             (flags & 0x30000) ? "MATCH" : "no", index);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    printf("[ bo3-bundle ] *** access violation during dump ***\n");
  }
}

// bo3-bundle: hardware-breakpoint-based memory write trap on the per-instance
// autoexec flag at [image_base + 0x4d4e5cc + inst*0x820]. Uses DR0 + DR7
// (byte granularity, write-only filter). When BO3 writes to the flag, the
// CPU triggers SINGLE_STEP exception; our VEH captures the writing RIP.
// One-shot per arm: disables after 1 fire to avoid noise.
std::atomic<bool> g_hw_bp_armed{false};
std::atomic<uintptr_t> g_hw_bp_addr{0};
std::atomic<int> g_hw_bp_fires{0};

LONG WINAPI hw_bp_handler(EXCEPTION_POINTERS *ep) {
  if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP)
    return EXCEPTION_CONTINUE_SEARCH;
  // DR6 lower bits indicate which DR triggered. Bit 0 = DR0.
  const auto dr6 = ep->ContextRecord->Dr6;
  if (!(dr6 & 0x1ULL))
    return EXCEPTION_CONTINUE_SEARCH;

  const auto fires = g_hw_bp_fires.fetch_add(1, std::memory_order_relaxed);
  const auto rip = ep->ContextRecord->Rip;
  const auto base = reinterpret_cast<uintptr_t>(
      GetModuleHandleA("BlackOps3.exe"));
  const auto rip_off = rip > base ? (rip - base) : 0ULL;
  const auto static_rip = rip_off ? (rip_off + 0x140000000ULL) : 0ULL;
  const auto flag = *reinterpret_cast<uint8_t *>(
      g_hw_bp_addr.load(std::memory_order_relaxed));
  printf("[ bo3-bundle ] *** HW BP FIRE #%d *** RIP=0x%llx (BO3 +0x%llx, "
         "static=0x%llx) flag_now=%d\n",
         fires + 1, static_cast<unsigned long long>(rip),
         static_cast<unsigned long long>(rip_off),
         static_cast<unsigned long long>(static_rip), flag);

  // One-shot: disable DR0 after first fire to avoid spam during deeper
  // investigation of the writer function. We'll just need ONE RIP.
  ep->ContextRecord->Dr7 &= ~0x1ULL;          // clear L0
  ep->ContextRecord->Dr7 &= ~(0xFULL << 16);  // clear DR0 type/length
  ep->ContextRecord->Dr0 = 0;
  ep->ContextRecord->Dr6 &= ~0xFULL;          // clear status bits
  // Set RF so the instruction following the trap doesn't re-trip (though
  // with DR0 cleared, this isn't strictly needed)
  ep->ContextRecord->EFlags |= 0x10000;
  g_hw_bp_armed.store(false, std::memory_order_relaxed);
  return EXCEPTION_CONTINUE_EXECUTION;
}

void arm_hw_bp_on_flag(int inst) {
  static std::atomic<bool> veh_installed{false};
  if (!veh_installed.exchange(true)) {
    AddVectoredExceptionHandler(1, hw_bp_handler);
  }
  if (g_hw_bp_armed.load(std::memory_order_relaxed))
    return;  // already armed, wait for fire

  const auto base = reinterpret_cast<uintptr_t>(
      GetModuleHandleA("BlackOps3.exe"));
  const auto flag_addr =
      base + 0x4d4e5cc + static_cast<uintptr_t>(inst) * 0x820;
  g_hw_bp_addr.store(flag_addr, std::memory_order_relaxed);

  HANDLE thread = GetCurrentThread();
  CONTEXT ctx{};
  ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
  if (!GetThreadContext(thread, &ctx)) {
    printf("[ bo3-bundle ] arm_hw_bp: GetThreadContext failed err=%lu\n",
           GetLastError());
    return;
  }
  ctx.Dr0 = flag_addr;
  // DR7: enable DR0 locally, type=WRITE (01) at bits 16-17, length=1 byte
  // (00) at bits 18-19. Set LE (bit 8) for exact match.
  ctx.Dr7 &= ~(0xFULL << 16);  // clear DR0 config
  ctx.Dr7 &= ~0x3ULL;           // clear L0/G0
  ctx.Dr7 |= 0x1ULL;            // L0 = 1
  ctx.Dr7 |= (0x1ULL << 16);    // type = 01 (WRITE)
  ctx.Dr7 |= 0x100ULL;          // LE = 1
  ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
  if (!SetThreadContext(thread, &ctx)) {
    printf("[ bo3-bundle ] arm_hw_bp: SetThreadContext failed err=%lu\n",
           GetLastError());
    return;
  }
  g_hw_bp_armed.store(true, std::memory_order_relaxed);
  printf("[ bo3-bundle ] HW BP armed: DR0=0x%llx (WRITE 1 byte) for inst=%d\n",
         static_cast<unsigned long long>(flag_addr), inst);
}

// bo3-bundle: observation detour on Scr_BeginLoadScripts (0x1412C7DF0).
// Logs every native call + flag state. Paired with the End observation,
// reveals the actual Begin->End sequence and which calls set vs consume
// the flag at 0x4d4e5cc.
utils::hook::detour scr_begin_load_scripts_hook;
void scr_begin_load_scripts_stub(int inst, int user) {
  const auto base = reinterpret_cast<uintptr_t>(
      GetModuleHandleA("BlackOps3.exe"));
  const auto flag_addr =
      base + 0x4d4e5cc + static_cast<uintptr_t>(inst) * 0x820;
  const auto before = *reinterpret_cast<const uint8_t *>(flag_addr);
  printf("[ bo3-bundle ] Scr_BeginLoadScripts(inst=%d, user=%d) "
         "ENTER -- flag before=%d\n",
         inst, user, before);
  scr_begin_load_scripts_hook.invoke<void>(inst, user);
  const auto after = *reinterpret_cast<const uint8_t *>(flag_addr);
  printf("[ bo3-bundle ] Scr_BeginLoadScripts(inst=%d) EXIT -- "
         "flag after=%d\n",
         inst, after);
  // Arm hardware breakpoint (DR0) on the flag's exact byte after Begin sets
  // it to 1. The next WRITE to this byte (probably the autoexec dispatcher
  // clearing the flag) will trip a SINGLE_STEP exception that captures the
  // writing RIP.
  if (after == 1) {
    arm_hw_bp_on_flag(inst);
  }
}

// bo3-bundle: observation detour on Scr_LoadScript (0x1412C83F0).
// Most chatty since BO3 calls this many times. Filtering:
//   - ALWAYS log calls for our test/extract scripts (substring match)
//   - log if the per-inst flag at 0x4d4e5cc transitions (entry != exit)
// Goal: find what flag-transition happens during Solo's mod scripts vs
// our custom scripts, identifying the autoexec dispatch point.
utils::hook::detour scr_load_script_hook;
unsigned int scr_load_script_stub(int inst, const char *filename) {
  const auto base = reinterpret_cast<uintptr_t>(
      GetModuleHandleA("BlackOps3.exe"));
  const auto flag_addr =
      base + 0x4d4e5cc + static_cast<uintptr_t>(inst) * 0x820;
  const auto before = *reinterpret_cast<const uint8_t *>(flag_addr);

  const bool always_log = filename &&
      (std::strstr(filename, "_ds4c") != nullptr ||
       std::strstr(filename, "_test_autoexec") != nullptr);

  const auto result =
      scr_load_script_hook.invoke<unsigned int>(inst, filename);
  const auto after = *reinterpret_cast<const uint8_t *>(flag_addr);

  // Log EVERY call temporarily to capture the full pattern. Will be noisy
  // but informative. Revert to filtered after one cycle of analysis.
  printf("[ bo3-bundle ] Scr_LoadScript(inst=%d, \"%s\") flag %d->%d, "
         "ret=%u%s\n",
         inst, filename ? filename : "(null)", before, after, result,
         before != after ? " [FLAG_TRANSITION]" : "");
  (void)always_log;
  return result;
}

static const std::unordered_map<std::string, std::string> dlc_links = {
    {"zm_zod", "https://forum.ezz.lol/topic/6/bo3-dlc"},
    {"zm_castle", "https://forum.ezz.lol/topic/6/bo3-dlc"},
    {"zm_island", "https://forum.ezz.lol/topic/6/bo3-dlc"},
    {"zm_stalingrad", "https://forum.ezz.lol/topic/6/bo3-dlc"},
    {"zm_genesis", "https://forum.ezz.lol/topic/6/bo3-dlc"},
    {"zm_cosmodrome", "https://forum.ezz.lol/topic/6/bo3-dlc"},
    {"zm_theater", "https://forum.ezz.lol/topic/6/bo3-dlc"},
    {"zm_moon", "https://forum.ezz.lol/topic/6/bo3-dlc"},
    {"zm_prototype", "https://forum.ezz.lol/topic/6/bo3-dlc"},
    {"zm_tomb", "https://forum.ezz.lol/topic/6/bo3-dlc"},
    {"zm_temple", "https://forum.ezz.lol/topic/6/bo3-dlc"},
    {"zm_sumpf", "https://forum.ezz.lol/topic/6/bo3-dlc"},
    {"zm_factory", "https://forum.ezz.lol/topic/6/bo3-dlc"},
    {"zm_asylum", "https://forum.ezz.lol/topic/6/bo3-dlc"}};
std::mutex dlc_mutex;
std::condition_variable dlc_cv;
std::string pending_dlc_map;
std::atomic<bool> dlc_thread_shutdown{false};
std::thread dlc_popup_thread_obj;

void dlc_popup_thread_func() {
  while (true) {
    std::unique_lock lock(dlc_mutex);
    dlc_cv.wait_for(lock, std::chrono::milliseconds(200), [] {
      return dlc_thread_shutdown.load() || !pending_dlc_map.empty();
    });
    if (dlc_thread_shutdown.load())
      break;
    if (pending_dlc_map.empty())
      continue;
    std::string map = std::move(pending_dlc_map);
    pending_dlc_map.clear();
    lock.unlock();

    const auto it = dlc_links.find(map);
    if (it != dlc_links.end()) {
      const std::string link = it->second;
      const std::string map_copy = map;
      scheduler::once(
          [map_copy, link] {
            game::UI_OpenErrorPopupWithMessage(
                0, game::ERROR_UI,
                utils::string::va(
                    "Missing DLC map: %s\n\nOpening download page...\n%s",
                    map_copy.c_str(), link.c_str()));
          },
          scheduler::main);
      ShellExecuteA(nullptr, "open", link.c_str(), nullptr, nullptr,
                    SW_SHOWNORMAL);
    }
  }
}

void queue_dlc_popup(const std::string &mapname) {
  std::lock_guard lock(dlc_mutex);
  pending_dlc_map = mapname;
  dlc_cv.notify_one();
}

bool has_mod(const std::string &pub_id) {
  for (unsigned int i = 0; i < *game::modsCount; ++i) {
    const auto &mod_data = game::modsPool[i];
    if (mod_data.publisherId == pub_id || mod_data.folderName == pub_id) {
      return true;
    }
  }

  return false;
}

std::string resolve_mod_workshop_id(const std::string &mod_name) {
  for (unsigned int i = 0; i < *game::modsCount; ++i) {
    const auto &mod_data = game::modsPool[i];
    if (mod_data.folderName == mod_name &&
        utils::string::is_numeric(mod_data.publisherId)) {
      return mod_data.publisherId;
    }
  }

  std::error_code ec;
  std::filesystem::path mods_dir("mods");
  if (std::filesystem::exists(mods_dir, ec)) {
    for (const auto &entry :
         std::filesystem::directory_iterator(mods_dir, ec)) {
      if (!entry.is_directory(ec))
        continue;

      auto ws_json = entry.path() / "zone" / "workshop.json";
      if (!std::filesystem::exists(ws_json, ec))
        continue;

      const auto json_str = utils::io::read_file(ws_json.string());
      if (json_str.empty())
        continue;

      rapidjson::Document doc;
      if (doc.Parse(json_str.c_str()).HasParseError() || !doc.IsObject())
        continue;

      auto folder_it = doc.FindMember("FolderName");
      if (folder_it != doc.MemberEnd() && folder_it->value.IsString()) {
        if (std::string(folder_it->value.GetString()) == mod_name) {
          auto pub_it = doc.FindMember("PublishedFileId");
          if (pub_it != doc.MemberEnd() && pub_it->value.IsString()) {
            std::string pfid = pub_it->value.GetString();
            if (utils::string::is_numeric(pfid.data()))
              return pfid;
          }
          auto pubid_it = doc.FindMember("PublisherID");
          if (pubid_it != doc.MemberEnd() && pubid_it->value.IsString()) {
            std::string pid = pubid_it->value.GetString();
            if (utils::string::is_numeric(pid.data()))
              return pid;
          }
        }
      }
    }
  }

  return {};
}

void load_usermap_mod_if_needed() {
  if (!game::isModLoaded()) {
    game::loadMod(game::LOCAL_CLIENT_0, "usermaps", false);
  }
}

uint32_t get_xzone_index_by_name(const char *zone_name) {
  for (uint32_t zoneIdx = 0; zoneIdx < *(game::g_zoneCount.get()); zoneIdx++) {
    game::XZoneName *zoneInfo =
        reinterpret_cast<game::XZoneName *>(&game::g_zoneNames[zoneIdx]);

    if (std::strcmp(zoneInfo->name, zone_name) == 0) {
      return zoneIdx;
    }
  }

  return 0xFFFFFFFF; // Invalid index
}

bool unload_xzone_by_name(const char *zone_name, bool createDefault,
                          bool suppressSync) {
  uint32_t zoneIdx = get_xzone_index_by_name(zone_name);
  if (zoneIdx != 0xFFFFFFFF) {
    game::DB_UnloadXZone(zoneIdx, createDefault, suppressSync ? 1 : 0);
    return true;
  }
  return false; // Zone not found
}

void clear_loaded_usermap() {
  // Set first byte of each to null, terminating string immediately - sets each
  // to an empty string
  *(game::usermap_publisher_id.get()) = 0;
  *(game::usermap_title.get()) = 0;
  *(game::internal_usermap_id.get()) = 0; // e.g. zm_*
}

void setup_server_map_stub(int localClientNum, const char *map,
                           const char *gametype) {
  const char *loaded_mod_id = game::getPublisherIdFromLoadedMod();
  const bool is_usermap =
      utils::string::is_numeric(map) || !get_usermap_publisher_id(map).empty();
  const bool is_mod_loaded = std::strlen(loaded_mod_id) > 0;
  const bool is_usermaps_mod_loaded =
      is_mod_loaded && std::strcmp(loaded_mod_id, "usermaps") == 0;

  if (is_usermap) {
    if (!is_mod_loaded) {
      game::loadMod(game::LOCAL_CLIENT_0, "usermaps", false);
    }
  } else {
    clear_loaded_usermap();

    if (is_usermaps_mod_loaded) {
      game::loadMod(game::LOCAL_CLIENT_0, "", false);
    }

    unload_xzone_by_name("zm_levelcommon", false, false);
  }

  setup_server_map_hook.invoke(localClientNum, map, gametype);
}

void load_workshop_data(game::workshop_data &item) {
  const auto base_path = item.absolutePathZoneFiles;
  const auto path = utils::string::va("%s/workshop.json", base_path);
  const auto json_str = utils::io::read_file(path);

  if (json_str.empty()) {
    printf("[ Workshop ] workshop.json has not been found in folder:\n%s\n",
           path);
    return;
  }

  rapidjson::Document doc;
  const rapidjson::ParseResult parse_result = doc.Parse(json_str);

  if (parse_result.IsError() || !doc.IsObject()) {
    printf("[ Workshop ] Unable to parse workshop.json from folder:\n%s\n",
           path);
    return;
  }

  if (!doc.HasMember("Title") || !doc.HasMember("Description") ||
      !doc.HasMember("FolderName") || !doc.HasMember("PublisherID")) {
    printf("[ Workshop ] workshop.json is invalid:\n%s\n", path);
    return;
  }

  utils::string::copy(item.title, doc["Title"].GetString());
  utils::string::copy(item.description, doc["Description"].GetString());
  utils::string::copy(item.folderName, doc["FolderName"].GetString());
  utils::string::copy(item.publisherId, doc["PublisherID"].GetString());
  item.publisherIdInteger = std::strtoul(item.publisherId, nullptr, 10);
}

void populate_workshop_paths(game::workshop_data &item,
                             const std::filesystem::path &content_folder,
                             const game::workshop_type type) {
  std::memset(&item, 0, sizeof(item));

  const auto zone_path = content_folder / "zone";
  const auto relative_zone_path =
      std::filesystem::path(type == game::WORKSHOP_MOD ? "mods" : "usermaps") /
      content_folder.filename() / "zone";

  utils::string::copy(item.contentPathToZoneFiles,
                      relative_zone_path.generic_string().c_str());
  utils::string::copy(item.absolutePathContentFolder,
                      content_folder.generic_string().c_str());
  utils::string::copy(item.absolutePathZoneFiles,
                      zone_path.generic_string().c_str());
  item.unk = 1;
  item.unk2 = 0;
  item.unk3 = 0;
  item.unk4 = 0;
  item.type = type;
}

void supplement_mods_from_disk() {
  if (*game::modsCount != 0) {
    return;
  }

  std::error_code ec;
  const auto mods_dir = std::filesystem::current_path() / "mods";
  if (!std::filesystem::exists(mods_dir, ec)) {
    return;
  }

  unsigned int count = 0;
  for (const auto &entry : std::filesystem::directory_iterator(mods_dir, ec)) {
    if (ec || !entry.is_directory(ec)) {
      continue;
    }

    const auto zone_dir = entry.path() / "zone";
    const auto workshop_json = zone_dir / "workshop.json";
    if (!std::filesystem::exists(zone_dir, ec) ||
        !std::filesystem::exists(workshop_json, ec)) {
      continue;
    }

    auto &mod_data = game::modsPool[count];
    populate_workshop_paths(mod_data, entry.path(), game::WORKSHOP_MOD);
    load_workshop_data(mod_data);
    ++count;
  }

  if (count) {
    *game::modsCount = count;
    printf("[ Workshop ] Supplemented %u mods from disk fallback\n", count);
  }
}

void load_usermap_content_stub(void *usermaps_count, int type) {
  utils::hook::invoke<void>(game::select(0x1420D6430, 0x1404E2360),
                            usermaps_count, type);

  for (unsigned int i = 0; i < *game::usermapsCount; ++i) {
    auto &usermap_data = game::usermapsPool[i];

    if (std::strcmp(usermap_data.folderName, usermap_data.title) != 0) {
      continue;
    }

    load_workshop_data(usermap_data);
  }
}

void load_mod_content_stub(void *mods_count, int type) {
  utils::hook::invoke<void>(game::select(0x1420D6430, 0x1404E2360), mods_count,
                            type);
  supplement_mods_from_disk();

  for (unsigned int i = 0; i < *game::modsCount; ++i) {
    auto &mod_data = game::modsPool[i];

    if (std::strcmp(mod_data.folderName, mod_data.title) != 0) {
      continue;
    }

    load_workshop_data(mod_data);
  }
}

game::workshop_data *load_usermap_stub(const char *map_arg) {
  std::string pub_id = map_arg;
  if (!utils::string::is_numeric(map_arg)) {
    pub_id = get_usermap_publisher_id(map_arg);
  }

  return load_usermap_hook.invoke<game::workshop_data *>(pub_id.data());
}

bool has_workshop_item_stub(int type, const char *map, int a3) {
  std::string pub_id = map;
  if (!utils::string::is_numeric(map)) {
    pub_id = get_usermap_publisher_id(map);
  }

  return utils::hook::invoke<bool>(0x1420D6380_g, type, pub_id.data(), a3);
}

const char *va_mods_path_stub(const char *fmt, const char *root_dir,
                              const char *mods_dir, const char *dir_name) {
  const auto original_path =
      utils::string::va(fmt, root_dir, mods_dir, dir_name);

  const char *result = utils::io::directory_exists(original_path)
      ? original_path
      : utils::string::va("%s/%s/%s", root_dir, mods_dir, dir_name);
  printf("[fs-trace] va_mods_path(fmt=\"%s\", root=\"%s\", mods=\"%s\", "
         "name=\"%s\") -> \"%s\"\n",
         fmt ? fmt : "(null)", root_dir ? root_dir : "(null)",
         mods_dir ? mods_dir : "(null)", dir_name ? dir_name : "(null)",
         result ? result : "(null)");
  return result;
}

const char *va_user_content_path_stub(const char *fmt, const char *root_dir,
                                      const char *user_content_dir) {
  const auto original_path = utils::string::va(fmt, root_dir, user_content_dir);

  const char *result = utils::io::directory_exists(original_path)
      ? original_path
      : utils::string::va("%s/%s", root_dir, user_content_dir);
  printf("[fs-trace] va_user_content_path(fmt=\"%s\", root=\"%s\", "
         "user_content=\"%s\") -> \"%s\"\n",
         fmt ? fmt : "(null)", root_dir ? root_dir : "(null)",
         user_content_dir ? user_content_dir : "(null)",
         result ? result : "(null)");
  return result;
}
} // namespace

void drain_pending_bundle_activations(int inst_raw) {
  if (!g_auto_activate_scripts.load(std::memory_order_relaxed))
    return;

  const auto inst = inst_raw == 1 ? game::SCRIPTINSTANCE_CLIENT
                                  : game::SCRIPTINSTANCE_SERVER;
  const bool wants_client = (inst == game::SCRIPTINSTANCE_CLIENT);

  // Drain only DS4C-collision scripts captured by the redundant-warning
  // hook during a prior bundle_load_ff.
  std::lock_guard lock(g_pending_scripts_mutex);
  if (g_pending_bundle_scripts.empty())
    return;

  // Get current map to filter out cross-map scripts. Scripts like
  // scripts/zm/zm_cosmodrome_ffotd.gsc have hard imports on cosmodrome-
  // specific files; activating them during a SoE (zm_zod) load triggers
  // fatal "Error linking script" → kicks to menu.
  const std::string mapname = game::get_dvar_string("mapname");
  std::string current_map_basename;
  if (mapname.size() > 3 && mapname.substr(0, 3) == "zm_") {
    current_map_basename = mapname.substr(3);
  }

  int activated = 0;
  int skipped_cross_map = 0;
  for (const auto &full_name : g_pending_bundle_scripts) {
    // Cross-map filter: skip scripts/zm/zm_<otherMap>_*.gsc where
    // <otherMap> != current_map_basename. Keep generic scripts like
    // scripts/zm/_zm_*.gsc, scripts/custom/*.gsc, scripts/shared/*.gsc.
    if (!current_map_basename.empty() &&
        full_name.rfind("scripts/zm/zm_", 0) == 0) {
      const size_t map_start = std::strlen("scripts/zm/zm_");
      const size_t map_end = full_name.find('_', map_start);
      if (map_end != std::string::npos && map_end > map_start) {
        const auto script_map =
            full_name.substr(map_start, map_end - map_start);
        if (script_map != current_map_basename) {
          skipped_cross_map++;
          continue;
        }
      }
    }

    bool is_csc = false;
    std::string base = full_name;
    if (base.size() > 4) {
      auto suffix = base.substr(base.size() - 4);
      if (suffix == ".csc")
        is_csc = true;
      if (suffix == ".gsc" || suffix == ".csc")
        base = base.substr(0, base.size() - 4);
    }
    if (wants_client != is_csc)
      continue;
    game::Scr_LoadScript(inst, base.c_str());
    activated++;
  }

  if (activated > 0 || skipped_cross_map > 0) {
    printf("[ bo3-bundle ] auto-activate during begin_load_scripts (map=%s): "
           "%d %s activated, %d skipped (cross-map)\n",
           mapname.c_str(), activated, inst_raw == 1 ? "CSC" : "GSC",
           skipped_cross_map);
  }
}

std::string get_mod_resized_name() {
  const std::string loaded_mod_id = game::getPublisherIdFromLoadedMod();

  if (loaded_mod_id == "usermaps" || loaded_mod_id.empty()) {
    return loaded_mod_id;
  }

  std::string mod_name = loaded_mod_id;

  for (unsigned int i = 0; i < *game::modsCount; ++i) {
    const auto &mod_data = game::modsPool[i];

    if (mod_data.publisherId == loaded_mod_id) {
      mod_name = mod_data.title;
      break;
    }
  }

  if (mod_name.size() > 31) {
    mod_name.resize(31);
  }

  return mod_name;
}

std::string get_usermap_publisher_id(const std::string &zone_name) {
  for (unsigned int i = 0; i < *game::usermapsCount; ++i) {
    const auto &usermap_data = game::usermapsPool[i];
    if (usermap_data.folderName == zone_name) {
      if (!utils::string::is_numeric(usermap_data.publisherId)) {
        printf("[ Workshop ] WARNING: The publisherId is not numerical you "
               "might have set your usermap folder incorrectly!\n%s\n",
               usermap_data.absolutePathZoneFiles);
      }

      return usermap_data.publisherId;
    }
  }

  return {};
}

int get_workshop_retry_attempts() {
  const int val = game::get_dvar_int("workshop_retry_attempts");
  if (val < 1)
    return 1;
  if (val > 1000)
    return 1000;
  return val;
}

std::string get_mod_publisher_id() {
  const std::string loaded_mod_id = game::getPublisherIdFromLoadedMod();

  if (loaded_mod_id == "usermaps" || loaded_mod_id.empty()) {
    return loaded_mod_id;
  }

  if (!utils::string::is_numeric(loaded_mod_id)) {
    printf("[ Workshop ] WARNING: The publisherId: %s, is not numerical you "
           "might have set your mod folder incorrectly!\n",
           loaded_mod_id.data());
  }

  return loaded_mod_id;
}

bool is_dlc_map(const std::string &mapname) {
  return mapname == "zm_zod" || mapname == "zm_castle" ||
         mapname == "zm_island" || mapname == "zm_stalingrad" ||
         mapname == "zm_genesis" || mapname == "zm_cosmodrome" ||
         mapname == "zm_theater" || mapname == "zm_moon" ||
         mapname == "zm_prototype" || mapname == "zm_tomb" ||
         mapname == "zm_temple" || mapname == "zm_sumpf" ||
         mapname == "zm_factory" || mapname == "zm_asylum";
}

std::atomic<bool> downloading_workshop_item{false};
std::atomic<bool> launcher_downloading{false};

bool is_any_download_active() {
  return downloading_workshop_item.load() || launcher_downloading.load() ||
         fastdl::is_downloading();
}

static std::mutex reconnect_mutex;
static std::string pending_mod_reconnect_address;
static std::string pending_download_reconnect_address;

void set_pending_mod_reconnect(const std::string &address) {
  std::lock_guard lock(reconnect_mutex);
  pending_mod_reconnect_address = address;
}

std::string get_pending_mod_reconnect() {
  std::lock_guard lock(reconnect_mutex);
  auto addr = std::move(pending_mod_reconnect_address);
  pending_mod_reconnect_address.clear();
  return addr;
}

void set_pending_download_reconnect(const std::string &address) {
  std::lock_guard lock(reconnect_mutex);
  // Don't overwrite if a download is already active, preserve the original
  // server
  if (pending_download_reconnect_address.empty() || !is_any_download_active()) {
    pending_download_reconnect_address = address;
  }
}

std::string get_pending_download_reconnect() {
  std::lock_guard lock(reconnect_mutex);
  auto addr = std::move(pending_download_reconnect_address);
  pending_download_reconnect_address.clear();
  return addr;
}

std::uint64_t compute_folder_size_bytes(const std::filesystem::path &folder) {
  std::error_code ec;
  if (!std::filesystem::exists(folder, ec))
    return 0;
  std::uint64_t total = 0;
  for (const auto &entry : std::filesystem::recursive_directory_iterator(
           folder, std::filesystem::directory_options::skip_permission_denied,
           ec)) {
    if (ec)
      break;
    if (!entry.is_regular_file(ec))
      continue;
    total += static_cast<std::uint64_t>(
        std::filesystem::file_size(entry.path(), ec));
    if (ec)
      break;
  }
  return total;
}

std::string human_readable_size(std::uint64_t bytes) {
  const char *suffixes[] = {"B", "KB", "MB", "GB", "TB"};
  double value = static_cast<double>(bytes);
  int idx = 0;
  while (value >= 1024.0 && idx < 4) {
    value /= 1024.0;
    ++idx;
  }
  char buf[64]{};
  std::snprintf(buf, sizeof(buf), "%.2f %s", value, suffixes[idx]);
  return buf;
}

std::uint64_t parse_human_size_to_bytes(const std::string &text) {
  std::smatch m;
  std::regex re(R"((\d+(?:\.\d+)?)\s*(B|KB|MB|GB|TB))", std::regex::icase);
  if (!std::regex_search(text, m, re) || m.size() < 3)
    return 0;
  const double value = std::stod(m[1].str());
  std::string unit = m[2].str();
  for (auto &c : unit)
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

  double mul = 1.0;
  if (unit == "KB")
    mul = 1024.0;
  else if (unit == "MB")
    mul = 1024.0 * 1024.0;
  else if (unit == "GB")
    mul = 1024.0 * 1024.0 * 1024.0;
  else if (unit == "TB")
    mul = 1024.0 * 1024.0 * 1024.0 * 1024.0;

  const auto bytes = value * mul;
  if (bytes <= 0.0)
    return 0;
  return static_cast<std::uint64_t>(bytes);
}

std::uint64_t scrape_workshop_file_size_bytes(const std::string &workshop_id) {
  try {
    utils::http::headers h;
    h["User-Agent"] =
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36";
    h["Accept"] = "text/html";
    h["Accept-Language"] = "en-US,en;q=0.9";
    h["Referer"] = "https://steamcommunity.com/app/311210/workshop/";

    const auto url = "https://steamcommunity.com/sharedfiles/filedetails/?id=" +
                     workshop_id + "&searchtext=";
    const auto resp = utils::http::get_data(url, h, {}, 2);
    if (!resp || resp->empty())
      return 0;

    const std::string &html = *resp;

    {
      std::regex re(
          R"(detailsStatRight[^>]*>\s*([\d,\.]+\s*(?:B|KB|MB|GB|TB))\s*<)",
          std::regex::icase);
      std::smatch m;
      if (std::regex_search(html, m, re) && m.size() >= 2) {
        std::string size_text = m[1].str();
        size_text.erase(std::remove(size_text.begin(), size_text.end(), ','),
                        size_text.end());
        const auto bytes = parse_human_size_to_bytes(size_text);
        if (bytes > 0)
          return bytes;
      }
    }
    {
      std::regex re(R"(File\s*Size\s*<\/div>\s*<div[^>]*>([^<]+)<)",
                    std::regex::icase);
      std::smatch m;
      if (std::regex_search(html, m, re) && m.size() >= 2) {
        std::string size_text = m[1].str();
        size_text.erase(std::remove(size_text.begin(), size_text.end(), ','),
                        size_text.end());
        const auto bytes = parse_human_size_to_bytes(size_text);
        if (bytes > 0)
          return bytes;
      }
    }
    {
      std::regex re(R"(File\s*Size[^\d]*(\d+(?:[,.]\d+)?)\s*(B|KB|MB|GB|TB))",
                    std::regex::icase);
      std::smatch m;
      if (std::regex_search(html, m, re) && m.size() >= 3) {
        std::string num = m[1].str();
        num.erase(std::remove(num.begin(), num.end(), ','), num.end());
        const auto bytes = parse_human_size_to_bytes(num + " " + m[2].str());
        if (bytes > 0)
          return bytes;
      }
    }
    return 0;
  } catch (...) {
    return 0;
  }
}

workshop_info get_steam_workshop_info(const std::string &workshop_id) {
  workshop_info info{};
  if (workshop_id.empty())
    return info;
  try {
    const std::string body = "itemcount=1&publishedfileids[0]=" + workshop_id;
    const auto resp = utils::http::post_data(
        "https://api.steampowered.com/ISteamRemoteStorage/"
        "GetPublishedFileDetails/v1/",
        body, 10);
    if (!resp || resp->empty())
      return info;

    rapidjson::Document doc;
    if (doc.Parse(resp->c_str()).HasParseError() || !doc.IsObject())
      return info;
    auto resp_it = doc.FindMember("response");
    if (resp_it == doc.MemberEnd() || !resp_it->value.IsObject())
      return info;
    auto details_it = resp_it->value.FindMember("publishedfiledetails");
    if (details_it == resp_it->value.MemberEnd() ||
        !details_it->value.IsArray() || details_it->value.Empty())
      return info;
    const auto &first = details_it->value[0];
    if (!first.IsObject())
      return info;

    auto title_it = first.FindMember("title");
    if (title_it != first.MemberEnd() && title_it->value.IsString())
      info.title = title_it->value.GetString();

    auto size_it = first.FindMember("file_size");
    if (size_it != first.MemberEnd()) {
      if (size_it->value.IsUint64())
        info.file_size = size_it->value.GetUint64();
      else if (size_it->value.IsInt64())
        info.file_size = static_cast<std::uint64_t>(size_it->value.GetInt64());
      else if (size_it->value.IsUint())
        info.file_size = size_it->value.GetUint();
      else if (size_it->value.IsInt())
        info.file_size = static_cast<std::uint64_t>(size_it->value.GetInt());
      else if (size_it->value.IsString())
        info.file_size = static_cast<std::uint64_t>(
            std::strtoull(size_it->value.GetString(), nullptr, 10));
      else if (size_it->value.IsDouble())
        info.file_size = static_cast<std::uint64_t>(size_it->value.GetDouble());
      else if (size_it->value.IsNumber())
        info.file_size = static_cast<std::uint64_t>(size_it->value.GetDouble());
    }

    if (info.file_size == 0) {
      const auto scraped = scrape_workshop_file_size_bytes(workshop_id);
      if (scraped > 0)
        info.file_size = scraped;
    }
  } catch (...) {
    const auto scraped = scrape_workshop_file_size_bytes(workshop_id);
    if (scraped > 0)
      info.file_size = scraped;
  }
  return info;
}

bool check_valid_usermap_id(const std::string &mapname,
                            const std::string &pub_id,
                            const std::string &workshop_id,
                            const std::string &base_url) {
  if (!game::DB_FileExists(mapname.data(), 0) && pub_id.empty()) {
    if (is_dlc_map(mapname.data())) {
      queue_dlc_popup(mapname);
      return false;
    }

    if (downloading_workshop_item.load() || launcher_downloading.load() ||
        fastdl::is_downloading()) {
      scheduler::once(
          [] {
            game::UI_OpenErrorPopupWithMessage(
                0, game::ERROR_UI,
                "You are already downloading a map in the background. You can "
                "download only one item at a time.");
          },
          scheduler::main);
      return false;
    }

    if (!base_url.empty()) {
      fastdl::download_context context{};
      context.mapname = mapname;
      context.pub_id = workshop_id.empty() ? mapname : workshop_id;
      context.map_path = "./usermaps/" + mapname;
      context.base_url = base_url;
      context.success_callback = []() {
        scheduler::once([] { game::reloadUserContent(); }, scheduler::main);
      };
      printf("[ Workshop ] Server has FastDL, attempting download for %s from "
             "%s\n",
             mapname.data(), base_url.data());
      fastdl::start_map_download(context);
      return false;
    }

    if (utils::string::is_numeric(mapname.data())) {
      const std::string id_copy = mapname;
      const auto ws_info = get_steam_workshop_info(id_copy);
      std::string confirm_msg =
          utils::string::va("Usermap '%s' was not found.\n", id_copy.c_str());
      if (!ws_info.title.empty())
        confirm_msg += "Title: " + ws_info.title + "\n";
      if (ws_info.file_size > 0)
        confirm_msg += "Size: " + human_readable_size(ws_info.file_size) + "\n";
      confirm_msg += "\nDo you want to download it from the Steam Workshop?";
      download_overlay::show_confirmation(
          "Download Map?", confirm_msg, [id_copy] {
            download_thread = utils::thread::create_named_thread(
                "workshop_download", steamcmd::initialize_download, id_copy,
                std::string("Map"));
            download_thread.detach();
          });
    } else if (!workshop_id.empty() &&
               utils::string::is_numeric(workshop_id.data())) {
      const std::string id_copy = workshop_id;
      const std::string name_copy = mapname;
      const auto ws_info = get_steam_workshop_info(id_copy);
      std::string confirm_msg =
          utils::string::va("Usermap '%s' was not found.\n", name_copy.c_str());
      if (!ws_info.title.empty())
        confirm_msg += "Title: " + ws_info.title + "\n";
      if (ws_info.file_size > 0)
        confirm_msg += "Size: " + human_readable_size(ws_info.file_size) + "\n";
      confirm_msg += "\nDo you want to download it from the Steam Workshop?";
      download_overlay::show_confirmation(
          "Download Map?", confirm_msg, [id_copy] {
            download_thread = utils::thread::create_named_thread(
                "workshop_download", steamcmd::initialize_download, id_copy,
                std::string("Map"));
            download_thread.detach();
          });
    } else {
      const std::string name_copy = mapname;
      scheduler::once(
          [name_copy] {
            game::UI_OpenErrorPopupWithMessage(
                0, game::ERROR_UI,
                utils::string::va(
                    "Missing usermap: %s\n\nThis server did not provide FastDL "
                    "and did not set workshop_id.\n\nSubscribe on Steam "
                    "Workshop, or ask the server to set sv_wwwBaseURL or "
                    "workshop_id.",
                    name_copy.c_str()));
          },
          scheduler::main);
    }
    return false;
  }
  return true;
}

bool check_valid_mod_id(const std::string &mod,
                        const std::string &workshop_id) {
  if (mod.empty() || mod == "usermaps") {
    return true;
  }

  if (!has_mod(mod)) {
    if (downloading_workshop_item.load() || launcher_downloading.load()) {
      scheduler::once(
          [] {
            game::UI_OpenErrorPopupWithMessage(
                0, game::ERROR_UI,
                "You are already downloading a mod in the background. You can "
                "download only one item at a time.");
          },
          scheduler::main);
      return false;
    }

    if (utils::string::is_numeric(mod.data())) {
      const std::string id_copy = mod;
      const auto ws_info = get_steam_workshop_info(id_copy);
      std::string confirm_msg =
          utils::string::va("Mod '%s' was not found.\n", id_copy.c_str());
      if (!ws_info.title.empty())
        confirm_msg += "Title: " + ws_info.title + "\n";
      if (ws_info.file_size > 0)
        confirm_msg += "Size: " + human_readable_size(ws_info.file_size) + "\n";
      confirm_msg += "\nDo you want to download it from the Steam Workshop?";
      download_overlay::show_confirmation(
          "Download Mod?", confirm_msg, [id_copy] {
            download_thread = utils::thread::create_named_thread(
                "workshop_download", steamcmd::initialize_download, id_copy,
                std::string("Mod"));
            download_thread.detach();
          });
    } else if (!workshop_id.empty() &&
               utils::string::is_numeric(workshop_id.data())) {
      const std::string id_copy = workshop_id;
      const std::string name_copy = mod;
      const auto ws_info = get_steam_workshop_info(id_copy);
      std::string confirm_msg =
          utils::string::va("Mod '%s' was not found.\n", name_copy.c_str());
      if (!ws_info.title.empty())
        confirm_msg += "Title: " + ws_info.title + "\n";
      if (ws_info.file_size > 0)
        confirm_msg += "Size: " + human_readable_size(ws_info.file_size) + "\n";
      confirm_msg += "\nDo you want to download it from the Steam Workshop?";
      download_overlay::show_confirmation(
          "Download Mod?", confirm_msg, [id_copy] {
            download_thread = utils::thread::create_named_thread(
                "workshop_download", steamcmd::initialize_download, id_copy,
                std::string("Mod"));
            download_thread.detach();
          });
    } else {
      std::string resolved_id = resolve_mod_workshop_id(mod);
      if (!resolved_id.empty()) {
        const std::string name_copy = mod;
        const auto ws_info = get_steam_workshop_info(resolved_id);
        std::string confirm_msg = utils::string::va(
            "Mod '%s' was not found.\nResolved workshop ID: %s\n",
            name_copy.c_str(), resolved_id.c_str());
        if (!ws_info.title.empty())
          confirm_msg += "Title: " + ws_info.title + "\n";
        if (ws_info.file_size > 0)
          confirm_msg +=
              "Size: " + human_readable_size(ws_info.file_size) + "\n";
        confirm_msg += "\nDo you want to download it now?";
        download_overlay::show_confirmation(
            "Download Mod?", confirm_msg, [resolved_id] {
              download_thread = utils::thread::create_named_thread(
                  "workshop_download", steamcmd::initialize_download,
                  resolved_id, std::string("Mod"));
              download_thread.detach();
            });
      } else {
        const std::string name_copy = mod;
        scheduler::once(
            [name_copy] {
              game::UI_OpenErrorPopupWithMessage(
                  0, game::ERROR_UI,
                  utils::string::va(
                      "Could not download: folder name is not numeric and "
                      "'workshop_id' dvar is empty.\nMod: %s\nSet workshop_id "
                      "or subscribe on Steam Workshop.",
                      name_copy.c_str()));
            },
            scheduler::main);
      }
    }
    return false;
  }

  return true;
}

bool mod_load_requires_fs_reinitialization(const std::string &mod_name) {
  return !mod_name.empty() && mod_name != "usermaps";
}

bool mod_switch_requires_fs_reinitialization(const std::string &current_mod,
                                             const std::string &new_mod) {
  return mod_load_requires_fs_reinitialization(current_mod) ||
         mod_load_requires_fs_reinitialization(new_mod);
}

void setup_same_mod_as_host(const std::string &usermap,
                            const std::string &mod) {
  const std::string loaded_mod = game::getPublisherIdFromLoadedMod();
  if (loaded_mod != mod) {
    if (!usermap.empty() || !mod.empty()) {
      bool fs_reinit_required =
          mod_switch_requires_fs_reinitialization(loaded_mod, mod);
      game::loadMod(game::LOCAL_CLIENT_0, mod.data(), fs_reinit_required);
      if (fs_reinit_required) {
        while (game::isModLoading(game::LOCAL_CLIENT_0)) {
          std::this_thread::sleep_for(100ms);
        }
      }
    } else if (game::isModLoaded()) {
      bool fs_reinit_required =
          mod_switch_requires_fs_reinitialization(loaded_mod, "");
      game::loadMod(game::LOCAL_CLIENT_0, "", fs_reinit_required);
      if (fs_reinit_required) {
        while (game::isModLoading(game::LOCAL_CLIENT_0)) {
          std::this_thread::sleep_for(100ms);
        }
      }
    }
  }
}

static std::mutex reconnect_guard_mutex;
static std::string last_auto_reconnect_target;

void com_error_missing_map_stub(const char *file, int line, int code,
                                const char *fmt, ...) {
  const auto target = party::get_connect_host();
  if (target.type != game::NA_BAD) {
    const auto addr_str =
        utils::string::va("%i.%i.%i.%i:%hu", target.ipv4.a, target.ipv4.b,
                          target.ipv4.c, target.ipv4.d, target.port);

    {
      std::lock_guard lock(reconnect_guard_mutex);
      if (last_auto_reconnect_target == addr_str) {
        last_auto_reconnect_target.clear();
        game::Com_Error_(file, line, code, "%s", "Missing map!");
        return;
      }
      last_auto_reconnect_target = addr_str;
    }

    const std::string addr_copy(addr_str);
    printf("[ Workshop ] Missing map/mod detected, reconnecting to %s for "
           "download\n",
           addr_copy.c_str());

    scheduler::once(
        [addr_copy] {
          game::Cbuf_AddText(
              0, utils::string::va("connect %s\n", addr_copy.c_str()));
        },
        scheduler::main, 3s);

    game::Com_Error_(file, line, code, "%s",
                     "Missing map! Reconnecting to download...");
    return;
  }

  game::Com_Error_(file, line, code, "%s", "Missing map!");
}

// bo3-bundle: skip DB_UnloadXZone when the flag is on.
//
// ShutdownGame triggers a chain that ultimately calls DB_UnloadXZone for
// every loaded zone. Earlier we tried NOPping ShutdownGame itself, which
// broke loadMod's state machine -- ShutdownGame does more than unload.
// Hooking DB_UnloadXZone is surgical: state-machine reset still runs,
// only the per-zone unload returns early.
//
// DB_UnloadXZone prologue is `48 89 5c 24 08` (5 bytes) -- safely
// detourable. Args: (uint zoneSlot, bool b1, int i3).
utils::hook::detour db_unload_xzone_hook;
utils::hook::detour unload_ff_mapping_hook;
utils::hook::detour zone_lookup_hook;
std::atomic<bool> g_skip_unload{false};
std::atomic<bool> g_redirect_mod_slots{false};
std::atomic<int> g_skip_unload_count{0};
std::atomic<int> g_redirect_count{0};

// Lookup function called from FastFileLoad's validator (at 0x14142515D).
// Args from the call site: rcx = &[rsp+0xb0], rdx = &[rsp+0x80] -- both
// pointers to stack-local structs, not raw strings. Dump first 24 bytes
// of *rcx to see the actual layout, then we'll know where the name lives.
int zone_lookup_stub(void *p1, void *p2) {
  const bool redirecting =
      g_redirect_mod_slots.load(std::memory_order_relaxed);
  // Trace ALL allocator calls when redirect is on so we can see what
  // strings the allocator gets (just filename? full path? hash?).
  const bool trace = redirecting;
  int n = trace
              ? g_redirect_count.fetch_add(1, std::memory_order_relaxed)
              : 99;
  int real = zone_lookup_hook.invoke<int>(p1, p2);
  if (trace && n < 40) {
    const char *path = static_cast<const char *>(p1);
    char preview[160] = {0};
    if (path) {
      strncpy_s(preview, path, 150);
      preview[150] = 0;
    }
    printf("[ bo3-bundle ] alloc(\"%s\") = %d\n", preview, real);
  }
  return real;
}

void db_unload_xzone_stub(unsigned int zoneSlot, bool b1, int i3) {
  if (g_skip_unload.load(std::memory_order_relaxed)) {
    // Only skip mod-tier slots (15 = en_core_mod, 16 = core_mod). Skipping
    // frontend slots (17/18/20/21) corrupted BO3's state and crashed.
    // Letting frontend unload normally keeps the rest of the engine sane;
    // we just preserve the mod's actual content.
    if (zoneSlot == 15 || zoneSlot == 16) {
      int n = g_skip_unload_count.fetch_add(1, std::memory_order_relaxed);
      if (n < 40) {
        printf("[ bo3-bundle ] SKIP DB_UnloadXZone(slot=%u) -- mod tier preserved\n",
               zoneSlot);
      }
      return;
    }
  }
  db_unload_xzone_hook.invoke<void>(zoneSlot, b1, i3);
}

// LOOP2 inside the unload chain (at static 0x141424BF6) calls this with
// rcx = &g_zoneNames[slot] (name is the first 64 bytes). This is the
// function that ACTUALLY frees the FF mapping -- after this returns, the
// "Unloaded fastfile '%s'" message gets printed. Skipping it for mod-tier
// names keeps Solo's en_core_mod / core_mod mappings alive.
void unload_ff_mapping_stub(void *zone_ptr, void *p2, int flags) {
  if (g_skip_unload.load(std::memory_order_relaxed) && zone_ptr) {
    const char *name = static_cast<const char *>(zone_ptr);
    // Defensive: ensure first byte is reasonable ASCII before strncmp.
    if (name[0] >= 0x20 && name[0] < 0x7F) {
      const bool is_mod = (strncmp(name, "core_mod", 8) == 0
                           && (name[8] == 0 || name[8] == '\0'))
                       || (strncmp(name, "en_core_mod", 11) == 0
                           && (name[11] == 0 || name[11] == '\0'));
      if (is_mod) {
        int n = g_skip_unload_count.fetch_add(1, std::memory_order_relaxed);
        if (n < 40) {
          printf("[ bo3-bundle ] SKIP unload_ff_mapping name=\"%.32s\"\n",
                 name);
        }
        return;
      }
    }
  }
  unload_ff_mapping_hook.invoke<void>(zone_ptr, p2, flags);
}

// bo3-bundle: file-watcher remote-command channel.
//
// The iteration loop "click console -> type command -> wait for crash ->
// restart BOIII" is too expensive. This watcher polls a file in
// %LOCALAPPDATA%\boiii\bundle_commands.txt. Anything appended to it gets
// dispatched via Cbuf_AddText on the main thread, then the file is
// truncated. So an external script can pipe commands in without anyone
// touching the in-game console.
//
// Output goes to console.log via the existing print path -- the external
// script just tails console.log to see the result.
std::thread g_cmd_watch_thread;
std::atomic<bool> g_cmd_watch_stop{false};

void cmd_watch_loop() {
  std::filesystem::path path;
  try {
    path = game::get_appdata_path() / "bundle_commands.txt";
  } catch (...) {
    return;
  }
  while (!g_cmd_watch_stop.load(std::memory_order_relaxed)) {
    std::this_thread::sleep_for(300ms);
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) continue;
    const auto sz = std::filesystem::file_size(path, ec);
    if (ec || sz == 0) continue;

    std::string content;
    {
      std::ifstream f(path, std::ios::binary);
      if (!f) continue;
      content.assign((std::istreambuf_iterator<char>(f)),
                     std::istreambuf_iterator<char>());
    }
    if (content.empty()) continue;
    // Truncate immediately so re-appends after this point come through next
    // poll instead of being re-executed.
    {
      std::ofstream(path, std::ios::trunc | std::ios::binary);
    }

    std::istringstream iss(content);
    std::string line;
    while (std::getline(iss, line)) {
      while (!line.empty() && (line.back() == '\r' || line.back() == ' '
                               || line.back() == '\t')) {
        line.pop_back();
      }
      if (line.empty() || line[0] == '#') continue;

      // SPECIAL: bundle_sleep_watcher <ms> blocks the watcher thread for
      // the given milliseconds before processing the next line. Used to
      // gap dumps from sync-blocking commands queued earlier (like
      // bundle_test_loadmod) so they can fire mid-load when the main
      // thread is busy.
      if (line.rfind("bundle_sleep_watcher", 0) == 0) {
        int ms = 1000;
        auto sp = line.find(' ');
        if (sp != std::string::npos) {
          try { ms = std::stoi(line.substr(sp + 1)); } catch (...) {}
        }
        printf("[ bo3-bundle ] >> sleep_watcher %d ms\n", ms);
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        continue;
      }

      // SPECIAL: bundle_load_solo_menu runs IN THE WATCHER THREAD with
      // SendInput interleaved with Sleeps. Running this on the main thread
      // (via scheduler::once) blocks BO3's input pump during the Sleeps,
      // so the keys we just sent never get processed.
      if (line == "bundle_load_solo_menu") {
        printf("[ bo3-bundle ] >> bundle_load_solo_menu (watcher thread)\n");
        const DWORD my_pid = GetCurrentProcessId();
        struct CB { DWORD pid; HWND found; } cb = { my_pid, nullptr };
        EnumWindows([](HWND h, LPARAM lp) -> BOOL {
          auto *cb = reinterpret_cast<CB*>(lp);
          DWORD wpid = 0;
          GetWindowThreadProcessId(h, &wpid);
          if (wpid == cb->pid && IsWindowVisible(h)) {
            char buf[128] = {0};
            GetWindowTextA(h, buf, sizeof(buf) - 1);
            if (buf[0] != 0) { cb->found = h; return FALSE; }
          }
          return TRUE;
        }, reinterpret_cast<LPARAM>(&cb));
        HWND hwnd = cb.found;
        auto focus_bo3 = [hwnd]() {
          if (!hwnd) return;
          const DWORD my_tid = GetCurrentThreadId();
          const DWORD fg_tid = GetWindowThreadProcessId(
              GetForegroundWindow(), nullptr);
          if (my_tid != fg_tid) AttachThreadInput(my_tid, fg_tid, TRUE);
          SetForegroundWindow(hwnd);
          BringWindowToTop(hwnd);
          if (my_tid != fg_tid) AttachThreadInput(my_tid, fg_tid, FALSE);
        };
        auto send = [](WORD vk, WORD scan, bool ext = false) {
          INPUT in[2] = {};
          in[0].type = INPUT_KEYBOARD;
          in[0].ki.wVk = vk;
          in[0].ki.wScan = scan;
          in[0].ki.dwFlags = KEYEVENTF_SCANCODE
                            | (ext ? KEYEVENTF_EXTENDEDKEY : 0);
          in[1] = in[0];
          in[1].ki.dwFlags |= KEYEVENTF_KEYUP;
          SendInput(2, in, sizeof(INPUT));
        };
        focus_bo3();
        Sleep(150);
        // Dismiss splash
        send(VK_RETURN, 0x1C);
        printf("[ bo3-bundle ] load_solo_menu: splash dismissed, waiting 8s\n");
        Sleep(8000); // splash -> main menu is SLOW (cinematic + cfg load)
        focus_bo3();
        Sleep(300);
        printf("[ bo3-bundle ] load_solo_menu: navigating to MODS\n");
        // Navigate MULTIPLAYER -> MODS (5x Down)
        for (int i = 0; i < 5; ++i) {
          send(VK_DOWN, 0x50, true);
          Sleep(350);
        }
        Sleep(600);
        printf("[ bo3-bundle ] load_solo_menu: opening MODS\n");
        // Open MODS menu
        send(VK_RETURN, 0x1C);
        Sleep(1800);
        printf("[ bo3-bundle ] load_solo_menu: loading Solo\n");
        // Load Solo (top item)
        send(VK_RETURN, 0x1C);
        printf("[ bo3-bundle ] load_solo_menu: sequence complete\n");
        continue;
      }

      // SPECIAL: bundle_dump_zones_now runs IMMEDIATELY on the watcher
      // thread (not via scheduler::once on main). Lets us read
      // g_zoneNames while the main thread is busy in a loadMod call
      // -- otherwise the dump command would queue up and run only
      // AFTER loadMod returns (and possibly after a crash).
      if (line == "bundle_dump_zones_now") {
        printf("[ bo3-bundle ] >> bundle_dump_zones_now (watcher thread)\n");
        struct zone_entry {
          char name[64];
          int flags;
          int slot;
          char unknown1[16];
          int state;
          unsigned char streamPreloaded;
          char padding[3];
        };
        static_assert(sizeof(zone_entry) == 96, "");
        auto *zones = reinterpret_cast<zone_entry *>(0x14998FB80_g);
        for (int i = 0; i < 64; ++i) {
          if (zones[i].name[0] == 0) continue;
          char safe_name[65] = {0};
          for (int k = 0; k < 64 && zones[i].name[k]; ++k) {
            char c = zones[i].name[k];
            safe_name[k] = (c >= 0x20 && c < 0x7F) ? c : '?';
          }
          printf("[ bo3-bundle ]   [%d] name=\"%s\" flags=0x%08X state=%d\n",
                 i, safe_name, zones[i].flags, zones[i].state);
        }
        continue;
      }

      const std::string cmd_copy = line;
      // Dispatch on main thread -- printf and Cbuf_AddText both need BO3's
      // console state ready, which the main pipeline guarantees.
      scheduler::once(
          [cmd_copy]() {
            printf("[ bo3-bundle ] >> %s\n", cmd_copy.c_str());
            game::Cbuf_AddText(0, (cmd_copy + "\n").c_str());
          },
          scheduler::main);
    }
  }
}

// bo3-bundle: persist file-open tracing state and CreateFile* detours.
// Tracing is gated by g_trace_file_opens so we don't spam the console with
// every file-open BO3 makes during normal gameplay -- only when we flip it
// on around a bundle_load_ff probe.
//
// CreateFileA was tried briefly and crashed BO3 at boot -- detours don't
// reliably patch CreateFileA's tiny-stub prologue on Win10/11. We catch
// path opens via CreateFileW (the underlying W variant) instead. If even
// CreateFileW captures nothing during a failed FF load, the FF reader is
// using NtCreateFile / file mappings / a pre-built index -- track that
// down via "Read error of file" xref instead of more low-level hooks.
utils::hook::detour create_file_w_hook;
std::atomic<bool> g_trace_file_opens{false};

// Filter out paths we never care about. HID device polling alone fires ~10
// CreateFileW calls every ~500ms which buries any actual FF lookup. The
// "interesting" path predicate keeps anything that COULD be a FF/asset
// lookup -- contains zone, .ff, .xpak, .pak, mods\, players\, workshop, or
// the BO3 install dir.
bool is_interesting_path(const char *p) {
  if (!p || !*p) return false;
  // Always reject HID device handles -- those are kernel polling, never FF.
  if (strstr(p, "\\hid#") || strstr(p, "/hid#")) return false;
  if (strstr(p, "\\HID#") || strstr(p, "/HID#")) return false;
  // Anything with these substrings is plausibly relevant. Keep this set
  // narrow -- broad needles like "Steam" or "Black Ops III" matched dozens
  // of probe paths during boot and crashed BO3 (printing during a CreateFile
  // callback before BO3's console critical section was initialized).
  static const char *needles[] = {
      ".ff", ".FF", ".xpak", ".XPAK", ".pak", ".PAK",
      "\\zone\\", "/zone/", "workshop\\content", "workshop/content",
      "\\mods\\", "/mods/"};
  for (const char *n : needles) {
    if (strstr(p, n)) return true;
  }
  return false;
}

HANDLE WINAPI create_file_w_stub(LPCWSTR fn, DWORD access, DWORD share,
                                  LPSECURITY_ATTRIBUTES sa, DWORD disp,
                                  DWORD attr, HANDLE templ) {
  HANDLE h = create_file_w_hook.invoke<HANDLE>(fn, access, share, sa, disp,
                                                attr, templ);
  if (g_trace_file_opens.load(std::memory_order_relaxed) && fn) {
    char ascii[1024];
    int n = WideCharToMultiByte(CP_UTF8, 0, fn, -1, ascii,
                                  sizeof(ascii) - 1, nullptr, nullptr);
    if (n > 0 && is_interesting_path(ascii)) {
      const bool fail = (h == INVALID_HANDLE_VALUE);
      const DWORD err = fail ? GetLastError() : 0;
      printf("[ bo3-bundle ] CreateFileW(\"%s\") = %s%s\n", ascii,
             fail ? "INVALID" : "ok",
             fail ? (" (err=" + std::to_string(err) + ")").c_str() : "");
      if (fail) SetLastError(err);
    }
  }
  return h;
}


// bo3-bundle: auto-apply the validator patch we discovered manually
// (jnz -> jmp at the "ERROR: Could not find zone" branch). Without this we
// have to bundle_findstrxref + bundle_patch on every BOIII restart, and any
// failed test crashes BO3, forcing a restart loop. This eliminates that
// step. Patch site: BlackOps3.exe + 0x1425169 (1 byte: 0x75 -> 0xEB).
//
// We refuse to patch if the byte at that offset isn't 0x75 -- means BO3 was
// updated and the offset moved.
void auto_patch_validator() {
  const HMODULE bo3 = GetModuleHandleA("BlackOps3.exe");
  if (!bo3) {
    printf("[ bo3-bundle ] AUTO-PATCH: BlackOps3.exe not loaded (?), skip\n");
    return;
  }
  const auto base = reinterpret_cast<uintptr_t>(bo3);
  // Offset of the "jnz over Com_Error" instruction within BlackOps3.exe.
  // Static address 0x141425169 (per docs); offset = 0x141425169 - 0x140000000.
  auto *patch_addr = reinterpret_cast<uint8_t *>(base + 0x1425169);
  if (*patch_addr == 0xEB) {
    printf("[ bo3-bundle ] AUTO-PATCH: validator already 0xEB, no-op\n");
    return;
  }
  if (*patch_addr != 0x75) {
    printf("[ bo3-bundle ] AUTO-PATCH: REFUSE -- expected 0x75 at +0x1425169, "
           "got 0x%02X. BO3 binary changed?\n",
           *patch_addr);
    return;
  }
  DWORD old_protect = 0;
  if (!VirtualProtect(patch_addr, 1, PAGE_EXECUTE_READWRITE, &old_protect)) {
    printf("[ bo3-bundle ] AUTO-PATCH: VirtualProtect failed err=%lu\n",
           GetLastError());
    return;
  }
  *patch_addr = 0xEB;
  DWORD dummy = 0;
  VirtualProtect(patch_addr, 1, old_protect, &dummy);
  FlushInstructionCache(GetCurrentProcess(), patch_addr, 1);
  printf("[ bo3-bundle ] AUTO-PATCH: validator 0x75 -> 0xEB at "
         "BlackOps3.exe+0x1425169 OK\n");
}

class component final : public generic_component {
public:
  void post_unpack() override {
    auto_patch_validator();

    // Hook DB_UnloadXZone so we can skip per-zone asset cleanup.
    db_unload_xzone_hook.create(0x141425A70_g, db_unload_xzone_stub);
    // Hook the FF-mapping unloader (LOOP2 in the wrapper at 0x141424BF6).
    // Without THIS hook, slot 15/16 still get their file mappings freed
    // even if DB_UnloadXZone is skipped. Static = 0x14141F040 (computed
    // from runtime 0x7ff6e17cf040 minus image base 0x7ff6e03b0000 plus
    // 0x140000000). Earlier 0x1413CF040 was a math typo.
    unload_ff_mapping_hook.create(0x14141F040_g, unload_ff_mapping_stub);
    // Hook the descriptor ALLOCATOR at 0x142179960 (called from the
    // lookup wrapper at 0x1413F0E80). Returns the actual slot index BO3
    // will allocate. Hooking this -- rather than the wrapper -- means our
    // returned value drives the actual data binding, not just what BO3
    // *thinks* the slot is.
    zone_lookup_hook.create(0x142179960_g, zone_lookup_stub);

    // Start the remote-command file watcher so an external script can
    // pipe console commands without anyone touching the in-game console.
    g_cmd_watch_stop.store(false, std::memory_order_relaxed);
    g_cmd_watch_thread = std::thread(cmd_watch_loop);

    // Hook CreateFileW only. CreateFileA detour crashes BO3 (tiny stub).
    //
    // Trace defaults OFF. Enabling at boot crashed BO3: our printf path
    // ends in BO3's print_message which acquires a critical section that
    // isn't initialized until later in boot. User toggles via
    // bundle_trace_files 1 once they're at the menu.
    if (auto *k32 = GetModuleHandleA("kernel32.dll")) {
      if (auto *cfw = GetProcAddress(k32, "CreateFileW")) {
        create_file_w_hook.create(cfw, create_file_w_stub);
      }
    }

    [[maybe_unused]] const auto *dvar_retry = game::register_dvar_int(
        "workshop_retry_attempts", 30, 1, 1000, game::DVAR_ARCHIVE,
        "Number of connection retry attempts for workshop downloads (default "
        "15, increase for slow connections)");
    [[maybe_unused]] const auto *dvar_timeout = game::register_dvar_int(
        "workshop_timeout", 300, 60, 3600, game::DVAR_ARCHIVE,
        "Download timeout in seconds for workshop items (reserved for future "
        "use)");

    dlc_popup_thread_obj = std::thread(dlc_popup_thread_func);

    command::add("userContentReload", [](const command::params &params) {
      game::reloadUserContent();
      if (!game::is_server())
        toast::info("Workshop", "User content reloaded");
    });
    command::add("workshop_config", [](const command::params &params) {
      printf(
          "[ Workshop ] workshop_retry_attempts: %d (set in game or config)\n",
          get_workshop_retry_attempts());
      printf("[ Workshop ] workshop_timeout: %d\n",
             game::get_dvar_int("workshop_timeout"));
    });
    command::add("workshop_download", [](const command::params &params) {
      if (params.size() < 2) {
        printf("[ Workshop ] Usage: workshop_download <id> [Map|Mod]\n");
        return;
      }
      const std::string id = params.get(1);
      std::string type_str = params.size() >= 3 ? params.get(2) : "Map";
      if (id.empty())
        return;
      if (is_any_download_active()) {
        game::UI_OpenErrorPopupWithMessage(
            0, game::ERROR_UI,
            "A download is already in progress. Wait for it to finish.");
        return;
      }
      if (type_str != "Map" && type_str != "Mod")
        type_str = "Map";
      printf("[ Workshop ] Starting download: %s (%s)\n", id.c_str(),
             type_str.c_str());
      if (!game::is_server())
        toast::show("Workshop",
                    utils::string::va("Downloading %s: %s", type_str.c_str(),
                                      id.c_str()),
                    "t7_icon_menu_options_download");
      download_thread = utils::thread::create_named_thread(
          "workshop_download", steamcmd::initialize_download, id, type_str);
      download_thread.detach();
    });

    // bo3-bundle: dump a runtime disassembly window via udis86. Lets us
    // inspect the *unpacked* code that Ghidra can't see (BO3.exe ships with
    // obfuscated bytes on disk; the real instructions only appear in
    // process memory after BO3's startup decoder runs).
    //
    // Usage: bundle_disasm <hex_address> [instr_count_default_30]
    // Example: bundle_disasm 1422A4585 40
    command::add("bundle_disasm",
                 [](const command::params &params) {
      if (params.size() < 2) {
        printf("[ bo3-bundle ] Usage: bundle_disasm <hex_address> "
               "[instr_count]\n");
        return;
      }
      uint64_t addr = 0;
      try {
        addr = std::stoull(params.get(1), nullptr, 16);
      } catch (...) {
        printf("[ bo3-bundle ] Couldn't parse address \"%s\" as hex\n",
               params.get(1));
        return;
      }
      // If the address looks like a static BO3 address (preferred load
      // base 0x140000000), auto-relocate via ASLR. Runtime addresses
      // (0x00007FF...) skip this.
      if (addr >= 0x140000000ULL && addr < 0x150000000ULL) {
        const auto rebased = game::relocate(static_cast<size_t>(addr));
        printf("[ bo3-bundle ] auto-rebased static 0x%llx -> runtime 0x%llx\n",
               static_cast<unsigned long long>(addr),
               static_cast<unsigned long long>(rebased));
        addr = static_cast<uint64_t>(rebased);
      }
      unsigned int count = 30;
      if (params.size() >= 3) {
        try {
          count = static_cast<unsigned int>(std::stoul(params.get(2)));
        } catch (...) {
          /* keep default */
        }
      }
      // Read up to 16 bytes per instruction (max x86-64 length); generous
      // buffer keeps us from undershooting the request.
      const size_t buffer_size = static_cast<size_t>(count) * 16;
      const uint8_t *bytes = reinterpret_cast<const uint8_t *>(addr);

      ud_t ud;
      ud_init(&ud);
      ud_set_mode(&ud, 64);
      ud_set_syntax(&ud, UD_SYN_INTEL);
      ud_set_pc(&ud, addr);
      ud_set_input_buffer(&ud, bytes, buffer_size);

      printf("[ bo3-bundle ] disasm @ 0x%016llx (%u instr)\n",
             static_cast<unsigned long long>(addr), count);
      for (unsigned int i = 0; i < count; ++i) {
        if (!ud_disassemble(&ud)) {
          printf("[ bo3-bundle ]   <decode stopped after %u instr>\n", i);
          break;
        }
        printf("[ bo3-bundle ]   0x%016llx: %-22s %s\n",
               static_cast<unsigned long long>(ud_insn_off(&ud)),
               ud_insn_hex(&ud),
               ud_insn_asm(&ud));
      }
    });

    // bo3-bundle: HACK -- write a fake entry into g_zoneNames[] so BO3's
    // language-pair lookup (function at 0x...0e80) can find it. Used to
    // probe whether we can satisfy the "en_X must be loaded" requirement
    // without actually loading en_X.ff.
    //
    // Usage: bundle_inject_zone <name> <flags_hex> [slot_dec]
    // Example: bundle_inject_zone en_zm_mod 8000080 -1
    //
    // Picks the first empty slot (name[0]==0 and flags==0). Sets state
    // to 3 (XZONE_COMPLETE). Slot defaults to -1 (matches what other
    // entries use).
    //
    // Risk: if BO3 actually tries to USE the zone's assets (not just
    // verify it's registered), things will crash hard. We'll see.
    command::add("bundle_inject_zone",
                 [](const command::params &params) {
      if (params.size() < 3) {
        printf("[ bo3-bundle ] Usage: bundle_inject_zone <name> "
               "<flags_hex> [slot_dec]\n");
        return;
      }
      struct zone_entry {
        char name[64];
        int flags;
        int slot;
        char unknown1[16];
        int state;
        unsigned char streamPreloaded;
        char padding[3];
      };
      static_assert(sizeof(zone_entry) == 96, "");
      const std::string name_s = params.get(1);
      int flags = 0;
      try {
        flags = static_cast<int>(std::stoul(params.get(2), nullptr, 16));
      } catch (...) {
        printf("[ bo3-bundle ] couldn't parse flags as hex\n");
        return;
      }
      int slot = -1;
      if (params.size() >= 4) {
        try {
          slot = std::stoi(params.get(3));
        } catch (...) { /* keep default */ }
      }
      auto *zones = reinterpret_cast<zone_entry *>(0x14998FB80_g);
      // Find first empty slot.
      int target = -1;
      for (int i = 0; i < 64; ++i) {
        if (zones[i].name[0] == '\0' && zones[i].flags == 0
            && zones[i].state == 0) {
          target = i;
          break;
        }
      }
      if (target == -1) {
        printf("[ bo3-bundle ] no empty zone slot available\n");
        return;
      }
      auto &z = zones[target];
      std::memset(&z, 0, sizeof(z));
      const auto copy_len = std::min(name_s.size(),
                                      sizeof(z.name) - 1);
      std::memcpy(z.name, name_s.data(), copy_len);
      z.flags = flags;
      z.slot = slot;
      z.state = 3;  // XZONE_COMPLETE
      printf("[ bo3-bundle ] injected zone[%d] name=\"%s\" flags=0x%X "
             "slot=%d state=COMPLETE\n",
             target, z.name, static_cast<unsigned>(z.flags), z.slot);
    });

    // bo3-bundle: dump raw bytes from a memory address as hex + ASCII.
    // Used to inspect the bytes before/after a string match so we can find
    // the actual start of the format string (and thus the address that
    // code references via lea).
    //
    // Usage: bundle_dumpbytes <hex_addr> [byte_count_default_64]
    command::add("bundle_dumpbytes",
                 [](const command::params &params) {
      if (params.size() < 2) {
        printf("[ bo3-bundle ] Usage: bundle_dumpbytes <hex_addr> [count]\n");
        return;
      }
      uint64_t addr = 0;
      try {
        addr = std::stoull(params.get(1), nullptr, 16);
      } catch (...) {
        printf("[ bo3-bundle ] couldn't parse address as hex\n");
        return;
      }
      size_t count = 64;
      if (params.size() >= 3) {
        try {
          count = std::stoul(params.get(2));
        } catch (...) { /* keep default */ }
      }
      if (count > 1024) count = 1024;
      const auto *bytes = reinterpret_cast<const uint8_t *>(addr);
      printf("[ bo3-bundle ] %zu bytes @ 0x%016llx\n",
             count, static_cast<unsigned long long>(addr));
      for (size_t i = 0; i < count; i += 16) {
        char hex[80] = {};
        char ascii[20] = {};
        size_t hex_pos = 0;
        for (size_t j = 0; j < 16 && i + j < count; ++j) {
          const uint8_t b = bytes[i + j];
          hex_pos += snprintf(hex + hex_pos,
                              sizeof(hex) - hex_pos,
                              "%02x ", b);
          ascii[j] = (b >= 32 && b < 127) ? static_cast<char>(b) : '.';
        }
        printf("[ bo3-bundle ]   +%04zx: %-48s | %s\n", i, hex, ascii);
      }
    });

    // bo3-bundle: scan BO3 memory for an 8-byte value matching the target
    // address. Catches string-table-style references where format strings
    // are stored as pointers in a data table (common when bundle_findxref
    // returns 0 hits despite the string clearly being referenced).
    //
    // Usage: bundle_finddataref <hex_target_addr>
    command::add("bundle_finddataref",
                 [](const command::params &params) {
      if (params.size() < 2) {
        printf("[ bo3-bundle ] Usage: bundle_finddataref <hex_addr>\n");
        return;
      }
      uint64_t target = 0;
      try {
        target = std::stoull(params.get(1), nullptr, 16);
      } catch (...) {
        printf("[ bo3-bundle ] couldn't parse address as hex\n");
        return;
      }
      printf("[ bo3-bundle ] scanning for 8-byte value 0x%016llx...\n",
             static_cast<unsigned long long>(target));

      const HMODULE module = GetModuleHandleA("BlackOps3.exe");
      MODULEINFO mi{};
      if (!module || !GetModuleInformation(GetCurrentProcess(), module, &mi,
                                            sizeof(mi))) {
        printf("[ bo3-bundle ] couldn't get BO3.exe module info\n");
        return;
      }
      const auto base = reinterpret_cast<uintptr_t>(mi.lpBaseOfDll);
      const auto end = base + mi.SizeOfImage;

      int hit_count = 0;
      uintptr_t addr = base;
      while (addr < end && hit_count < 25) {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(reinterpret_cast<void *>(addr), &info,
                         sizeof(info)) == 0) {
          break;
        }
        const auto region_end = reinterpret_cast<uintptr_t>(info.BaseAddress)
                                + info.RegionSize;
        const bool readable =
            info.State == MEM_COMMIT &&
            (info.Protect & (PAGE_READONLY | PAGE_READWRITE
                             | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE
                             | PAGE_WRITECOPY));
        if (readable && info.RegionSize >= 8) {
          const auto *bytes =
              reinterpret_cast<const uint8_t *>(info.BaseAddress);
          // Walk 8 bytes at a time aligned (most pointers are at least
          // 4-byte aligned, often 8). Step 4 to catch 4-byte-aligned ones.
          for (size_t i = 0; i + 8 <= info.RegionSize && hit_count < 25;
               i += 4) {
            uint64_t v = 0;
            std::memcpy(&v, bytes + i, 8);
            if (v == target) {
              const auto match_addr =
                  reinterpret_cast<uintptr_t>(info.BaseAddress) + i;
              printf("[ bo3-bundle ]   data ref @ 0x%016llx\n",
                     static_cast<unsigned long long>(match_addr));
              ++hit_count;
            }
          }
        }
        addr = region_end;
      }
      printf("[ bo3-bundle ] done (%d data refs; capped at 25)\n", hit_count);
    });

    // bo3-bundle: find code references to a runtime address. Scans BO3's
    // executable memory looking for instructions whose RIP-relative operand
    // equals the target address. Useful after bundle_findstring locates a
    // format string -- bundle_findxref then locates the function that
    // references it.
    //
    // Usage: bundle_findxref <hex_target_runtime_address>
    // Example: bundle_findxref 7ff77e7c0c30
    command::add("bundle_findxref",
                 [](const command::params &params) {
      if (params.size() < 2) {
        printf("[ bo3-bundle ] Usage: bundle_findxref <hex_runtime_addr>\n");
        return;
      }
      uint64_t target = 0;
      try {
        target = std::stoull(params.get(1), nullptr, 16);
      } catch (...) {
        printf("[ bo3-bundle ] couldn't parse address as hex\n");
        return;
      }
      printf("[ bo3-bundle ] scanning for xrefs to 0x%016llx...\n",
             static_cast<unsigned long long>(target));

      const HMODULE module = GetModuleHandleA("BlackOps3.exe");
      MODULEINFO mi{};
      if (!module || !GetModuleInformation(GetCurrentProcess(), module, &mi,
                                            sizeof(mi))) {
        printf("[ bo3-bundle ] couldn't get BO3.exe module info\n");
        return;
      }
      const auto base = reinterpret_cast<uintptr_t>(mi.lpBaseOfDll);
      const auto end = base + mi.SizeOfImage;

      int hit_count = 0;
      uintptr_t addr = base;
      while (addr < end && hit_count < 25) {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(reinterpret_cast<void *>(addr), &info,
                         sizeof(info)) == 0) {
          break;
        }
        const auto region_end = reinterpret_cast<uintptr_t>(info.BaseAddress)
                                + info.RegionSize;
        const bool executable =
            info.State == MEM_COMMIT &&
            (info.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ
                             | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
        if (executable) {
          // Walk this region 1 byte at a time, decoding each position.
          // Slow but reliable -- udis86 handles arbitrary alignment.
          ud_t ud;
          ud_init(&ud);
          ud_set_mode(&ud, 64);
          ud_set_syntax(&ud, UD_SYN_INTEL);
          const auto *bytes = reinterpret_cast<const uint8_t *>(info.BaseAddress);
          for (size_t i = 0; i < info.RegionSize - 16; ++i) {
            // Quick prefilter: any common RIP-relative instruction encoding.
            //   4? 8d/8b <ModRM> <disp32>    -- LEA/MOV with REX prefix
            //                                   (4? = 0x48..0x4F covers all
            //                                   REX.W combinations including
            //                                   r8-r15 destinations)
            //   8d/8b <ModRM> <disp32>       -- 32-bit LEA/MOV (no REX)
            //   ff 15/25 <ModRM> <disp32>    -- CALL/JMP [rip+]
            //   e8/e9 <disp32>               -- CALL/JMP rel32
            // ModRM = mod:00 (top 2 bits 0) + R/M:101 (low 3 bits 5)
            //         => (modrm & 0xC7) == 0x05.
            const uint8_t b0 = bytes[i];
            const uint8_t b1 = bytes[i + 1];
            const uint8_t b2 = bytes[i + 2];
            bool maybe_riprel = false;
            int disp_offset = 0;
            if (b0 >= 0x48 && b0 <= 0x4F &&
                (b1 == 0x8d || b1 == 0x8b || b1 == 0x89) &&
                (b2 & 0xC7) == 0x05) {
              maybe_riprel = true;
              disp_offset = 3;
            } else if ((b0 == 0x8d || b0 == 0x8b || b0 == 0x89
                        || b0 == 0x88) &&
                       (b1 & 0xC7) == 0x05) {
              // 0x8d=LEA, 0x8b=MOV r,r/m, 0x89=MOV r/m,r (32-bit WRITE),
              // 0x88=MOV r/m8,r8 (8-bit write).
              maybe_riprel = true;
              disp_offset = 2;
            } else if (b0 == 0xc7 && (b1 & 0xC7) == 0x05) {
              // C7 /0 = MOV r/m32, imm32 (write constant to memory).
              // Disp is at +2; 4-byte immediate follows (so total 10
              // bytes), but we only care about the disp32 here.
              maybe_riprel = true;
              disp_offset = 2;
            } else if (b0 == 0xe8 || b0 == 0xe9) {
              maybe_riprel = true;
              disp_offset = 1;
            } else if (b0 == 0xff && (b1 == 0x15 || b1 == 0x25
                                       || b1 == 0x05 || b1 == 0x0d)) {
              // 0x15=CALL [rip+disp], 0x25=JMP [rip+disp],
              // 0x05=INC dword [rip+disp], 0x0d=DEC dword [rip+disp].
              maybe_riprel = true;
              disp_offset = 2;
            }
            if (!maybe_riprel) continue;
            const auto inst_addr = reinterpret_cast<uintptr_t>(
                info.BaseAddress) + i;
            ud_set_pc(&ud, inst_addr);
            ud_set_input_buffer(&ud, bytes + i, 16);
            if (!ud_disassemble(&ud)) continue;
            const unsigned len = ud_insn_len(&ud);
            // Compute the RIP-relative target. disp_offset is the byte
            // offset of the disp32 within the instruction, set by the
            // prefilter above based on which encoding matched.
            int32_t disp = 0;
            std::memcpy(&disp, bytes + i + disp_offset, 4);
            const uint64_t computed = inst_addr + len
                + static_cast<int64_t>(disp);
            if (computed == target) {
              printf("[ bo3-bundle ]   xref @ 0x%016llx: %-22s %s\n",
                     static_cast<unsigned long long>(inst_addr),
                     ud_insn_hex(&ud), ud_insn_asm(&ud));
              ++hit_count;
              if (hit_count >= 25) break;
            }
          }
        }
        addr = region_end;
      }
      printf("[ bo3-bundle ] done (%d xrefs; capped at 25)\n", hit_count);
    });

    // bo3-bundle: scan BO3's loaded memory for a literal string and report
    // every address it appears at. Used to locate format-string constants
    // (e.g. "FastFileLoad: Setting %s") so we can find the function that
    // references them via subsequent disassembly.
    //
    // Usage: bundle_findstring <ascii_text>
    // Example: bundle_findstring "FastFileLoad: Setting"
    command::add("bundle_findstring",
                 [](const command::params &params) {
      if (params.size() < 2) {
        printf("[ bo3-bundle ] Usage: bundle_findstring <text>\n");
        return;
      }
      // Reassemble the search query from all args after the command name --
      // BOIII's command parser splits on whitespace, so "FastFileLoad:
      // Setting" comes through as two tokens.
      std::string needle;
      for (int i = 1; i < params.size(); ++i) {
        if (i > 1) needle += ' ';
        needle += params.get(i);
      }
      if (needle.empty()) {
        printf("[ bo3-bundle ] empty needle\n");
        return;
      }
      printf("[ bo3-bundle ] scanning for \"%s\" (%zu bytes)...\n",
             needle.c_str(), needle.size());

      // Walk BO3's loaded module's memory regions via VirtualQuery.
      const HMODULE module = GetModuleHandleA("BlackOps3.exe");
      if (!module) {
        printf("[ bo3-bundle ] BlackOps3.exe module not found\n");
        return;
      }
      MODULEINFO mi{};
      if (!GetModuleInformation(GetCurrentProcess(), module, &mi,
                                sizeof(mi))) {
        printf("[ bo3-bundle ] GetModuleInformation failed\n");
        return;
      }
      const auto base = reinterpret_cast<uintptr_t>(mi.lpBaseOfDll);
      const auto end = base + mi.SizeOfImage;
      printf("[ bo3-bundle ] BO3.exe range: 0x%llx - 0x%llx (%u MB)\n",
             static_cast<unsigned long long>(base),
             static_cast<unsigned long long>(end),
             mi.SizeOfImage / (1024 * 1024));

      int hit_count = 0;
      uintptr_t addr = base;
      while (addr < end && hit_count < 10) {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(reinterpret_cast<void *>(addr), &info,
                         sizeof(info)) == 0) {
          break;
        }
        const auto region_end = reinterpret_cast<uintptr_t>(info.BaseAddress)
                                + info.RegionSize;
        const bool readable =
            info.State == MEM_COMMIT &&
            (info.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ
                             | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY));
        if (readable && info.RegionSize > needle.size()) {
          const auto *bytes = reinterpret_cast<const char *>(info.BaseAddress);
          const size_t scan_len = info.RegionSize - needle.size();
          for (size_t i = 0; i < scan_len; ++i) {
            if (bytes[i] == needle[0]
                && std::memcmp(bytes + i, needle.data(), needle.size()) == 0) {
              const auto match_addr =
                  reinterpret_cast<uintptr_t>(info.BaseAddress) + i;
              printf("[ bo3-bundle ]   match at 0x%016llx (region %p, "
                     "protect=0x%X)\n",
                     static_cast<unsigned long long>(match_addr),
                     info.BaseAddress, info.Protect);
              ++hit_count;
              if (hit_count >= 10) break;
            }
          }
        }
        addr = region_end;
      }
      printf("[ bo3-bundle ] done (%d matches; capped at 10)\n", hit_count);
    });

    // bo3-bundle: directly invoke BO3's DB_LoadXAssets (the documented
    // fastfile loader at 0x1414236A0) with a constructed XZoneInfo. We can
    // CALL the function safely even though we couldn't HOOK it (hooking
    // overwrites the prologue and the detour interfered with BO3's
    // expectations; calling just routes through the normal entry).
    //
    // Usage: bundle_load_ff <fastfile_name_without_extension>
    // Example: bundle_load_ff core_mod
    //
    // The fastfile is searched for via the FS search path, so the name
    // resolves to whichever workshop folder finds it first. If you call
    // bundle_addpath for a second mod first, then bundle_load_ff,
    // BO3 will load that ff from whichever path's first match.
    // bo3-bundle: dump BO3's g_zoneNames[] -- the global table of currently
    // loaded zones, their allocFlags, and state. Lets us see what flags
    // Solo's en_core_mod / core_mod were loaded with so we can call
    // DB_LoadXAssets with matching values.
    //
    // g_zoneNames is at static 0x14998FB80 (documented by BOIII in
    // symbols.hpp). Each entry is 96 bytes (sizeof XZoneName); BO3's
    // disassembly uses an upper bound of 0x32 entries iirc.
    command::add("bundle_dump_zones",
                 [](const command::params & /*params*/) {
      // Layout matches the XZoneName struct in BOIII's structs.hpp.
      struct zone_entry {
        char name[64];
        int flags;
        int slot;
        char unknown1[16];
        int state;            // XZoneState enum
        unsigned char streamPreloaded;
        char padding[3];
      };
      static_assert(sizeof(zone_entry) == 96,
                    "XZoneName must be 96 bytes");

      auto *zones = reinterpret_cast<zone_entry *>(0x14998FB80_g);
      printf("[ bo3-bundle ] === g_zoneNames[] @ %p ===\n", zones);
      int loaded = 0;
      for (int i = 0; i < 64; ++i) {
        const auto &z = zones[i];
        // Empty slot detection: name byte 0 == '\0' AND flags == 0
        // (BO3 zeroes entries on unload).
        if (z.name[0] == '\0' && z.flags == 0 && z.state == 0) {
          continue;
        }
        const char *state_name = "?";
        switch (z.state) {
          case -1: state_name = "UNLOADING"; break;
          case 0:  state_name = "EMPTY"; break;
          case 1:  state_name = "LOADING"; break;
          case 2:  state_name = "LOADED"; break;
          case 3:  state_name = "COMPLETE"; break;
          case 4:  state_name = "FAILED"; break;
        }
        printf("[ bo3-bundle ]   [%d] name=\"%.64s\" flags=0x%X slot=%d "
               "state=%s\n",
               i, z.name, static_cast<unsigned>(z.flags), z.slot,
               state_name);
        ++loaded;
      }
      printf("[ bo3-bundle ] === %d non-empty entries ===\n", loaded);
    });

    // bo3-bundle: load a fastfile + its language pair (en_X) in one
    // DB_LoadXAssets call. BO3 always loads pairs together; loading just
    // one half throws "Could not find zone 'en_X'" because internal
    // references span both halves.
    //
    // Usage: bundle_load_ff_pair <base_name> [allocFlags_hex]
    // Example: bundle_load_ff_pair core_mod
    //   -> loads en_core_mod + core_mod with allocFlags=0x800
    command::add("bundle_load_ff_pair",
                 [](const command::params &params) {
      if (params.size() < 2) {
        printf("[ bo3-bundle ] Usage: bundle_load_ff_pair <base_name> "
               "[allocFlags_hex]\n");
        return;
      }
      static thread_local std::string en_name, base_name;
      base_name = params.get(1);
      en_name = "en_" + base_name;
      int alloc_flags = 0x800;
      if (params.size() >= 3) {
        try {
          alloc_flags = static_cast<int>(
              std::stoul(params.get(2), nullptr, 16));
        } catch (...) { /* keep default */ }
      }
      // BO3's own pattern: language pair first, then base fastfile. The
      // language entry has the high bit 0x8000000 set in its allocFlags --
      // observed in g_zoneNames[] where en_core_mod has flags=0x8000040
      // and core_mod has flags=0x40. We assume BO3 uses this bit to
      // distinguish "this IS the language pair, don't auto-prefix" so the
      // name we pass ("en_X") is taken literally.
      const int lang_alloc_flags = alloc_flags | 0x8000000;
      game::XZoneInfo zi[2]{};
      zi[0].name = en_name.c_str();
      zi[0].allocFlags = lang_alloc_flags;
      zi[1].name = base_name.c_str();
      zi[1].allocFlags = alloc_flags;
      printf("[ bo3-bundle ] DB_LoadXAssets([{\"%s\", 0x%X}, "
             "{\"%s\", 0x%X}], count=2, sync=true)...\n",
             zi[0].name, static_cast<unsigned>(lang_alloc_flags),
             zi[1].name, static_cast<unsigned>(alloc_flags));
      game::DB_LoadXAssets(zi, 2, true, false);
      printf("[ bo3-bundle ] returned\n");
    });

    command::add("bundle_load_ff",
                 [](const command::params &params) {
      if (params.size() < 2) {
        printf("[ bo3-bundle ] Usage: bundle_load_ff <ff_name> "
               "[allocFlags_hex] [sync_0_or_1]\n");
        printf("[ bo3-bundle ] Default allocFlags=0x800 (observed at BO3's "
               "own load-zone call site at 0x142148732). Other observed "
               "values: 0x200, 0x800000, 0x400000.\n");
        printf("[ bo3-bundle ] NOTE: BO3 typically loads fastfiles in PAIRS "
               "(en_X + X). For mod fastfiles, prefer bundle_load_ff_pair.\n");
        return;
      }
      static thread_local std::string saved_name;
      saved_name = params.get(1);
      game::XZoneInfo zi{};
      zi.name = saved_name.c_str();
      // 0x800 is what BO3 itself uses when calling DB_LoadXAssets to load a
      // zone in this code path. 0x4 (our previous guess) freed too much
      // vanilla content -- it was a different slot ID.
      zi.allocFlags = 0x800;
      if (params.size() >= 3) {
        try {
          zi.allocFlags = static_cast<int>(
              std::stoul(params.get(2), nullptr, 16));
        } catch (...) { /* keep default */ }
      }
      // freeFlags: BO3's normal mod-load call site at 0x1420F938F uses
      // freeFlags = 0x140 (= 0x100 | allocFlags 0x40). This may be what
      // triggers the "free old mod + autoload en_X.ff into fresh tier"
      // sequence we need. Default to 0x100 | allocFlags so the user can
      // mirror BO3's pattern without typing it out.
      zi.freeFlags = 0x100 | zi.allocFlags;
      if (params.size() >= 4) {
        try {
          zi.freeFlags = static_cast<int>(
              std::stoul(params.get(3), nullptr, 16));
        } catch (...) { /* keep default */ }
      }
      zi.allocSlot = 0;
      zi.freeSlot = 0;

      bool sync = true;
      if (params.size() >= 5) {
        sync = std::string(params.get(4)) != "0";
      }

      printf("[ bo3-bundle ] DB_LoadXAssets({name=\"%s\", allocFlags=0x%X, "
             "freeFlags=0x%X, allocSlot=0, freeSlot=0}, count=1, sync=%s, "
             "suppressSync=false)...\n",
             zi.name, static_cast<unsigned>(zi.allocFlags),
             static_cast<unsigned>(zi.freeFlags),
             sync ? "true" : "false");

      // Gate the redundant-warning hook so it only captures DURING this
      // load (not on every BO3 FF load that might happen unrelated).
      // The hook adds DS4C's collision-named script assets to
      // g_pending_bundle_scripts; drain_pending_bundle_activations later
      // calls Scr_LoadScript on each at the right phase.
      const auto pending_before = [] {
        std::lock_guard lock(g_pending_scripts_mutex);
        return g_pending_bundle_scripts.size();
      }();
      g_inside_bundle_load.store(true, std::memory_order_relaxed);

      // File-open tracing is on globally from boot (see post_unpack); the
      // is_interesting filter keeps the log clean.
      game::DB_LoadXAssets(&zi, 1, sync, false);

      g_inside_bundle_load.store(false, std::memory_order_relaxed);
      const auto pending_after = [] {
        std::lock_guard lock(g_pending_scripts_mutex);
        return g_pending_bundle_scripts.size();
      }();
      printf("[ bo3-bundle ] returned (captured %zu collision-named scripts "
             "via redundant-warning hook, total pending: %zu)\n",
             pending_after - pending_before, pending_after);
    });

    // bo3-bundle: multi-mod GSC activation.
    // After loading a second mod's FF via bundle_load_ff, its script_parse_tree
    // assets are in the pool but inert -- nothing called Scr_LoadScript on
    // them, so their EXPORT_AUTOEXEC functions never fire. This command
    // enumerates the script_parse_tree pool and calls Scr_LoadScript on each
    // asset, picking SERVER instance for .gsc and CLIENT for .csc.
    //
    // Usage: bundle_activate_scripts [name_substr_filter]
    //   With no filter, activates ALL scripts in the pool (safe no-op for
    //   already-active ones). With a filter, only activates scripts whose
    //   name contains the substring.
    command::add("bundle_activate_scripts",
                 [](const command::params &params) {
      const std::string filter = params.size() >= 2 ? params.get(1) : "";

      struct ctx_t {
        std::string filter;
        int enumerated;
        int matched;
        int activated_gsc;
        int activated_csc;
      };
      ctx_t ctx{filter, 0, 0, 0, 0};

      printf("[ bo3-bundle ] activating script_parse_tree assets (filter=%s)"
             "...\n",
             filter.empty() ? "<none>" : filter.c_str());

      game::DB_EnumXAssets(
          game::ASSET_TYPE_SCRIPTPARSETREE,
          [](game::XAssetHeader header, void *userdata) {
            auto *c = static_cast<ctx_t *>(userdata);
            c->enumerated++;
            if (!header.luaFile || !header.luaFile->name)
              return;
            std::string name = header.luaFile->name;
            if (!c->filter.empty() &&
                name.find(c->filter) == std::string::npos)
              return;
            c->matched++;
            bool is_csc = false;
            if (name.size() > 4) {
              auto suffix = name.substr(name.size() - 4);
              if (suffix == ".csc")
                is_csc = true;
              if (suffix == ".gsc" || suffix == ".csc")
                name = name.substr(0, name.size() - 4);
            }
            const auto inst = is_csc ? game::SCRIPTINSTANCE_CLIENT
                                     : game::SCRIPTINSTANCE_SERVER;
            game::Scr_LoadScript(inst, name.c_str());
            if (is_csc)
              c->activated_csc++;
            else
              c->activated_gsc++;
          },
          &ctx, false);

      printf("[ bo3-bundle ] enumerated=%d matched=%d "
             "activated_gsc=%d activated_csc=%d\n",
             ctx.enumerated, ctx.matched, ctx.activated_gsc,
             ctx.activated_csc);
    });

    // bo3-bundle: list script_parse_tree assets in the pool. Useful for
    // discovering DS4C's specific script paths after loading its FF.
    // Usage: bundle_list_scripts [name_substr_filter] [limit]
    command::add("bundle_list_scripts",
                 [](const command::params &params) {
      const std::string filter = params.size() >= 2 ? params.get(1) : "";
      int limit = 200;
      if (params.size() >= 3) {
        try {
          limit = std::max(1, std::stoi(params.get(2)));
        } catch (...) { /* keep default */ }
      }

      struct ctx_t {
        std::string filter;
        int limit;
        int enumerated;
        int matched;
        int printed;
      };
      ctx_t ctx{filter, limit, 0, 0, 0};

      printf("[ bo3-bundle ] listing script_parse_tree assets (filter=%s, "
             "limit=%d)...\n",
             filter.empty() ? "<none>" : filter.c_str(), limit);

      game::DB_EnumXAssets(
          game::ASSET_TYPE_SCRIPTPARSETREE,
          [](game::XAssetHeader header, void *userdata) {
            auto *c = static_cast<ctx_t *>(userdata);
            c->enumerated++;
            if (!header.luaFile || !header.luaFile->name)
              return;
            const std::string name = header.luaFile->name;
            if (!c->filter.empty() &&
                name.find(c->filter) == std::string::npos)
              return;
            c->matched++;
            if (c->printed < c->limit) {
              printf("[ bo3-bundle ]   %s\n", name.c_str());
              c->printed++;
            }
          },
          &ctx, false);

      printf("[ bo3-bundle ] enumerated=%d matched=%d printed=%d "
             "(remaining=%d)\n",
             ctx.enumerated, ctx.matched, ctx.printed,
             ctx.matched - ctx.printed);
    });

    // bo3-bundle: explicit single-script activation for targeted testing.
    // Usage: bundle_scr_loadscript <inst_0_server_or_1_client> <name>
    command::add("bundle_scr_loadscript",
                 [](const command::params &params) {
      if (params.size() < 3) {
        printf("[ bo3-bundle ] Usage: bundle_scr_loadscript "
               "<inst_0_or_1> <script_name_no_ext>\n");
        return;
      }
      int inst_raw = 0;
      try {
        inst_raw = std::stoi(params.get(1));
      } catch (...) {
        printf("[ bo3-bundle ] couldn't parse inst\n");
        return;
      }
      const auto inst = inst_raw == 1 ? game::SCRIPTINSTANCE_CLIENT
                                      : game::SCRIPTINSTANCE_SERVER;
      const std::string name = params.get(2);
      printf("[ bo3-bundle ] Scr_LoadScript(inst=%d, name=\"%s\")\n",
             inst_raw, name.c_str());
      const auto handle = game::Scr_LoadScript(inst, name.c_str());
      printf("[ bo3-bundle ]   returned handle=%u (0=failed)\n", handle);
    });

    // bo3-bundle: master switch for the begin_load_scripts integration.
    // When ON, every begin_load_scripts call (i.e. every map init) drains
    // the g_pending_bundle_scripts set (populated by bundle_load_ff's pool
    // diff) through Scr_LoadScript at the correct phase. Default OFF.
    //
    // Usage: bundle_auto_activate_scripts <0|1>
    command::add("bundle_auto_activate_scripts",
                 [](const command::params &params) {
      const bool enable = (params.size() >= 2
                           && std::string(params.get(1)) != "0");
      g_auto_activate_scripts.store(enable, std::memory_order_relaxed);
      printf("[ bo3-bundle ] auto_activate_scripts: %s (drains during next "
             "begin_load_scripts, i.e. next map init)\n",
             enable ? "ON" : "OFF");
    });

    // bo3-bundle: inspect the pending DS4C-script activation set built by
    // bundle_load_ff's snapshot+diff.
    command::add("bundle_dump_pending_activations",
                 [](const command::params &) {
      std::lock_guard lock(g_pending_scripts_mutex);
      printf("[ bo3-bundle ] pending bundle activations: %zu entries\n",
             g_pending_bundle_scripts.size());
      int shown = 0;
      for (const auto &n : g_pending_bundle_scripts) {
        printf("[ bo3-bundle ]   %s\n", n.c_str());
        if (++shown >= 200) {
          printf("[ bo3-bundle ]   ... (truncated at 200)\n");
          break;
        }
      }
    });

    // bo3-bundle: dump the autoexec list at static 0x1432E6000.
    // Disasm of Scr_RunExports (0x1412D7F10) showed it iterates:
    //   rcx = *[0x1432E6000]  (head pointer)
    //   loop: [rcx+8] flags (mask 0x30000), [rcx+0xC] index into export table
    // We don't yet know the entry stride or how the iteration increments.
    // Dump raw bytes so we can analyze structure visually.
    //
    // Usage: bundle_dump_autoexec_list [byte_count_hex] [stride_hex]
    command::add("bundle_dump_autoexec_list",
                 [](const command::params &params) {
      int byte_count = 0x200;
      int stride = 0x10;
      if (params.size() >= 2) {
        try {
          byte_count = static_cast<int>(std::stoul(params.get(1), nullptr, 16));
        } catch (...) { /* keep default */ }
      }
      if (params.size() >= 3) {
        try {
          stride = static_cast<int>(std::stoul(params.get(2), nullptr, 16));
        } catch (...) { /* keep default */ }
      }
      if (byte_count < 16) byte_count = 16;
      if (stride < 4) stride = 4;

      const auto base = reinterpret_cast<uintptr_t>(
          GetModuleHandleA("BlackOps3.exe"));
      const auto list_global_addr =
          base + (0x1432E6000ULL - 0x140000000ULL);
      printf("[ bo3-bundle ] list_global @ 0x%llx (static 0x1432E6000)\n",
             static_cast<unsigned long long>(list_global_addr));

      const auto list_head = *reinterpret_cast<uintptr_t *>(list_global_addr);
      printf("[ bo3-bundle ] *list_global = 0x%llx\n",
             static_cast<unsigned long long>(list_head));

      if (!list_head) {
        printf("[ bo3-bundle ] list head is NULL\n");
        return;
      }

      dump_autoexec_list_seh(list_head, byte_count, stride);
    });

    // bo3-bundle: clear the pending activation set. Use if you want to
    // start fresh (e.g. between test cycles).
    command::add("bundle_clear_pending_activations",
                 [](const command::params &) {
      std::lock_guard lock(g_pending_scripts_mutex);
      const auto n = g_pending_bundle_scripts.size();
      g_pending_bundle_scripts.clear();
      printf("[ bo3-bundle ] cleared %zu pending activations\n", n);
    });

    // bo3-bundle (Path G phase 1a): extract DS4C scripts to disk under
    // renamed paths in BOIII's custom_scripts folder. For each captured
    // pending script:
    //   1. Fetch the asset via DB_FindXAssetHeader (returns DS4C's version
    //      since DS4C is at head of asset pool)
    //   2. Compute renamed disk path: original_path_no_ext + "_ds4c" + ext
    //   3. Write buffer bytes to disk
    // BOIII's load_scripts() will pick up these files during the next
    // begin_load_scripts (map init) and load them as custom scripts, which
    // calls Scr_LoadScript on each. Renamed names mean no asset-pool
    // collision with Solo. Phase 1a does NOT patch import tables, so
    // cross-references between DS4C scripts will still resolve to the
    // ORIGINAL names in the asset pool (which return DS4C's versions at
    // head). For pure DS4C-internal calls this works as long as DS4C's
    // assets remain at pool head.
    //
    // Usage: bundle_extract_ds4c
    // Files written to: %LOCALAPPDATA%\boiii\data\custom_scripts\zm\<renamed>
    command::add("bundle_extract_ds4c",
                 [](const command::params &) {
      std::set<std::string> names;
      {
        std::lock_guard lock(g_pending_scripts_mutex);
        names = g_pending_bundle_scripts;
      }
      if (names.empty()) {
        printf("[ bo3-bundle ] extract_ds4c: pending set is empty. Run "
               "bundle_load_ff for DS4C first to populate it.\n");
        return;
      }

      const auto out_root = game::get_appdata_path() / "data"
                            / "custom_scripts" / "zm";
      std::error_code ec;
      std::filesystem::create_directories(out_root, ec);
      if (ec) {
        printf("[ bo3-bundle ] extract_ds4c: failed to create %s: %s\n",
               out_root.string().c_str(), ec.message().c_str());
        return;
      }

      int written = 0;
      int skipped_missing = 0;
      int skipped_write_err = 0;
      for (const auto &full_name : names) {
        const auto header = game::DB_FindXAssetHeader(
            game::ASSET_TYPE_SCRIPTPARSETREE, full_name.c_str(), false, 0);
        const auto *rf = header.luaFile;
        if (!rf || !rf->buffer || rf->len <= 0) {
          printf("[ bo3-bundle ]   miss: %s\n", full_name.c_str());
          skipped_missing++;
          continue;
        }

        // Compute renamed path: "scripts/zm/_zm_cheats.gsc" ->
        // "_zm_cheats_ds4c.gsc" (flat under custom_scripts/zm/)
        const auto last_slash = full_name.find_last_of('/');
        std::string base = (last_slash == std::string::npos)
                           ? full_name : full_name.substr(last_slash + 1);
        std::string ext;
        const auto dot = base.find_last_of('.');
        if (dot != std::string::npos) {
          ext = base.substr(dot);
          base = base.substr(0, dot);
        }
        const auto renamed = base + "_ds4c" + ext;
        const auto out_path = out_root / renamed;

        std::ofstream out(out_path, std::ios::binary | std::ios::trunc);
        if (!out) {
          printf("[ bo3-bundle ]   write FAILED for %s\n",
                 out_path.string().c_str());
          skipped_write_err++;
          continue;
        }
        out.write(rf->buffer, rf->len);
        out.close();
        printf("[ bo3-bundle ]   wrote %d bytes -> %s\n", rf->len,
               renamed.c_str());
        written++;
      }

      printf("[ bo3-bundle ] extract_ds4c: %d written, %d miss, %d "
             "write-err (dir: %s)\n",
             written, skipped_missing, skipped_write_err,
             out_root.string().c_str());
    });

    // bo3-bundle: manual file-open trace toggle. Useful for instrumenting
    // arbitrary actions (mod load via Mods menu, map change, etc.) without
    // needing to trigger via bundle_load_ff. Pass 1 to enable, 0 to disable.
    command::add("bundle_trace_files",
                 [](const command::params &params) {
      bool enable = (params.size() >= 2
                     && std::string(params.get(1)) != "0");
      g_trace_file_opens.store(enable, std::memory_order_relaxed);
      printf("[ bo3-bundle ] file-open tracing: %s\n",
             enable ? "ON" : "OFF");
    });

    // bo3-bundle: when set, DB_UnloadXZone is short-circuited so loadMod
    // can engage its state-machine reset without actually unloading the
    // current mod's FFs. Set this BEFORE triggering a second-mod load.
    command::add("bundle_skip_unload",
                 [](const command::params &params) {
      bool enable = (params.size() >= 2
                     && std::string(params.get(1)) != "0");
      g_skip_unload.store(enable, std::memory_order_relaxed);
      g_skip_unload_count.store(0, std::memory_order_relaxed);
      printf("[ bo3-bundle ] skip_unload: %s\n", enable ? "ON" : "OFF");
    });

    // bo3-bundle: when set, zone_lookup() returns 22/23 for mod-tier
    // names (en_core_mod / core_mod) instead of the real index. Set this
    // before triggering the second-mod load to redirect it into fresh
    // slots and preserve mod 1 in slots 15/16.
    command::add("bundle_redirect_mod_slots",
                 [](const command::params &params) {
      bool enable = (params.size() >= 2
                     && std::string(params.get(1)) != "0");
      g_redirect_mod_slots.store(enable, std::memory_order_relaxed);
      g_redirect_count.store(0, std::memory_order_relaxed);
      printf("[ bo3-bundle ] redirect_mod_slots: %s\n",
             enable ? "ON" : "OFF");
    });

    // bo3-bundle: combined "dismiss splash + open MODS + load first mod"
    // in one go on a single window-focus context. Was getting flaky when
    // splitting into separate send_key calls because focus drifts between
    // commands and Windows blocks subsequent foreground steals.
    command::add("bundle_load_solo_menu",
                 [](const command::params & /*params*/) {
      const DWORD my_pid = GetCurrentProcessId();
      struct CB { DWORD pid; HWND found; } cb = { my_pid, nullptr };
      EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        auto *cb = reinterpret_cast<CB*>(lp);
        DWORD wpid = 0;
        GetWindowThreadProcessId(h, &wpid);
        if (wpid == cb->pid && IsWindowVisible(h)) {
          char buf[128] = {0};
          GetWindowTextA(h, buf, sizeof(buf) - 1);
          if (buf[0] != 0) { cb->found = h; return FALSE; }
        }
        return TRUE;
      }, reinterpret_cast<LPARAM>(&cb));
      HWND hwnd = cb.found;
      if (hwnd) {
        const DWORD my_tid = GetCurrentThreadId();
        const DWORD fg_tid = GetWindowThreadProcessId(GetForegroundWindow(),
                                                       nullptr);
        if (my_tid != fg_tid) AttachThreadInput(my_tid, fg_tid, TRUE);
        SetForegroundWindow(hwnd);
        BringWindowToTop(hwnd);
        if (my_tid != fg_tid) AttachThreadInput(my_tid, fg_tid, FALSE);
        Sleep(200);
      }
      auto send = [](WORD vk, WORD scan, bool ext = false) {
        INPUT in[2] = {};
        in[0].type = INPUT_KEYBOARD;
        in[0].ki.wVk = vk;
        in[0].ki.wScan = scan;
        in[0].ki.dwFlags = KEYEVENTF_SCANCODE
                          | (ext ? KEYEVENTF_EXTENDEDKEY : 0);
        in[1].type = INPUT_KEYBOARD;
        in[1].ki.wVk = vk;
        in[1].ki.wScan = scan;
        in[1].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP
                          | (ext ? KEYEVENTF_EXTENDEDKEY : 0);
        SendInput(2, in, sizeof(INPUT));
      };
      // Dismiss splash
      send(VK_RETURN, 0x1C);
      Sleep(2500);
      // 5x Down (MULTIPLAYER -> MODS)
      for (int i = 0; i < 5; ++i) { send(VK_DOWN, 0x50, true); Sleep(200); }
      Sleep(300);
      // Open MODS menu
      send(VK_RETURN, 0x1C);
      Sleep(800);
      // Load Solo (top item)
      send(VK_RETURN, 0x1C);
      printf("[ bo3-bundle ] load_solo_menu: sent dismiss + 5xDown + 2xEnter\n");
    });

    // bo3-bundle: send a synthetic keypress to the foreground BO3 window.
    // Same SendInput mechanism as bundle_dismiss_splash, but parameterized.
    // Usage: bundle_send_key <vk_hex> [count] [delay_ms]
    // Common: 0D=Enter, 1B=Esc, 28=Down, 26=Up, 25=Left, 27=Right
    command::add("bundle_send_key",
                 [](const command::params &params) {
      if (params.size() < 2) {
        printf("[ bo3-bundle ] Usage: bundle_send_key <vk_hex> [count] "
               "[delay_ms]\n");
        return;
      }
      WORD vk = 0;
      int count = 1, delay = 100;
      try {
        vk = static_cast<WORD>(std::stoul(params.get(1), nullptr, 16));
        if (params.size() >= 3) count = std::stoi(params.get(2));
        if (params.size() >= 4) delay = std::stoi(params.get(3));
      } catch (...) {
        printf("[ bo3-bundle ] couldn't parse args\n");
        return;
      }
      // Foreground BO3 window first.
      const DWORD my_pid = GetCurrentProcessId();
      struct CB { DWORD pid; HWND found; } cb = { my_pid, nullptr };
      EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        auto *cb = reinterpret_cast<CB*>(lp);
        DWORD wpid = 0;
        GetWindowThreadProcessId(h, &wpid);
        if (wpid == cb->pid && IsWindowVisible(h)) {
          char buf[128] = {0};
          GetWindowTextA(h, buf, sizeof(buf) - 1);
          if (buf[0] != 0) { cb->found = h; return FALSE; }
        }
        return TRUE;
      }, reinterpret_cast<LPARAM>(&cb));
      if (cb.found) {
        const DWORD my_tid = GetCurrentThreadId();
        const DWORD fg_tid = GetWindowThreadProcessId(GetForegroundWindow(),
                                                       nullptr);
        if (my_tid != fg_tid) AttachThreadInput(my_tid, fg_tid, TRUE);
        SetForegroundWindow(cb.found);
        BringWindowToTop(cb.found);
        if (my_tid != fg_tid) AttachThreadInput(my_tid, fg_tid, FALSE);
        Sleep(50);
      }
      // Scancodes for common keys -- BO3 cares about scancodes via Raw Input.
      auto vk_to_scan = [](WORD v) -> WORD {
        switch (v) {
          case VK_RETURN: return 0x1C;
          case VK_ESCAPE: return 0x01;
          case VK_DOWN:   return 0x50;
          case VK_UP:     return 0x48;
          case VK_LEFT:   return 0x4B;
          case VK_RIGHT:  return 0x4D;
          default: return MapVirtualKey(v, MAPVK_VK_TO_VSC);
        }
      };
      const WORD scan = vk_to_scan(vk);
      const bool extended = (vk == VK_DOWN || vk == VK_UP || vk == VK_LEFT
                              || vk == VK_RIGHT);
      for (int i = 0; i < count; ++i) {
        INPUT inputs[2] = {};
        inputs[0].type = INPUT_KEYBOARD;
        inputs[0].ki.wVk = vk;
        inputs[0].ki.wScan = scan;
        inputs[0].ki.dwFlags = KEYEVENTF_SCANCODE
                              | (extended ? KEYEVENTF_EXTENDEDKEY : 0);
        inputs[1].type = INPUT_KEYBOARD;
        inputs[1].ki.wVk = vk;
        inputs[1].ki.wScan = scan;
        inputs[1].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP
                              | (extended ? KEYEVENTF_EXTENDEDKEY : 0);
        SendInput(2, inputs, sizeof(INPUT));
        if (i < count - 1) Sleep(delay);
      }
      printf("[ bo3-bundle ] send_key vk=0x%02X scan=0x%02X count=%d\n",
             vk, scan, count);
    });

    // bo3-bundle: dismiss the "Press ENTER to Start" splash by injecting a
    // synthetic VK_RETURN via SendInput. BO3 uses Raw Input so we send
    // BOTH virtual-key and scancode forms. We also bring the BO3 window
    // to the foreground first -- SendInput targets the foreground window.
    command::add("bundle_dismiss_splash",
                 [](const command::params & /*params*/) {
      // Find the BO3 window. BOIII titles it "EZZ - <playername>".
      // Enumerate all windows belonging to our own process and pick the
      // visible one with a non-empty title.
      const DWORD my_pid = GetCurrentProcessId();
      struct CB { DWORD pid; HWND found; } cb = { my_pid, nullptr };
      EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        auto *cb = reinterpret_cast<CB*>(lp);
        DWORD wpid = 0;
        GetWindowThreadProcessId(h, &wpid);
        if (wpid == cb->pid && IsWindowVisible(h)) {
          char buf[128] = {0};
          GetWindowTextA(h, buf, sizeof(buf) - 1);
          // Skip the file-watcher's hidden console/tooling windows.
          if (buf[0] != 0) {
            cb->found = h;
            return FALSE;
          }
        }
        return TRUE;
      }, reinterpret_cast<LPARAM>(&cb));
      HWND hwnd = cb.found;
      printf("[ bo3-bundle ] dismiss_splash: BO3 hwnd=%p, fg=%p\n",
             hwnd, GetForegroundWindow());
      if (hwnd) {
        // Force window to foreground (AttachThreadInput trick to bypass
        // the foreground-lock).
        const DWORD my_tid = GetCurrentThreadId();
        const DWORD fg_tid = GetWindowThreadProcessId(GetForegroundWindow(),
                                                       nullptr);
        if (my_tid != fg_tid) {
          AttachThreadInput(my_tid, fg_tid, TRUE);
        }
        SetForegroundWindow(hwnd);
        BringWindowToTop(hwnd);
        if (my_tid != fg_tid) {
          AttachThreadInput(my_tid, fg_tid, FALSE);
        }
        Sleep(100);
      }
      // Send VK_RETURN + scancode 0x1C, both down and up. SendInput
      // injects at the lowest level above the kernel driver.
      INPUT inputs[4] = {};
      // Virtual-key Enter down
      inputs[0].type = INPUT_KEYBOARD;
      inputs[0].ki.wVk = VK_RETURN;
      // Scan-code Enter down
      inputs[1].type = INPUT_KEYBOARD;
      inputs[1].ki.wScan = 0x1C;
      inputs[1].ki.dwFlags = KEYEVENTF_SCANCODE;
      // Scan-code Enter up
      inputs[2].type = INPUT_KEYBOARD;
      inputs[2].ki.wScan = 0x1C;
      inputs[2].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
      // Virtual-key Enter up
      inputs[3].type = INPUT_KEYBOARD;
      inputs[3].ki.wVk = VK_RETURN;
      inputs[3].ki.dwFlags = KEYEVENTF_KEYUP;
      const UINT n = SendInput(4, inputs, sizeof(INPUT));
      printf("[ bo3-bundle ] dismiss_splash: SendInput sent %u/4 events "
             "(err=%lu)\n", n, n < 4 ? GetLastError() : 0);
    });

    // bo3-bundle: take the LAST node in the search-path linked list and
    // move it to the FRONT. Used after bundle_addpath to test whether
    // putting our newly-added entry at the head changes which mod's
    // content BO3 finds first via FS_FOpenFile.
    //
    // Linked list manipulation: O(N) walk to find tail + tail-1, detach,
    // re-insert at head. Safe as long as no other thread is iterating the
    // list during the swap (BOIII's fs_add_game_directory_stub doesn't
    // run concurrently with console commands, so we're OK in practice).
    command::add("bundle_promote_last",
                 [](const command::params & /*params*/) {
      struct sp_node {
        sp_node *next;
        const char *data;
      };
      auto **head_ptr = reinterpret_cast<sp_node **>(0x157A65308_g);
      sp_node *head = *head_ptr;
      if (!head || !head->next) {
        printf("[ bo3-bundle ] list is empty or has only one entry; "
               "nothing to promote.\n");
        return;
      }
      // Find tail and the node before it.
      sp_node *prev = head;
      while (prev->next->next) {
        prev = prev->next;
      }
      sp_node *tail = prev->next;
      // Detach tail and re-insert at head.
      prev->next = nullptr;
      tail->next = head;
      *head_ptr = tail;
      const char *path = tail->data ? tail->data : "(null)";
      const char *gamedir = tail->data ? tail->data + 0x100 : "(null)";
      printf("[ bo3-bundle ] promoted last entry to front: "
             "path=\"%.128s\" gamedir=\"%.32s\"\n", path, gamedir);
    });

    // bo3-bundle: dump BO3's FS search-path linked list. Walks from the
    // head pointer at static 0x157A6530F (computed from the rip-relative
    // load inside FS_AddSearchPath at 0x1422A2942) and prints each entry's
    // path + gamedir fields. Used to verify whether our manual
    // FS_AddSearchPath calls actually insert a node into the live list.
    command::add("bundle_dump_searchpath",
                 [](const command::params & /*params*/) {
      struct sp_node {
        sp_node *next;       // offset 0
        const char *data;    // offset 8 -- has path[0x100], gamedir[??]
      };
      // Head at static 0x157A65308 (computed from rip-relative load inside
      // FS_AddSearchPath: instruction starts at 0x1422A293B, 7 bytes long;
      // disp32 = 0x157C29C6 added to after-instruction RIP 0x1422A2942 ->
      // target 0x157A65308). Earlier off-by-7 read garbage/zero.
      auto **head_ptr = reinterpret_cast<sp_node **>(0x157A65308_g);
      sp_node *node = *head_ptr;
      printf("[ bo3-bundle ] === search path list (head @ %p, first node %p) ===\n",
             head_ptr, node);
      int i = 0;
      while (node && i < 50) {
        const char *data = node->data;
        if (data) {
          // Show first 0x80 chars of path field, then gamedir at offset 0x100.
          printf("[ bo3-bundle ]   [%d] path=\"%.128s\" gamedir=\"%.32s\"\n",
                 i, data, data + 0x100);
        } else {
          printf("[ bo3-bundle ]   [%d] (data=NULL)\n", i);
        }
        node = node->next;
        ++i;
      }
      printf("[ bo3-bundle ] === %d entries ===\n", i);
    });

    // bo3-bundle: directly invoke BO3's FS_AddSearchPath (the function we
    // identified at static 0x1422A28D0 -- the one that walks the search-path
    // linked list at [rip+0x157c29c6] and inserts a new node). Used to test
    // whether we can layer EXTRA workshop folders onto an already-loaded
    // mod's search path without going through loadMod.
    //
    // Usage: bundle_addpath <basepath> <gamedir>
    // Example: bundle_addpath "C:\Program Files (x86)\Steam\steamapps\workshop\content\311210" 3656779213
    command::add("bundle_addpath",
                 [](const command::params &params) {
      if (params.size() < 3) {
        printf("[ bo3-bundle ] Usage: bundle_addpath <basepath> <gamedir>\n");
        return;
      }
      using add_search_path_t = void(__cdecl *)(const char *path,
                                                 const char *gamedir,
                                                 int flag1, int flag2);
      const auto fn = reinterpret_cast<add_search_path_t>(0x1422A28D0_g);
      const std::string basepath = params.get(1);
      const std::string gamedir = params.get(2);
      printf("[ bo3-bundle ] FS_AddSearchPath(\"%s\", \"%s\", 0, 0)\n",
             basepath.c_str(), gamedir.c_str());
      fn(basepath.c_str(), gamedir.c_str(), 0, 0);
      printf("[ bo3-bundle ] returned. Use \\path or game's print to "
             "verify the new entry is in the list.\n");
    });

    // bo3-bundle: same as bundle_addpath but lets us pass arbitrary values
    // for the 3rd/4th args (which we passed 0,0 originally). Used to probe
    // whether either flag controls insertion position in the linked list
    // (vs. always appending). Possible behaviors when flag is non-zero:
    // - prepend instead of append
    // - format the gamedir differently (we saw a format-string branch in
    //   FS_AddSearchPath when r8d != 0)
    // - mark the entry "primary" / "writable" / "language-localized"
    //
    // Usage: bundle_addpath_flags <basepath> <gamedir> <flag1_dec> <flag2_dec>
    command::add("bundle_addpath_flags",
                 [](const command::params &params) {
      if (params.size() < 5) {
        printf("[ bo3-bundle ] Usage: bundle_addpath_flags <basepath> "
               "<gamedir> <flag1> <flag2>\n");
        return;
      }
      using add_search_path_t = void(__cdecl *)(const char *path,
                                                 const char *gamedir,
                                                 int flag1, int flag2);
      const auto fn = reinterpret_cast<add_search_path_t>(0x1422A28D0_g);
      const std::string basepath = params.get(1);
      const std::string gamedir = params.get(2);
      int flag1 = 0, flag2 = 0;
      try {
        flag1 = std::stoi(params.get(3));
        flag2 = std::stoi(params.get(4));
      } catch (...) {
        printf("[ bo3-bundle ] flags must be decimal integers\n");
        return;
      }
      printf("[ bo3-bundle ] FS_AddSearchPath(\"%s\", \"%s\", %d, %d)\n",
             basepath.c_str(), gamedir.c_str(), flag1, flag2);
      fn(basepath.c_str(), gamedir.c_str(), flag1, flag2);
      printf("[ bo3-bundle ] returned. Run bundle_dump_searchpath to "
             "see effect.\n");
    });

    // bo3-bundle multi-mod experiment.
    // Usage: bundle_test_loadmod <mod_folder_name> <reloadFS_0_or_1>
    // Calls game::loadMod() a second time after a mod is already loaded, to
    // probe whether BO3's runtime can handle multi-mod stacking. Pass the
    // empty string ("") as mod name to unload everything.
    command::add("bundle_test_loadmod",
                 [](const command::params &params) {
      if (params.size() < 2) {
        printf("[ bo3-bundle ] Usage: bundle_test_loadmod <mod_name> "
               "<reloadFS_0_or_1>\n");
        printf("[ bo3-bundle ] Currently loaded: \"%s\"\n",
               game::getPublisherIdFromLoadedMod());
        return;
      }
      const std::string mod_name = params.get(1);
      const bool reload_fs = (params.size() >= 3 && params.get(2)
                              && std::string(params.get(2)) != "0");
      printf("[ bo3-bundle ] before  loaded=\"%s\"\n",
             game::getPublisherIdFromLoadedMod());
      printf("[ bo3-bundle ] calling loadMod(\"%s\", reloadFS=%s)...\n",
             mod_name.c_str(), reload_fs ? "true" : "false");
      game::loadMod(game::LOCAL_CLIENT_0, mod_name.data(), reload_fs);
      // Wait for completion; loadMod is async when reloadFS=true.
      int waited_ms = 0;
      while (game::isModLoading(game::LOCAL_CLIENT_0) && waited_ms < 30000) {
        std::this_thread::sleep_for(100ms);
        waited_ms += 100;
      }
      printf("[ bo3-bundle ] after   loaded=\"%s\"  (waited %dms)\n",
             game::getPublisherIdFromLoadedMod(), waited_ms);
    });

    // bo3-bundle: find a literal string in BO3.exe AND list every xref to it
    // in one shot. Combines bundle_findstring + bundle_findxref so we don't
    // have to copy hex addresses by hand between commands.
    //
    // Usage: bundle_findstrxref <text>
    // Example: bundle_findstrxref "Could not find zone"
    //
    // For each match address it then scans BO3.exe's executable regions for
    // any RIP-relative instruction (lea/mov/call/jmp) that points to it.
    command::add("bundle_findstrxref",
                 [](const command::params &params) {
      if (params.size() < 2) {
        printf("[ bo3-bundle ] Usage: bundle_findstrxref <text>\n");
        return;
      }
      std::string needle;
      for (int i = 1; i < params.size(); ++i) {
        if (i > 1) needle += ' ';
        needle += params.get(i);
      }
      if (needle.empty()) {
        printf("[ bo3-bundle ] empty needle\n");
        return;
      }

      const HMODULE module = GetModuleHandleA("BlackOps3.exe");
      if (!module) {
        printf("[ bo3-bundle ] BlackOps3.exe module not found\n");
        return;
      }
      MODULEINFO mi{};
      if (!GetModuleInformation(GetCurrentProcess(), module, &mi,
                                sizeof(mi))) {
        printf("[ bo3-bundle ] GetModuleInformation failed\n");
        return;
      }
      const auto base = reinterpret_cast<uintptr_t>(mi.lpBaseOfDll);
      const auto end = base + mi.SizeOfImage;

      // Pass 1: find string addresses (capped low because we usually want
      // only the first few -- the format string itself).
      std::vector<uintptr_t> string_addrs;
      uintptr_t addr = base;
      while (addr < end && string_addrs.size() < 8) {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(reinterpret_cast<void *>(addr), &info,
                         sizeof(info)) == 0)
          break;
        const auto region_end = reinterpret_cast<uintptr_t>(info.BaseAddress)
                                + info.RegionSize;
        const bool readable =
            info.State == MEM_COMMIT &&
            (info.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ
                             | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY));
        if (readable && info.RegionSize > needle.size()) {
          const auto *bytes = reinterpret_cast<const char *>(info.BaseAddress);
          const size_t scan_len = info.RegionSize - needle.size();
          for (size_t i = 0; i < scan_len; ++i) {
            if (bytes[i] == needle[0]
                && std::memcmp(bytes + i, needle.data(), needle.size()) == 0) {
              const auto match_addr =
                  reinterpret_cast<uintptr_t>(info.BaseAddress) + i;
              string_addrs.push_back(match_addr);
              if (string_addrs.size() >= 8) break;
            }
          }
        }
        addr = region_end;
      }
      if (string_addrs.empty()) {
        printf("[ bo3-bundle ] no string matches\n");
        return;
      }
      printf("[ bo3-bundle ] string \"%s\" found at %zu addresses:\n",
             needle.c_str(), string_addrs.size());
      for (auto a : string_addrs) {
        printf("[ bo3-bundle ]   string @ 0x%016llx\n",
               static_cast<unsigned long long>(a));
      }

      // Pass 2: scan executable regions ONCE, checking each candidate
      // RIP-relative instruction against every string address.
      printf("[ bo3-bundle ] scanning xrefs (single pass)...\n");
      int total_hits = 0;
      addr = base;
      while (addr < end) {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(reinterpret_cast<void *>(addr), &info,
                         sizeof(info)) == 0)
          break;
        const auto region_end = reinterpret_cast<uintptr_t>(info.BaseAddress)
                                + info.RegionSize;
        const bool executable =
            info.State == MEM_COMMIT &&
            (info.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ
                             | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
        if (executable) {
          ud_t ud;
          ud_init(&ud);
          ud_set_mode(&ud, 64);
          ud_set_syntax(&ud, UD_SYN_INTEL);
          const auto *bytes =
              reinterpret_cast<const uint8_t *>(info.BaseAddress);
          for (size_t i = 0; i < info.RegionSize - 16; ++i) {
            const uint8_t b0 = bytes[i];
            const uint8_t b1 = bytes[i + 1];
            const uint8_t b2 = bytes[i + 2];
            bool maybe_riprel = false;
            int disp_offset = 0;
            if (b0 >= 0x48 && b0 <= 0x4F &&
                (b1 == 0x8d || b1 == 0x8b || b1 == 0x89) &&
                (b2 & 0xC7) == 0x05) {
              maybe_riprel = true;
              disp_offset = 3;
            } else if ((b0 == 0x8d || b0 == 0x8b || b0 == 0x89
                        || b0 == 0x88) &&
                       (b1 & 0xC7) == 0x05) {
              // 0x8d=LEA, 0x8b=MOV r,r/m, 0x89=MOV r/m,r (32-bit WRITE),
              // 0x88=MOV r/m8,r8 (8-bit write).
              maybe_riprel = true;
              disp_offset = 2;
            } else if (b0 == 0xc7 && (b1 & 0xC7) == 0x05) {
              // C7 /0 = MOV r/m32, imm32 (write constant to memory).
              // Disp is at +2; 4-byte immediate follows (so total 10
              // bytes), but we only care about the disp32 here.
              maybe_riprel = true;
              disp_offset = 2;
            } else if (b0 == 0xe8 || b0 == 0xe9) {
              maybe_riprel = true;
              disp_offset = 1;
            } else if (b0 == 0xff && (b1 == 0x15 || b1 == 0x25
                                       || b1 == 0x05 || b1 == 0x0d)) {
              // 0x15=CALL [rip+disp], 0x25=JMP [rip+disp],
              // 0x05=INC dword [rip+disp], 0x0d=DEC dword [rip+disp].
              maybe_riprel = true;
              disp_offset = 2;
            }
            if (!maybe_riprel) continue;
            const auto inst_addr =
                reinterpret_cast<uintptr_t>(info.BaseAddress) + i;
            ud_set_pc(&ud, inst_addr);
            ud_set_input_buffer(&ud, bytes + i, 16);
            if (!ud_disassemble(&ud)) continue;
            const unsigned len = ud_insn_len(&ud);
            int32_t disp = 0;
            std::memcpy(&disp, bytes + i + disp_offset, 4);
            const uint64_t computed = inst_addr + len
                + static_cast<int64_t>(disp);
            for (auto target : string_addrs) {
              if (computed == target) {
                printf("[ bo3-bundle ]   xref @ 0x%016llx -> str @ 0x%llx: "
                       "%s\n",
                       static_cast<unsigned long long>(inst_addr),
                       static_cast<unsigned long long>(target),
                       ud_insn_asm(&ud));
                ++total_hits;
                break;
              }
            }
          }
        }
        addr = region_end;
      }
      printf("[ bo3-bundle ] done (%d total xrefs)\n", total_hits);
    });

    // bo3-bundle: write arbitrary bytes to BO3.exe memory. Lets us experiment
    // with binary patches (NOP'ing a jump, flipping je->jmp, etc.) from the
    // console without rebuilding boiii.exe for every iteration.
    //
    // BOIII verifies BO3.exe is RWX at startup so we don't need to call
    // VirtualProtect, but we do anyway for safety against future BO3 builds.
    //
    // Usage: bundle_patch <hex_addr> <hex_byte0> [hex_byte1] ...
    // Example: bundle_patch 7ff77e123456 90 90 90 90 90  (NOP 5 bytes)
    // Example: bundle_patch 7ff77e123456 EB 1A           (jz -> jmp +0x1a)
    command::add("bundle_patch",
                 [](const command::params &params) {
      if (params.size() < 3) {
        printf("[ bo3-bundle ] Usage: bundle_patch <hex_addr> "
               "<hex_byte> [hex_byte ...]\n");
        return;
      }
      uint64_t addr = 0;
      try {
        addr = std::stoull(params.get(1), nullptr, 16);
      } catch (...) {
        printf("[ bo3-bundle ] couldn't parse address as hex\n");
        return;
      }
      std::vector<uint8_t> patch_bytes;
      for (int i = 2; i < params.size(); ++i) {
        try {
          unsigned long v = std::stoul(params.get(i), nullptr, 16);
          if (v > 0xFF) {
            printf("[ bo3-bundle ] byte %d out of range: 0x%lx\n", i, v);
            return;
          }
          patch_bytes.push_back(static_cast<uint8_t>(v));
        } catch (...) {
          printf("[ bo3-bundle ] couldn't parse arg %d as hex byte\n", i);
          return;
        }
      }

      auto *target = reinterpret_cast<uint8_t *>(addr);
      // Print original bytes first so we can manually undo if needed.
      printf("[ bo3-bundle ] patch @ 0x%llx, %zu bytes\n",
             static_cast<unsigned long long>(addr), patch_bytes.size());
      printf("[ bo3-bundle ]   before:");
      for (size_t i = 0; i < patch_bytes.size(); ++i) {
        printf(" %02X", target[i]);
      }
      printf("\n");

      DWORD old_protect = 0;
      if (!VirtualProtect(target, patch_bytes.size(),
                          PAGE_EXECUTE_READWRITE, &old_protect)) {
        printf("[ bo3-bundle ] VirtualProtect failed err=%lu\n",
               GetLastError());
        return;
      }
      std::memcpy(target, patch_bytes.data(), patch_bytes.size());
      DWORD dummy = 0;
      VirtualProtect(target, patch_bytes.size(), old_protect, &dummy);
      FlushInstructionCache(GetCurrentProcess(), target, patch_bytes.size());

      printf("[ bo3-bundle ]   after :");
      for (size_t i = 0; i < patch_bytes.size(); ++i) {
        printf(" %02X", target[i]);
      }
      printf("\n");
    });

    utils::hook::call(game::select(0x1420D6AA6, 0x1404E2936),
                      va_mods_path_stub);
    utils::hook::call(game::select(0x1420D6577, 0x1404E24A7),
                      va_user_content_path_stub);

    load_usermap_hook.create(game::select(0x1420D5700, 0x1404E18B0),
                             load_usermap_stub);
    utils::hook::call(game::select(0x1420D67F5, 0x1404E25F2),
                      load_usermap_content_stub);

    if (game::is_server()) {
      utils::hook::jump(0x1404E2635_g, load_mod_content_stub);
      return;
    }

    utils::hook::call(0x1420D6745_g, load_mod_content_stub);
    utils::hook::call(0x14135CD84_g, has_workshop_item_stub);
    setup_server_map_hook.create(*game::CL_SetupForNewServerMap,
                                 setup_server_map_stub);

    // bo3-bundle: hook the "Redundant %s asset" Com_PrintWarning call site
    // in the asset linker (client only). When bundle_load_ff is active,
    // this captures DS4C's collision-named script assets into
    // g_pending_bundle_scripts for later activation via Scr_LoadScript at
    // begin_load_scripts time. Static call addr 0x141422AF2.
    utils::hook::call(0x141422AF2_g, bundle_redundant_warning_hook);

    // bo3-bundle: observe Scr_EndLoadScripts native calls to learn the
    // pattern (when BO3 fires autoexec, what args/state). Logs only,
    // forwards to original cleanly. Static 0x1412C8020.
    scr_end_load_scripts_hook.create(0x1412C8020_g,
                                     scr_end_load_scripts_stub);

    // bo3-bundle: observe Scr_BeginLoadScripts native calls to triangulate
    // with End observations. Logs flag state BEFORE and AFTER the call,
    // revealing whether Begin actually sets the flag at 0x4d4e5cc as we
    // expected. Static 0x1412C7DF0.
    scr_begin_load_scripts_hook.create(0x1412C7DF0_g,
                                       scr_begin_load_scripts_stub);

    // bo3-bundle: observe Scr_LoadScript per-call. Filtered logging keeps
    // log readable: always logs our test/extract scripts, plus any call
    // that transitions the flag at 0x4d4e5cc. Reveals what consumes the
    // flag (presumably the actual autoexec dispatcher). Static 0x1412C83F0.
    scr_load_script_hook.create(0x1412C83F0_g, scr_load_script_stub);

    if (game::is_client()) {
      utils::hook::call(0x14135CDA1_g, com_error_missing_map_stub);
    }
  }

  void pre_destroy() override {
    g_cmd_watch_stop.store(true, std::memory_order_relaxed);
    if (g_cmd_watch_thread.joinable())
      g_cmd_watch_thread.join();
    downloading_workshop_item = false;
    dlc_thread_shutdown = true;
    dlc_cv.notify_one();
    if (dlc_popup_thread_obj.joinable())
      dlc_popup_thread_obj.join();
    if (download_thread.joinable())
      download_thread.join();
  }
};

// Exported probe so patches.cpp's Com_Error hook can ask whether we're in
// the middle of a multi-mod skip-unload test and should suppress fatal
// asset-load errors. At workshop:: scope (anon-ns vars are visible from
// the enclosing namespace).
bool is_skip_unload_active() {
  return g_skip_unload.load(std::memory_order_relaxed);
}

} // namespace workshop

REGISTER_COMPONENT(workshop::component)
