#include "sigruntime.hpp"

#include <algorithm>
#include <cstdio>
#include <utility>

#include "sha256.hpp"

namespace xerces_core {
namespace {
std::string hex(std::uint64_t value) {
  char text[24];
  std::snprintf(text, sizeof(text), "0x%llX",
                static_cast<unsigned long long>(value));
  return text;
}
} // namespace

std::string signature_cache_name(std::uint32_t build_checksum,
                                 std::string_view registry_text) {
  // 16 hex digits (64 bits) tell registries apart; this is a cache key, not
  // a security check.
  return "signatures-" + hex(build_checksum) + "-" +
         sha256::hex(registry_text).substr(0, 16) + ".json";
}

bool cached_signatures_agree(const signature_report &cached,
                             const std::vector<signature> &signatures,
                             std::uint32_t build_checksum, std::string *why) {
  const auto disagree = [&](std::string text) {
    if (why != nullptr) {
      *why = std::move(text);
    }
    return false;
  };
  for (const auto &sig : signatures) {
    if (cached.find(sig.id) == nullptr) {
      return disagree(sig.id + " is not in the cache");
    }
  }
  for (const auto &result : cached.results) {
    const auto sig = std::ranges::find_if(
        signatures, [&](const signature &s) { return s.id == result.id; });
    if (sig == signatures.end()) {
      return disagree(result.id + " is no longer in the registry");
    }
    if (const auto known = sig->known.find(build_checksum);
        known != sig->known.end() && known->second != result.rva) {
      return disagree(result.id + " is cached at " + hex(result.rva) +
                      " but the registry's known address for this build is " +
                      hex(known->second));
    }
  }
  return true;
}

std::vector<std::string>
require_signatures(signature_report &report, std::string_view feature,
                   std::span<const std::string_view> ids,
                   std::string_view absent) {
  std::vector<std::string> added;
  for (const std::string_view id : ids) {
    const signature_result *result = report.find(id);
    if (result != nullptr && result->ok) {
      continue;
    }
    if (result == nullptr) {
      signature_result missing;
      missing.id = std::string(id);
      missing.symbol = std::string(id); // no registry entry names the symbol
      missing.error = std::string(absent);
      report.results.push_back(std::move(missing));
    }
    // Looked up only now, so a feature that misses nothing gets no key.
    std::vector<std::string> &off = report.disabled[std::string(feature)];
    if (std::ranges::find(off, id) == off.end()) {
      off.emplace_back(id);
      added.emplace_back(id);
    }
  }
  return added;
}
} // namespace xerces_core
