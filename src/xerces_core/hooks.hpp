#pragma once

#include <map>
#include <string>
#include <vector>

#include "diagnostics.hpp"
#include "manifest.hpp"
#include "planner.hpp"

namespace xerces_core {
// GSC hook chains (DESIGN.md 6.6): one ordered list of hooks per target
// function, the highest-priority mod outermost.
struct chain_link {
  std::string mod;
  std::string handler;
  hook_kind kind = hook_kind::wrap;
};

struct hook_chain {
  std::string target;
  std::vector<chain_link> links; // outermost first
};

struct hook_plan {
  std::vector<hook_chain> chains; // sorted by target
  diagnostics diags;
};

// `map_first` is rule 6 (the pack's map_scripts_first): false ranks the map's
// hooks below every mod's.
hook_plan plan_hooks(const plan &resolved,
                     const std::map<std::string, manifest> &manifests,
                     bool map_first = true);

// What a call to the target does, step by step: "before:<mod>",
// "enter:<mod>" for wraps, "replace:<mod>" or "original", "after:<mod>".
// Tests compare this with what the in-game probe records.
std::vector<std::string> simulate_chain(const hook_chain &chain);
} // namespace xerces_core
