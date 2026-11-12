#include <std_include.hpp>

#include <loader/component_loader.hpp>

#include "scheduler.hpp"
#include "upnp.hpp"

#include <natupnp.h>

#include <future>

namespace upnp {
namespace {
std::mutex state_mutex;
std::string mapped_endpoint;
uint16_t mapped_port{};
std::atomic_bool operation_running{};
// Xerces: whether the port should be open. A close while the router is
// still being asked removes the mapping as soon as the answer comes.
std::atomic_bool wanted{};

std::string local_ipv4() {
  const auto handle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (handle == INVALID_SOCKET)
    return {};

  sockaddr_in route{};
  route.sin_family = AF_INET;
  route.sin_port = htons(53);
  inet_pton(AF_INET, "1.1.1.1", &route.sin_addr);

  std::string result;
  if (connect(handle, reinterpret_cast<sockaddr *>(&route), sizeof(route)) ==
      0) {
    sockaddr_in local{};
    int size = sizeof(local);
    if (getsockname(handle, reinterpret_cast<sockaddr *>(&local), &size) == 0) {
      char text[INET_ADDRSTRLEN]{};
      if (inet_ntop(AF_INET, &local.sin_addr, text, sizeof(text)))
        result = text;
    }
  }
  closesocket(handle);
  return result;
}

std::wstring widen(const std::string &text) {
  return {text.begin(), text.end()};
}

bool create_nat(CComPtr<IUPnPNAT> &nat,
                CComPtr<IStaticPortMappingCollection> &mappings) {
  if (FAILED(CoCreateInstance(CLSID_UPnPNAT, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&nat))))
    return false;
  return SUCCEEDED(nat->get_StaticPortMappingCollection(&mappings)) && mappings;
}

void unmap_port(const uint16_t port) {
  const auto initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  const bool uninitialize = SUCCEEDED(initialized);
  {
    CComPtr<IUPnPNAT> nat;
    CComPtr<IStaticPortMappingCollection> mappings;
    if (create_nat(nat, mappings)) {
      const CComBSTR protocol(L"UDP");
      mappings->Remove(port, protocol);
    }
  }
  if (uninitialize)
    CoUninitialize();
}

void map_port(const uint16_t port, const callback &done) {
  const auto initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  const bool uninitialize = SUCCEEDED(initialized);
  outcome out{result::no_router, {}, port};
  {
    const auto local = local_ipv4();
    CComPtr<IUPnPNAT> nat;
    CComPtr<IStaticPortMappingCollection> mappings;
    if (!local.empty() && create_nat(nat, mappings)) {
      const CComBSTR protocol(L"UDP");
      const CComBSTR client(widen(local).c_str());
      constexpr const wchar_t *ours = L"Xerces lobby (Black Ops III)";
      const CComBSTR description(ours);
      CComPtr<IStaticPortMapping> mapping;
      auto added = mappings->Add(port, protocol, port, client, VARIANT_TRUE,
                                 description, &mapping);
      if (FAILED(added) || !mapping) {
        // A game that crashed with its lobby open left its mapping behind
        // (static, no lease). When the router still holds one of ours on
        // this port (say, for this PC's previous address), it is replaced;
        // a mapping anything else made is left alone.
        CComPtr<IStaticPortMapping> existing;
        CComBSTR existing_description;
        if (SUCCEEDED(mappings->get_Item(port, protocol, &existing)) &&
            existing &&
            SUCCEEDED(existing->get_Description(&existing_description)) &&
            existing_description && wcscmp(existing_description, ours) == 0) {
          existing.Release();
          mapping.Release();
          if (SUCCEEDED(mappings->Remove(port, protocol)))
            added = mappings->Add(port, protocol, port, client, VARIANT_TRUE,
                                  description, &mapping);
        }
      }
      if (SUCCEEDED(added) && mapping) {
        out.value = result::mapped;
        BSTR external_raw{};
        if (SUCCEEDED(mapping->get_ExternalIPAddress(&external_raw)) &&
            external_raw) {
          char external[INET_ADDRSTRLEN]{};
          WideCharToMultiByte(CP_ACP, 0, external_raw, -1, external,
                              sizeof(external), nullptr, nullptr);
          SysFreeString(external_raw);
          IN_ADDR parsed{};
          if (inet_pton(AF_INET, external, &parsed) == 1)
            out.external_ip = external;
        }
        // Remembered even without an outside address, so the mapping is
        // always removed again.
        std::lock_guard lock(state_mutex);
        mapped_port = port;
        mapped_endpoint = out.external_ip.empty()
                              ? std::string{}
                              : out.external_ip + ":" + std::to_string(port);
      } else {
        out.value = result::refused;
      }
    }
  }
  operation_running = false;
  if (out.value == result::mapped && !wanted) {
    // Closed while the router was being asked.
    uint16_t stale{};
    {
      std::lock_guard lock(state_mutex);
      stale = std::exchange(mapped_port, uint16_t{});
      mapped_endpoint.clear();
    }
    if (stale)
      unmap_port(stale);
  }
  if (uninitialize)
    CoUninitialize();
  if (done && wanted)
    done(out);
}

// Removals of the mapped port begun by close_port_now and not finished
std::atomic_int removals_running{};

uint16_t take_mapped_port() {
  std::lock_guard lock(state_mutex);
  mapped_endpoint.clear();
  return std::exchange(mapped_port, uint16_t{});
}
} // namespace

void open_port(const uint16_t port, callback done) {
  if (port < 1024)
    return;
  wanted = true;
  if (operation_running.exchange(true))
    return;
  scheduler::once([port, done = std::move(done)] { map_port(port, done); },
                  scheduler::async);
}

void close_port() {
  wanted = false;
  if (const uint16_t port = take_mapped_port())
    scheduler::once([port] { unmap_port(port); }, scheduler::async);
}

void close_port_now() {
  wanted = false;
  // A request still out usually finishes within a second; its answer then
  // sees wanted is false and removes what it mapped itself.
  for (int i = 0; i < 10 && operation_running; ++i)
    std::this_thread::sleep_for(100ms);
  uint16_t port = 0;
  {
    std::lock_guard lock(state_mutex);
    mapped_endpoint.clear();
    port = std::exchange(mapped_port, uint16_t{});
    if (port)
      ++removals_running;
  }
  if (port) {
    // A router that stops answering would hold the exit for as long as
    // Windows waits for it, so the removal runs on its own thread.
    try {
      std::thread([port] {
        unmap_port(port);
        --removals_running;
      }).detach();
    } catch (const std::system_error &) {
      --removals_running;
    }
  }
  // Every caller waits for the removal, at most 2 s, whoever began it: the
  // exit's shutdown and a late fault's exit (exception.cpp) can both be here,
  // and the one that found no port must not end the process under the
  // other's request.
  for (int i = 0; i < 40 && removals_running; ++i)
    std::this_thread::sleep_for(50ms);
  if (port && removals_running)
    printf("[UPnP] The router did not answer in time; it may still forward "
           "UDP port %u until the port is mapped again\n",
           static_cast<unsigned>(port));
}

std::string external_endpoint() {
  std::lock_guard lock(state_mutex);
  return mapped_endpoint;
}

bool port_mapped() {
  std::lock_guard lock(state_mutex);
  return mapped_port != 0;
}

class component final : public client_component {
  DEFINE_COMPONENT_NAME("upnp");

public:
  // Xerces: at once, because the scheduler may already have stopped.
  void pre_destroy() override { close_port_now(); }
};
} // namespace upnp

REGISTER_COMPONENT(upnp::component)
