#pragma once

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "diagnostics.hpp"

namespace xerces_core {
// A stringtable as BO3 CSV files hold it: comma separated, optional quotes.
struct table {
  std::vector<std::vector<std::string>> rows;
};

std::optional<table> parse_csv(std::string_view text,
                               std::string *error = nullptr);
std::string write_csv(const table &value);

// A value two mods both changed: the higher one wins and it is reported.
struct merge_conflict {
  std::string key; // row key, dvar name or JSON path
  std::size_t column = 0;
  std::string winner; // mod id
  std::string loser;  // mod id
  std::string winner_value;
  std::string loser_value;
};

struct merge_stats {
  std::map<std::string, std::size_t> added;   // mod -> rows or keys added
  std::map<std::string, std::size_t> changed; // mod -> rows or keys changed
  std::map<std::string, std::size_t> removed; // mod -> rows removed
  std::vector<merge_conflict> conflicts;
  diagnostics diags;

  std::size_t total(const std::map<std::string, std::size_t> &counts) const;
};

struct table_source {
  std::string mod;
  table data;
};

struct table_merge {
  table merged;
  merge_stats stats;
};

// Three-way merge (DESIGN.md 6.5). `sources` go from lowest to highest
// priority. `base` is the stock table; without one, the lowest source's copy
// is the base and rows missing from other copies are not taken as removals
// (those mods never knew about them).
table_merge merge_tables(const std::optional<table> &base,
                         const std::vector<table_source> &sources,
                         const std::vector<int> &key_columns);

struct text_source {
  std::string mod;
  std::string text;
};

struct text_merge {
  std::string merged;
  merge_stats stats;
};

// .cfg files by dvar name (set, seta, sets, setu); other lines are kept once.
text_merge merge_cfg(const std::optional<std::string> &base,
                     const std::vector<text_source> &sources);
// key=value files by key.
text_merge merge_key_value(const std::optional<std::string> &base,
                           const std::vector<text_source> &sources);
// JSON by key path; objects merge, everything else is a value.
text_merge merge_json_documents(const std::optional<std::string> &base,
                                const std::vector<text_source> &sources);

struct strings_source {
  std::string mod;
  std::map<std::string, std::string> entries;
};

struct strings_merge {
  std::map<std::string, std::string> merged;
  merge_stats stats;
};

// Localized strings by key; the same key in two mods is resolved by order.
strings_merge
merge_strings(const std::optional<std::map<std::string, std::string>> &base,
              const std::vector<strings_source> &sources);
} // namespace xerces_core
