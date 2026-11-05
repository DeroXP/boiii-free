#include "log.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <format>
#include <vector>

#include "json.hpp"

namespace xerces_core {
namespace {
std::string replace_all_case_insensitive(std::string text,
                                         std::string_view needle,
                                         std::string_view replacement) {
  if (needle.empty()) {
    return text;
  }
  const auto lower = [](std::string value) {
    std::ranges::transform(value, value.begin(), [](unsigned char c) {
      return static_cast<char>(std::tolower(c));
    });
    return value;
  };
  const std::string haystack = lower(text);
  const std::string pattern = lower(std::string(needle));
  std::string out;
  std::size_t start = 0;
  while (true) {
    const std::size_t found = haystack.find(pattern, start);
    if (found == std::string::npos) {
      out.append(text, start, std::string::npos);
      return out;
    }
    out.append(text, start, found - start);
    out.append(replacement);
    start = found + pattern.size();
  }
}

std::string profile_from_environment() {
  char *value = nullptr;
  std::size_t size = 0;
  if (_dupenv_s(&value, &size, "USERPROFILE") != 0 || value == nullptr) {
    return {};
  }
  std::string out(value);
  std::free(value);
  return out;
}
} // namespace

const char *to_string(log_level level) {
  switch (level) {
  case log_level::trace:
    return "trace";
  case log_level::debug:
    return "debug";
  case log_level::info:
    return "info";
  case log_level::warn:
    return "warn";
  case log_level::error:
    return "error";
  }
  return "info";
}

std::string redact_profile(std::string_view text, std::string_view profile) {
  if (profile.size() < 3) {
    return std::string(text);
  }
  std::string backslash(profile);
  std::ranges::replace(backslash, '/', '\\');
  std::string forward(backslash);
  std::ranges::replace(forward, '\\', '/');
  std::string escaped;
  for (const char c : backslash) {
    escaped += c == '\\' ? "\\\\" : std::string(1, c);
  }
  std::string out(text);
  out = replace_all_case_insensitive(std::move(out), escaped, "%USERPROFILE%");
  out =
      replace_all_case_insensitive(std::move(out), backslash, "%USERPROFILE%");
  out = replace_all_case_insensitive(std::move(out), forward, "%USERPROFILE%");
  return out;
}

json_log::json_log(std::string role, std::string session)
    : role_(std::move(role)), session_(std::move(session)),
      profile_(profile_from_environment()) {}

std::string json_log::now_utc() {
  const auto now = std::chrono::system_clock::now();
  return std::format("{:%Y-%m-%dT%H:%M:%S}Z",
                     std::chrono::floor<std::chrono::milliseconds>(now));
}

std::string json_log::format(const log_event &event,
                             std::string_view timestamp) const {
  rapidjson::Document doc(rapidjson::kObjectType);
  auto &alloc = doc.GetAllocator();
  json::set(doc, "ts", timestamp, alloc);
  json::set(doc, "level", to_string(event.level), alloc);
  json::set(doc, "role", role_, alloc);
  json::set(doc, "component", event.component, alloc);
  json::set(doc, "event", event.event, alloc);
  json::set(doc, "msg", redact_profile(event.message, profile_), alloc);
  json::set(doc, "session", session_, alloc);
  if (!event.plan_id.empty()) {
    json::set(doc, "plan_id", event.plan_id, alloc);
  }
  if (!event.data_json.empty()) {
    const auto parsed = json::parse(redact_profile(event.data_json, profile_));
    if (parsed.ok && parsed.document.IsObject()) {
      json::set(doc, "data", json::copy(parsed.document, alloc), alloc);
    }
  }
  return json::canonical(doc);
}

bool json_log::open(const std::filesystem::path &logs_dir, std::size_t keep) {
  std::error_code ec;
  std::filesystem::create_directories(logs_dir, ec);
  const auto now = std::chrono::floor<std::chrono::seconds>(
      std::chrono::system_clock::now());
  path_ = logs_dir /
          std::format("{:%Y%m%d-%H%M%S}-{}-{}.jsonl", now, role_, session_);
  file_.open(path_, std::ios::app | std::ios::binary);

  std::vector<std::filesystem::path> sessions;
  for (const auto &entry : std::filesystem::directory_iterator(logs_dir, ec)) {
    if (entry.is_regular_file(ec) && entry.path().extension() == ".jsonl") {
      sessions.push_back(entry.path());
    }
  }
  std::ranges::sort(sessions);
  while (sessions.size() > keep) {
    if (sessions.front() != path_) {
      std::filesystem::remove(sessions.front(), ec);
    }
    sessions.erase(sessions.begin());
  }
  return file_.is_open();
}

void json_log::write(const log_event &event) {
  const std::string line = format(event, now_utc());
  std::lock_guard lock(mutex_);
  if (file_.is_open()) {
    file_ << line << '\n';
    file_.flush();
  }
}
} // namespace xerces_core
