#include "hooks.hpp"

#include <algorithm>

namespace xerces_core {
hook_plan plan_hooks(const plan &resolved,
                     const std::map<std::string, manifest> &manifests) {
  hook_plan out;
  std::vector<std::string> order;
  if (resolved.map) {
    order.push_back(resolved.map->id); // maps rank first for scripts (6.3)
  }
  for (const auto &entry : resolved.stack) {
    order.push_back(entry.id);
  }

  std::map<std::string, hook_chain> chains;
  for (const auto &id : order) {
    const auto found = manifests.find(id);
    if (found == manifests.end()) {
      continue;
    }
    for (const auto &hook : found->second.hooks) {
      auto &chain = chains[hook.target];
      chain.target = hook.target;
      if (hook.kind == hook_kind::replace) {
        const auto existing =
            std::ranges::find_if(chain.links, [](const chain_link &link) {
              return link.kind == hook_kind::replace;
            });
        if (existing != chain.links.end()) {
          out.diags.error("hooks.replace.conflict", hook.target,
                          found->second.name + " and " + existing->mod +
                              " both replace " + hook.target + "; " +
                              existing->mod + " is higher and wins.");
          continue;
        }
      }
      chain.links.push_back({id, hook.handler, hook.kind});
    }
  }
  for (auto &[target, chain] : chains) {
    out.chains.push_back(std::move(chain));
  }
  return out;
}

std::vector<std::string> simulate_chain(const hook_chain &chain) {
  std::vector<std::string> steps;
  for (const auto &link : chain.links) {
    if (link.kind == hook_kind::before) {
      steps.push_back("before:" + link.mod);
    }
  }
  for (const auto &link : chain.links) {
    if (link.kind == hook_kind::wrap) {
      steps.push_back("enter:" + link.mod);
    }
  }
  const auto replacement =
      std::ranges::find_if(chain.links, [](const chain_link &link) {
        return link.kind == hook_kind::replace;
      });
  steps.push_back(replacement == chain.links.end()
                      ? "original"
                      : "replace:" + replacement->mod);
  for (const auto &link : chain.links) {
    if (link.kind == hook_kind::after) {
      steps.push_back("after:" + link.mod);
    }
  }
  return steps;
}
} // namespace xerces_core
