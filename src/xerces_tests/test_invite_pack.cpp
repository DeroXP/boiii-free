#include "harness.hpp"

#include <string>
#include <vector>

#include <xerces_core/invite_pack.hpp>

using namespace xerces_core;

namespace {
installed_content item(const std::string &id, const std::string &folder,
                       std::optional<std::uint64_t> workshop_id = {}) {
  installed_content out;
  out.info.id = id;
  out.info.workshop_id = workshop_id;
  out.folder = folder;
  return out;
}

pack_mod mod(const std::string &id, std::optional<std::uint64_t> workshop_id) {
  pack_mod out;
  out.id = id;
  out.workshop_id = workshop_id;
  return out;
}
} // namespace

TEST_CASE("invite pack: what is here, what is missing, in the pack's order") {
  catalog_state catalog;
  catalog.items = {item("perks.plus", "2911000001", 2911000001),
                   item("ws.2911000002", "2911000002"),
                   item("my.local.mod", "my_local_mod")};
  catalog.sources = {"workshop", "workshop", "mods"};

  pack value;
  value.mods = {mod("perks.plus", 2911000001),
                // Installed as a legacy item: matched by its Workshop folder.
                mod("guns.pack", 2911000002), mod("maps.extra", 2911000003),
                mod("host.only", std::nullopt), mod("my.local.mod", {})};
  value.order = {"perks.plus", "guns.pack", "maps.extra", "host.only",
                 "my.local.mod"};

  const invite_pack_check check = check_invite_pack(value, catalog);
  CHECK_EQ(check.launch_mods,
           (std::vector<std::string>{"perks.plus", "ws.2911000002",
                                     "my.local.mod"}));
  REQUIRE_EQ(check.missing.size(), std::size_t(2));
  CHECK_EQ(check.missing[0].id, std::string("maps.extra"));
  CHECK_EQ(check.missing[0].workshop_id.value_or(0), std::uint64_t(2911000003));
  CHECK_EQ(check.missing[1].id, std::string("host.only"));
  CHECK(!check.missing[1].workshop_id.has_value());
  CHECK(!check.complete());
  CHECK_EQ(check.not_on_workshop(), std::size_t(1));

  // A folder in mods\ named like a Workshop id is not the Workshop item.
  catalog_state local;
  local.items = {item("someone.else", "2911000003")};
  local.sources = {"mods"};
  pack one;
  one.mods = {mod("maps.extra", 2911000003)};
  one.order = {"maps.extra"};
  CHECK_EQ(check_invite_pack(one, local).missing.size(), std::size_t(1));

  CHECK_EQ(workshop_page_url(2911000003),
           std::string("steam://url/CommunityFilePage/2911000003"));
}

TEST_CASE("invite pack: an empty pack needs nothing") {
  const invite_pack_check check = check_invite_pack(pack{}, catalog_state{});
  CHECK(check.complete());
  CHECK(check.launch_mods.empty());
}
