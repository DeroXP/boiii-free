#include "merge.hpp"

#include <algorithm>
#include <cctype>
#include <functional>
#include <limits>
#include <set>

#include "json.hpp"

namespace xerces_core {
namespace {
constexpr std::size_t whole_row = std::numeric_limits<std::size_t>::max();

const std::string &cell(const std::vector<std::string> &row,
                        std::size_t column) {
  static const std::string empty;
  return column < row.size() ? row[column] : empty;
}

struct keyed_row {
  std::string key;
  const std::vector<std::string> *row;
};

// Rows are keyed by their key columns; repeats of a key are told apart by
// their occurrence, so the n-th repeat in one table matches the n-th in
// another.
std::vector<keyed_row> key_rows(const table &value,
                                const std::vector<int> &columns) {
  std::vector<keyed_row> out;
  std::map<std::string, int> seen;
  for (const auto &row : value.rows) {
    std::string key;
    for (const int column : columns) {
      key += cell(row, static_cast<std::size_t>(column));
      key.push_back('\x1f');
    }
    const int occurrence = seen[key]++;
    key += std::to_string(occurrence);
    out.push_back({std::move(key), &row});
  }
  return out;
}

std::string display_key(const std::string &key) {
  std::string out;
  const std::size_t last = key.rfind('\x1f');
  for (std::size_t i = 0;
       i < key.size() && (last == std::string::npos || i < last); ++i) {
    out.push_back(key[i] == '\x1f' ? ',' : key[i]);
  }
  if (last != std::string::npos && key.substr(last + 1) != "0") {
    out += " #" + key.substr(last + 1);
  }
  return out;
}

void count(std::map<std::string, std::size_t> &counts, const std::string &mod,
           std::size_t amount) {
  if (amount > 0) {
    counts[mod] += amount;
  }
}

std::string trim(std::string_view text) {
  std::size_t start = 0;
  std::size_t end = text.size();
  while (start < end && std::isspace(static_cast<unsigned char>(text[start]))) {
    ++start;
  }
  while (end > start &&
         std::isspace(static_cast<unsigned char>(text[end - 1]))) {
    --end;
  }
  return std::string(text.substr(start, end - start));
}

std::vector<std::string> split_lines(std::string_view text) {
  std::vector<std::string> lines;
  std::size_t start = 0;
  while (start < text.size()) {
    std::size_t end = text.find('\n', start);
    if (end == std::string_view::npos) {
      end = text.size();
    }
    std::string line(text.substr(start, end - start));
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    lines.push_back(std::move(line));
    start = end + 1;
  }
  return lines;
}

std::string lower(std::string text) {
  std::ranges::transform(text, text.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return text;
}

// A line-based file where some lines set a key. Lines without a key are kept
// once each, in the order they first appear.
struct keyed_line {
  std::string key; // empty for lines that set nothing
  std::string value;
  std::string line;
};

using line_parser = std::function<std::vector<keyed_line>(std::string_view)>;

std::vector<keyed_line> parse_cfg_lines(std::string_view text) {
  std::vector<keyed_line> out;
  for (const auto &raw : split_lines(text)) {
    const std::string line = trim(raw);
    keyed_line entry{"", "", line};
    const std::size_t space = line.find_first_of(" \t");
    if (space != std::string::npos) {
      const std::string command = lower(line.substr(0, space));
      if (command == "set" || command == "seta" || command == "sets" ||
          command == "setu") {
        const std::string rest = trim(std::string_view(line).substr(space));
        const std::size_t name_end = rest.find_first_of(" \t");
        entry.key = lower(rest.substr(0, name_end));
        entry.value = name_end == std::string::npos
                          ? ""
                          : trim(std::string_view(rest).substr(name_end));
      }
    }
    out.push_back(std::move(entry));
  }
  return out;
}

std::vector<keyed_line> parse_key_value_lines(std::string_view text) {
  std::vector<keyed_line> out;
  for (const auto &raw : split_lines(text)) {
    const std::string line = trim(raw);
    keyed_line entry{"", "", line};
    if (!line.empty() && line[0] != '#' && line[0] != ';') {
      if (const std::size_t eq = line.find('='); eq != std::string::npos) {
        entry.key = trim(std::string_view(line).substr(0, eq));
        entry.value = trim(std::string_view(line).substr(eq + 1));
      }
    }
    out.push_back(std::move(entry));
  }
  return out;
}

text_merge merge_lines(const std::optional<std::string> &base,
                       const std::vector<text_source> &sources,
                       const line_parser &parse) {
  text_merge out;
  if (!base && sources.empty()) {
    return out;
  }
  const std::vector<keyed_line> base_lines =
      parse(base ? *base : sources.front().text);
  std::map<std::string, std::string> base_values;
  std::vector<std::string> order; // keys, or "\x1e<line>" for plain lines
  std::map<std::string, keyed_line> current;
  std::map<std::string, std::string> owner;
  std::set<std::string> plain;
  for (const auto &line : base_lines) {
    if (line.key.empty()) {
      if (plain.insert(line.line).second) {
        order.push_back("\x1e" + line.line);
      }
      continue;
    }
    if (!current.contains(line.key)) {
      order.push_back(line.key);
    }
    base_values[line.key] = line.value;
    current[line.key] = line;
  }

  for (std::size_t i = base ? 0 : 1; i < sources.size(); ++i) {
    const auto &source = sources[i];
    std::size_t added = 0;
    std::size_t changed = 0;
    for (const auto &line : parse(source.text)) {
      if (line.key.empty()) {
        if (plain.insert(line.line).second) {
          order.push_back("\x1e" + line.line);
        }
        continue;
      }
      const auto in_base = base_values.find(line.key);
      if (in_base != base_values.end() && in_base->second == line.value) {
        continue; // unchanged from the base
      }
      const auto existing = current.find(line.key);
      if (existing == current.end()) {
        order.push_back(line.key);
        ++added;
      } else {
        if (const auto prior = owner.find(line.key);
            prior != owner.end() && prior->second != source.mod &&
            existing->second.value != line.value) {
          out.stats.conflicts.push_back({line.key, 0, source.mod, prior->second,
                                         line.value, existing->second.value});
        }
        ++changed;
      }
      current[line.key] = line;
      owner[line.key] = source.mod;
    }
    count(out.stats.added, source.mod, added);
    count(out.stats.changed, source.mod, changed);
  }

  for (const auto &entry : order) {
    out.merged +=
        entry.starts_with('\x1e') ? entry.substr(1) : current.at(entry).line;
    out.merged.push_back('\n');
  }
  return out;
}

using json_owners = std::map<std::string, std::pair<std::string, std::string>>;

// Another mod that set this path, a value it sits inside, or a value inside
// it: a lower mod that added {"x":1} owns "/new/x" too.
const json_owners::value_type *other_owner(const json_owners &owners,
                                           const std::string &path,
                                           const std::string &mod) {
  for (std::string at = path;; at.erase(at.rfind('/'))) {
    if (const auto it = owners.find(at);
        it != owners.end() && it->second.first != mod) {
      return &*it;
    }
    if (at.empty()) {
      break;
    }
  }
  const std::string inside = path + "/";
  for (auto it = owners.lower_bound(inside);
       it != owners.end() && it->first.starts_with(inside); ++it) {
    if (it->second.first != mod) {
      return &*it;
    }
  }
  return nullptr;
}

void merge_json_value(rapidjson::Value &result, const rapidjson::Value *base,
                      const rapidjson::Value &source, const std::string &path,
                      const std::string &mod, json_owners &owners,
                      merge_stats &stats, std::size_t &added,
                      std::size_t &changed, json::allocator &alloc) {
  if (source.IsObject() && result.IsObject()) {
    for (auto it = source.MemberBegin(); it != source.MemberEnd(); ++it) {
      const std::string key(it->name.GetString(), it->name.GetStringLength());
      const std::string child_path = json::pointer(path, key);
      const rapidjson::Value *base_child =
          base != nullptr ? json::member(*base, key) : nullptr;
      const auto existing = result.FindMember(it->name);
      if (existing == result.MemberEnd()) {
        result.AddMember(rapidjson::Value(it->name, alloc),
                         rapidjson::Value(it->value, alloc), alloc);
        owners[child_path] = {mod, json::canonical(it->value)};
        ++added;
        continue;
      }
      merge_json_value(existing->value, base_child, it->value, child_path, mod,
                       owners, stats, added, changed, alloc);
    }
    return;
  }
  const std::string value = json::canonical(source);
  if (base != nullptr && json::canonical(*base) == value) {
    return; // unchanged from the base
  }
  const std::string previous = json::canonical(result);
  if (previous == value) {
    return;
  }
  if (const auto *prior = other_owner(owners, path, mod)) {
    stats.conflicts.push_back(
        {path, 0, mod, prior->second.first, value, previous});
  }
  result.CopyFrom(source, alloc);
  // This mod now owns everything under the path.
  const std::string inside = path + "/";
  for (auto it = owners.lower_bound(inside);
       it != owners.end() && it->first.starts_with(inside);) {
    it = owners.erase(it);
  }
  owners[path] = {mod, value};
  ++changed;
}
} // namespace

std::size_t
merge_stats::total(const std::map<std::string, std::size_t> &counts) const {
  std::size_t sum = 0;
  for (const auto &[mod, amount] : counts) {
    sum += amount;
  }
  return sum;
}

std::optional<table> parse_csv(std::string_view text, std::string *error) {
  table out;
  std::vector<std::string> row;
  std::string field;
  bool quoted = false;
  bool field_started = false;
  std::size_t i = 0;
  const auto end_row = [&] {
    row.push_back(std::move(field));
    field.clear();
    out.rows.push_back(std::move(row));
    row.clear();
    field_started = false;
  };
  while (i < text.size()) {
    const char c = text[i];
    if (quoted) {
      if (c == '"') {
        if (i + 1 < text.size() && text[i + 1] == '"') {
          field.push_back('"');
          i += 2;
          continue;
        }
        quoted = false;
        ++i;
        continue;
      }
      field.push_back(c);
      ++i;
      continue;
    }
    if (c == '"' && field.empty() && !field_started) {
      quoted = true;
      field_started = true;
      ++i;
      continue;
    }
    if (c == ',') {
      row.push_back(std::move(field));
      field.clear();
      field_started = false;
      ++i;
      continue;
    }
    if (c == '\r' || c == '\n') {
      if (!row.empty() || field_started) { // a blank line is not a row
        end_row();
      }
      i += (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n') ? 2 : 1;
      continue;
    }
    field.push_back(c);
    field_started = true;
    ++i;
  }
  if (quoted) {
    if (error != nullptr) {
      *error = "a quoted field is not closed";
    }
    return std::nullopt;
  }
  if (field_started || !field.empty() || !row.empty()) {
    end_row();
  }
  return out;
}

std::string write_csv(const table &value) {
  std::string out;
  for (const auto &row : value.rows) {
    if (row.size() == 1 && row[0].empty()) {
      out += "\"\"\n"; // quoted, or it would read back as a blank line
      continue;
    }
    for (std::size_t i = 0; i < row.size(); ++i) {
      if (i > 0) {
        out.push_back(',');
      }
      const std::string &field = row[i];
      if (field.find_first_of(",\"\r\n") == std::string::npos) {
        out += field;
        continue;
      }
      out.push_back('"');
      for (const char c : field) {
        if (c == '"') {
          out.push_back('"');
        }
        out.push_back(c);
      }
      out.push_back('"');
    }
    out.push_back('\n');
  }
  return out;
}

table_merge merge_tables(const std::optional<table> &base,
                         const std::vector<table_source> &sources,
                         const std::vector<int> &key_columns) {
  table_merge out;
  const std::vector<int> columns =
      key_columns.empty() ? std::vector<int>{0} : key_columns;
  const bool real_base = base.has_value();
  if (!real_base && sources.empty()) {
    return out;
  }
  const table &base_table = real_base ? *base : sources.front().data;

  std::vector<std::string> order;
  std::map<std::string, std::vector<std::string>> rows;
  std::map<std::string, const std::vector<std::string> *> base_rows;
  for (const auto &[key, row] : key_rows(base_table, columns)) {
    order.push_back(key);
    rows[key] = *row;
    base_rows[key] = row;
  }
  std::map<std::pair<std::string, std::size_t>,
           std::pair<std::string, std::string>>
      cell_owner;
  std::map<std::string, std::string> added_owner;
  std::map<std::string, std::string> removed_by;

  for (std::size_t i = real_base ? 0 : 1; i < sources.size(); ++i) {
    const auto &source = sources[i];
    std::size_t added = 0;
    std::size_t changed = 0;
    std::size_t removed = 0;
    std::set<std::string> present;
    for (const auto &[key, row_ptr] : key_rows(source.data, columns)) {
      const auto &row = *row_ptr;
      present.insert(key);
      if (const auto in_base = base_rows.find(key);
          in_base != base_rows.end()) {
        const auto &base_row = *in_base->second;
        bool row_changed = false;
        const std::size_t width = std::max(base_row.size(), row.size());
        for (std::size_t column = 0; column < width; ++column) {
          const std::string &base_value = cell(base_row, column);
          const std::string &value = cell(row, column);
          if (base_value == value) {
            continue;
          }
          row_changed = true;
          const auto owner = cell_owner.find({key, column});
          if (owner != cell_owner.end() && owner->second.first != source.mod &&
              owner->second.second != value) {
            out.stats.conflicts.push_back({display_key(key), column, source.mod,
                                           owner->second.first, value,
                                           owner->second.second});
          }
          auto &target = rows[key];
          if (target.size() <= column) {
            target.resize(column + 1);
          }
          target[column] = value;
          cell_owner[{key, column}] = {source.mod, value};
        }
        if (row_changed) {
          ++changed;
          if (const auto gone = removed_by.find(key);
              gone != removed_by.end()) {
            out.stats.conflicts.push_back({display_key(key), whole_row,
                                           source.mod, gone->second,
                                           "(changed)", "(removed)"});
            removed_by.erase(gone);
          }
        }
        continue;
      }
      if (const auto existing = rows.find(key); existing != rows.end()) {
        if (existing->second != row) {
          const std::size_t width =
              std::max(existing->second.size(), row.size());
          for (std::size_t column = 0; column < width; ++column) {
            if (cell(existing->second, column) != cell(row, column)) {
              out.stats.conflicts.push_back(
                  {display_key(key), column, source.mod, added_owner[key],
                   cell(row, column), cell(existing->second, column)});
            }
          }
          existing->second = row;
          added_owner[key] = source.mod;
          ++changed;
        }
        continue;
      }
      order.push_back(key);
      rows[key] = row;
      added_owner[key] = source.mod;
      ++added;
    }
    if (real_base) {
      for (const auto &[key, row] : base_rows) {
        if (present.contains(key) || removed_by.contains(key)) {
          continue;
        }
        for (const auto &[cell_key, owner] : cell_owner) {
          if (cell_key.first == key && owner.first != source.mod) {
            out.stats.conflicts.push_back({display_key(key), whole_row,
                                           source.mod, owner.first, "(removed)",
                                           "(changed)"});
            break;
          }
        }
        removed_by[key] = source.mod;
        ++removed;
      }
    }
    count(out.stats.added, source.mod, added);
    count(out.stats.changed, source.mod, changed);
    count(out.stats.removed, source.mod, removed);
  }

  for (const auto &key : order) {
    if (!removed_by.contains(key)) {
      out.merged.rows.push_back(rows[key]);
    }
  }
  return out;
}

text_merge merge_cfg(const std::optional<std::string> &base,
                     const std::vector<text_source> &sources) {
  return merge_lines(base, sources, parse_cfg_lines);
}

text_merge merge_key_value(const std::optional<std::string> &base,
                           const std::vector<text_source> &sources) {
  return merge_lines(base, sources, parse_key_value_lines);
}

text_merge merge_json_documents(const std::optional<std::string> &base,
                                const std::vector<text_source> &sources) {
  text_merge out;
  if (!base && sources.empty()) {
    return out;
  }
  rapidjson::Document result;
  rapidjson::Document base_doc;
  const std::string &start = base ? *base : sources.front().text;
  const auto parsed_start = json::parse(start);
  if (!parsed_start.ok) {
    out.stats.diags.error("merge.json.invalid", "",
                          "The base document is not valid JSON.");
    return out;
  }
  result.CopyFrom(parsed_start.document, result.GetAllocator());
  base_doc.CopyFrom(parsed_start.document, base_doc.GetAllocator());
  json_owners owners;
  for (std::size_t i = base ? 0 : 1; i < sources.size(); ++i) {
    const auto parsed = json::parse(sources[i].text);
    if (!parsed.ok) {
      out.stats.diags.error("merge.json.invalid", sources[i].mod,
                            sources[i].mod + "'s copy is not valid JSON.");
      continue;
    }
    std::size_t added = 0;
    std::size_t changed = 0;
    merge_json_value(result, &base_doc, parsed.document, "", sources[i].mod,
                     owners, out.stats, added, changed, result.GetAllocator());
    count(out.stats.added, sources[i].mod, added);
    count(out.stats.changed, sources[i].mod, changed);
  }
  out.merged = json::pretty(result) + "\n";
  return out;
}

strings_merge
merge_strings(const std::optional<std::map<std::string, std::string>> &base,
              const std::vector<strings_source> &sources) {
  strings_merge out;
  if (!base && sources.empty()) {
    return out;
  }
  const auto &start = base ? *base : sources.front().entries;
  out.merged = start;
  std::map<std::string, std::string> owner;
  for (std::size_t i = base ? 0 : 1; i < sources.size(); ++i) {
    const auto &source = sources[i];
    std::size_t added = 0;
    std::size_t changed = 0;
    for (const auto &[key, text] : source.entries) {
      if (const auto in_base = start.find(key);
          in_base != start.end() && in_base->second == text) {
        continue;
      }
      const auto existing = out.merged.find(key);
      if (existing == out.merged.end()) {
        ++added;
      } else if (existing->second != text) {
        if (const auto prior = owner.find(key);
            prior != owner.end() && prior->second != source.mod) {
          out.stats.conflicts.push_back(
              {key, 0, source.mod, prior->second, text, existing->second});
        }
        ++changed;
      }
      out.merged[key] = text;
      owner[key] = source.mod;
    }
    count(out.stats.added, source.mod, added);
    count(out.stats.changed, source.mod, changed);
  }
  return out;
}
} // namespace xerces_core
