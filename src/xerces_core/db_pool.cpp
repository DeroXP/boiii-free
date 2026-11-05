#include "db_pool.hpp"

#include <cstdio>

#include "json.hpp"
#include "launch.hpp"

namespace xerces_core::db_pool {
namespace {
std::string hex(std::uint64_t value) {
  char text[24];
  std::snprintf(text, sizeof(text), "0x%llX",
                static_cast<unsigned long long>(value));
  return text;
}

std::string mib(std::uint64_t bytes) {
  return std::to_string(bytes / (1024 * 1024)) + " MiB";
}

void set_hex(rapidjson::Value &object, std::string_view key,
             std::uint64_t value, json::allocator &alloc) {
  json::set(object, key, hex(value), alloc);
}

rapidjson::Value call_value(const pool_call &call, json::allocator &alloc) {
  rapidjson::Value item(rapidjson::kObjectType);
  json::set(item, "name", json::valid_utf8(call.name.view(), 16), alloc);
  set_hex(item, "flags", call.flags, alloc);
  set_hex(item, "size", call.size_in, alloc);
  if (call.size_out != call.size_in) {
    set_hex(item, "size_given", call.size_out, alloc);
  }
  json::set(item, "reserved", call.result != 0, alloc);
  return item;
}
} // namespace

bool requested(const std::vector<std::string> &args) {
  return has_launch_flag(args, flag) ||
         has_launch_flag(args, "-" + std::string(flag));
}

std::uint64_t pool_size(bool first_call, std::string_view name,
                        std::uint32_t flags, std::uint64_t size) {
  if (first_call && name == main_pool_name && flags == main_pool_flags &&
      size == stock_pool_size) {
    return raised_pool_size;
  }
  return size;
}

std::uint64_t page_budget(std::uint64_t bytes) {
  return bytes == stock_page_budget ? raised_page_budget : bytes;
}

pool_name copy_pool_name(const char *name) {
  pool_name out;
  if (name == nullptr) {
    return out;
  }
  for (std::size_t i = 0; i + 1 < sizeof(out.text) && name[i] != '\0'; ++i) {
    out.text[i] = name[i];
  }
  return out;
}

bool pool_raised(const report &r) {
  return !r.calls.empty() && r.calls.front().size_out == raised_pool_size &&
         r.calls.front().size_in == stock_pool_size;
}

bool budget_raised(const report &r) {
  return r.budget && r.budget->bytes_out == raised_page_budget &&
         r.budget->bytes_in == stock_page_budget;
}

bool applied(const report &r) {
  return pool_raised(r) && budget_raised(r) && r.calls.front().result != 0 &&
         (!r.main_size || *r.main_size == raised_pool_size);
}

std::string message(const report &r) {
  if (applied(r)) {
    std::string out = "Larger zone memory is on: the DB main pool is " +
                      mib(raised_pool_size) + " (stock " +
                      mib(stock_pool_size) + ") and the page budget " +
                      mib(raised_page_budget) + " (stock " +
                      mib(stock_page_budget) + ")";
    if (r.main_size) {
      out += "; the pool reads back " + mib(*r.main_size);
    }
    return out;
  }
  if (r.pool_calls == 0 && r.budget_calls == 0) {
    return "Larger zone memory did nothing: the engine never called the "
           "hooked pool helpers, so the pools are the stock ones";
  }
  std::string out = "Larger zone memory did not fully apply:";
  if (!pool_raised(r)) {
    if (r.calls.empty()) {
      out += " the pool helper was never called;";
    } else {
      const pool_call &first = r.calls.front();
      out += " the first pool was \"" +
             json::valid_utf8(first.name.view(), 16) + "\" flags " +
             hex(first.flags) + " size " + hex(first.size_in) +
             ", not the stock DB main pool, so no pool was raised;";
    }
  } else if (r.calls.front().result == 0) {
    out += " the engine could not reserve the raised pool;";
  } else if (r.main_size && *r.main_size != raised_pool_size) {
    out += " the DB main pool reads back " + hex(*r.main_size) + ";";
  }
  if (!budget_raised(r)) {
    out += r.budget ? " the page budget was " + hex(r.budget->bytes_in) +
                          ", not the stock 5 GiB, so it was left alone;"
                    : " the page budget helper was never called;";
  }
  out.pop_back();
  return out;
}

std::string pool_call_json(const pool_call &call) {
  rapidjson::Document doc;
  return json::canonical(call_value(call, doc.GetAllocator()));
}

std::string data_json(const report &r) {
  rapidjson::Document doc(rapidjson::kObjectType);
  auto &alloc = doc.GetAllocator();
  json::set(doc, "applied", applied(r), alloc);
  json::set(doc, "pool_raised", pool_raised(r), alloc);
  json::set(doc, "budget_raised", budget_raised(r), alloc);
  json::set(doc, "pool_calls", static_cast<std::int64_t>(r.pool_calls), alloc);
  json::set(doc, "budget_calls", static_cast<std::int64_t>(r.budget_calls),
            alloc);
  if (r.budget) {
    rapidjson::Value budget(rapidjson::kObjectType);
    set_hex(budget, "bytes", r.budget->bytes_in, alloc);
    if (r.budget->bytes_out != r.budget->bytes_in) {
      set_hex(budget, "bytes_given", r.budget->bytes_out, alloc);
    }
    set_hex(budget, "rdx", r.budget->rdx, alloc);
    json::set(doc, "budget", std::move(budget), alloc);
  }
  if (r.main_size) {
    set_hex(doc, "main_size_now", *r.main_size, alloc);
  }
  if (r.main_pages) {
    json::set(doc, "main_pages_now", static_cast<std::int64_t>(*r.main_pages),
              alloc);
  }
  rapidjson::Value calls(rapidjson::kArrayType);
  for (const pool_call &call : r.calls) {
    calls.PushBack(call_value(call, alloc), alloc);
  }
  json::set(doc, "calls", std::move(calls), alloc);
  if (r.calls.size() < r.pool_calls) {
    json::set(doc, "calls_not_recorded",
              static_cast<std::int64_t>(r.pool_calls - r.calls.size()), alloc);
  }
  return json::canonical(doc);
}

bool above_stock(std::uint32_t pages) {
  return std::uint64_t{pages} * page_size > stock_pool_size;
}

std::string usage_message(std::uint32_t pages, std::uint32_t peak_pages,
                          std::string_view when) {
  const std::string now = mib(std::uint64_t{pages} * page_size);
  const std::string peak = mib(std::uint64_t{peak_pages} * page_size);
  std::string out = when == "in_game"
                        ? "In a level, the DB main pool holds " + now
                        : "The DB main pool now holds " + now;
  out += above_stock(pages) ? ", more than the stock " + mib(stock_pool_size)
                            : ", within the stock " + mib(stock_pool_size);
  if (peak_pages > pages) {
    out += " (at most " + peak + " so far)";
  }
  return out;
}

std::string usage_json(std::uint32_t pages, std::uint32_t peak_pages,
                       std::string_view when) {
  rapidjson::Document doc(rapidjson::kObjectType);
  auto &alloc = doc.GetAllocator();
  json::set(doc, "when", when, alloc);
  json::set(doc, "pages", static_cast<std::int64_t>(pages), alloc);
  json::set(doc, "mib",
            static_cast<std::int64_t>(std::uint64_t{pages} * page_size >> 20),
            alloc);
  json::set(
      doc, "peak_mib",
      static_cast<std::int64_t>(std::uint64_t{peak_pages} * page_size >> 20),
      alloc);
  json::set(doc, "above_stock", above_stock(pages), alloc);
  return json::canonical(doc);
}
} // namespace xerces_core::db_pool
