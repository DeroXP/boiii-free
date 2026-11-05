// The launcher window on Direct3D 11 (window.hpp). Based on RmlUi 6.3's
// Backends/RmlUi_Backend_Win32_DX11.cpp (MIT licence, (c) The RmlUi Team and
// contributors), as measured in the D4 spike (src/xerces_launcher_spike), with
// a frameless window, the overlapped GPU start always on, an idle wait and
// frame capture.

#include "window.hpp"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/Log.h>

#include <RmlUi_Include_Windows.h>
#include <RmlUi_Platform_Win32.h>
#include <RmlUi_Renderer_DX11.h>

#include <dwmapi.h>

#include <atomic>
#include <mutex>
#include <optional>
#include <thread>

#include "deferred_render.hpp"
#include "png.hpp"

namespace xerces_launcher {
namespace {
#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((HANDLE)-4)
#endif
#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

using set_dpi_context_fn = BOOL(WINAPI *)(HANDLE);
using get_dpi_for_window_fn = UINT(WINAPI *)(HWND);
using metrics_for_dpi_fn = int(WINAPI *)(int, UINT);

set_dpi_context_fn set_dpi_context = nullptr;
get_dpi_for_window_fn get_dpi_for_window = nullptr;
metrics_for_dpi_fn metrics_for_dpi = nullptr;

void load_dpi_functions() {
  HMODULE user32 = GetModuleHandleW(L"user32.dll");
  set_dpi_context = reinterpret_cast<set_dpi_context_fn>(reinterpret_cast<void *>(
      GetProcAddress(user32, "SetProcessDpiAwarenessContext")));
  get_dpi_for_window =
      reinterpret_cast<get_dpi_for_window_fn>(reinterpret_cast<void *>(
          GetProcAddress(user32, "GetDpiForWindow")));
  metrics_for_dpi = reinterpret_cast<metrics_for_dpi_fn>(
      reinterpret_cast<void *>(GetProcAddress(user32, "GetSystemMetricsForDpi")));
  if (set_dpi_context) {
    set_dpi_context(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  }
}

UINT window_dpi(HWND window) {
  const UINT dpi = get_dpi_for_window ? get_dpi_for_window(window) : 0;
  return dpi != 0 ? dpi : USER_DEFAULT_SCREEN_DPI;
}

int metric(int index, UINT dpi) {
  return metrics_for_dpi ? metrics_for_dpi(index, dpi) : GetSystemMetrics(index);
}

struct device_resources {
  ID3D11Device *device = nullptr;
  ID3D11DeviceContext *device_context = nullptr;
  IDXGISwapChain *swap_chain = nullptr;
  ID3D11RenderTargetView *target = nullptr;
};

struct state {
  SystemInterface_Win32 system_interface;
  std::optional<RenderInterface_DX11> render_interface;
  TextInputMethodEditor_Win32 ime;
  window_callbacks callbacks;

  HINSTANCE instance = nullptr;
  std::wstring class_name;
  HWND handle = nullptr;
  device_resources gpu;

  bool context_dimensions_dirty = true;
  bool buffer_dimensions_dirty = true;
  Rml::Vector2i dimensions{1, 1};
  bool running = true;
  Rml::Context *context = nullptr;

  bool gpu_joined = true;
  std::thread gpu_thread;
  std::optional<deferred_render_interface> deferred;

  std::mutex capture_mutex;
  std::filesystem::path capture_path;
  std::atomic<bool> capture_requested = false;
  std::string capture_error;
};
std::unique_ptr<state> s;

void create_render_target() {
  ID3D11Texture2D *back_buffer = nullptr;
  if (SUCCEEDED(s->gpu.swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer)))) {
    s->gpu.device->CreateRenderTargetView(back_buffer, nullptr, &s->gpu.target);
    back_buffer->Release();
  }
}

void release_render_target() {
  if (s->gpu.target) {
    s->gpu.target->Release();
    s->gpu.target = nullptr;
  }
}

bool create_device() {
  const D3D_FEATURE_LEVEL levels[2] = {D3D_FEATURE_LEVEL_11_0,
                                       D3D_FEATURE_LEVEL_10_0};
  D3D_FEATURE_LEVEL level{};
  HRESULT result = D3D11CreateDevice(
      nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
      D3D11_SDK_VERSION, &s->gpu.device, &level, &s->gpu.device_context);
  if (result == DXGI_ERROR_UNSUPPORTED) {
    result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
                               levels, 2, D3D11_SDK_VERSION, &s->gpu.device,
                               &level, &s->gpu.device_context);
  }
  return SUCCEEDED(result);
}

bool create_swap_chain() {
  IDXGIDevice *dxgi_device = nullptr;
  IDXGIAdapter *adapter = nullptr;
  IDXGIFactory *factory = nullptr;
  bool ok = SUCCEEDED(s->gpu.device->QueryInterface(IID_PPV_ARGS(&dxgi_device))) &&
            SUCCEEDED(dxgi_device->GetAdapter(&adapter)) &&
            SUCCEEDED(adapter->GetParent(IID_PPV_ARGS(&factory)));
  if (ok) {
    DXGI_SWAP_CHAIN_DESC desc = {};
    desc.BufferCount = 2;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow = s->handle;
    desc.SampleDesc.Count = 1;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ok = SUCCEEDED(factory->CreateSwapChain(s->gpu.device, &desc,
                                            &s->gpu.swap_chain));
    // The launcher handles Alt+Enter itself (it never goes fullscreen).
    factory->MakeWindowAssociation(s->handle, DXGI_MWA_NO_ALT_ENTER);
  }
  if (factory)
    factory->Release();
  if (adapter)
    adapter->Release();
  if (dxgi_device)
    dxgi_device->Release();
  if (ok) {
    create_render_target();
  }
  return ok;
}

// Joins the GPU worker and finishes the setup on the window's thread.
void ensure_gpu() {
  if (!s || s->gpu_joined) {
    return;
  }
  s->gpu_thread.join();
  s->gpu_joined = true;
  mark("gpu worker joined");
  if (s->gpu.device && s->render_interface && create_swap_chain()) {
    RECT client{};
    GetClientRect(s->handle, &client);
    s->dimensions = Rml::Math::Max(Rml::Vector2i{1, 1},
                                   {int(client.right), int(client.bottom)});
    s->render_interface->SetViewport(s->dimensions.x, s->dimensions.y);
    s->buffer_dimensions_dirty = true;
    mark("swap chain");
  } else {
    Rml::Log::Message(Rml::Log::LT_ERROR,
                      "The Direct3D 11 device or swap chain failed.");
  }
}

void release_gpu() {
  release_render_target();
  if (s->gpu.swap_chain) {
    s->gpu.swap_chain->Release();
    s->gpu.swap_chain = nullptr;
  }
  if (s->gpu.device_context) {
    s->gpu.device_context->Release();
    s->gpu.device_context = nullptr;
  }
  if (s->gpu.device) {
    s->gpu.device->Release();
    s->gpu.device = nullptr;
  }
}

// Copies the back buffer (after the frame was drawn, before Present) to a
// PNG.
void capture_back_buffer() {
  std::filesystem::path path;
  {
    std::lock_guard lock(s->capture_mutex);
    path = s->capture_path;
  }
  s->capture_error.clear();
  ID3D11Texture2D *back_buffer = nullptr;
  if (FAILED(s->gpu.swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer)))) {
    s->capture_error = "no back buffer";
    return;
  }
  D3D11_TEXTURE2D_DESC desc{};
  back_buffer->GetDesc(&desc);
  desc.Usage = D3D11_USAGE_STAGING;
  desc.BindFlags = 0;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  desc.MiscFlags = 0;
  ID3D11Texture2D *staging = nullptr;
  if (FAILED(s->gpu.device->CreateTexture2D(&desc, nullptr, &staging))) {
    back_buffer->Release();
    s->capture_error = "no staging texture";
    return;
  }
  s->gpu.device_context->CopyResource(staging, back_buffer);
  D3D11_MAPPED_SUBRESOURCE mapped{};
  if (SUCCEEDED(s->gpu.device_context->Map(staging, 0, D3D11_MAP_READ, 0,
                                           &mapped))) {
    std::vector<unsigned char> rgb(std::size_t(desc.Width) * desc.Height * 3);
    for (UINT y = 0; y < desc.Height; ++y) {
      const auto *row = static_cast<const unsigned char *>(mapped.pData) +
                        std::size_t(y) * mapped.RowPitch;
      for (UINT x = 0; x < desc.Width; ++x) {
        unsigned char *out = &rgb[(std::size_t(y) * desc.Width + x) * 3];
        out[0] = row[x * 4 + 0];
        out[1] = row[x * 4 + 1];
        out[2] = row[x * 4 + 2];
      }
    }
    s->gpu.device_context->Unmap(staging, 0);
    if (!write_png_rgb(path, rgb.data(), int(desc.Width), int(desc.Height))) {
      s->capture_error = "could not write " + path.string();
    }
  } else {
    s->capture_error = "could not map the staging texture";
  }
  staging->Release();
  back_buffer->Release();
}

LRESULT CALLBACK window_procedure(HWND handle, UINT message, WPARAM w_param,
                                  LPARAM l_param) {
  if (!s) {
    return DefWindowProcW(handle, message, w_param, l_param);
  }
  switch (message) {
  case WM_NCCALCSIZE:
    // No system title bar or borders: the whole window is client area. A
    // maximized window would hang over the screen edge by its frame size.
    if (w_param == TRUE) {
      auto *params = reinterpret_cast<NCCALCSIZE_PARAMS *>(l_param);
      if (IsZoomed(handle)) {
        const UINT dpi = window_dpi(handle);
        const int frame = metric(SM_CXFRAME, dpi) + metric(SM_CXPADDEDBORDER, dpi);
        params->rgrc[0].left += frame;
        params->rgrc[0].right -= frame;
        params->rgrc[0].top += frame;
        params->rgrc[0].bottom -= frame;
      }
      return 0;
    }
    break;
  case WM_NCHITTEST: {
    POINT point{static_cast<short>(LOWORD(l_param)),
                static_cast<short>(HIWORD(l_param))};
    ScreenToClient(handle, &point);
    RECT client{};
    GetClientRect(handle, &client);
    if (!IsZoomed(handle)) {
      const int edge = MulDiv(6, int(window_dpi(handle)), 96);
      const bool left = point.x < edge;
      const bool right = point.x >= client.right - edge;
      const bool top = point.y < edge;
      const bool bottom = point.y >= client.bottom - edge;
      if (top && left)
        return HTTOPLEFT;
      if (top && right)
        return HTTOPRIGHT;
      if (bottom && left)
        return HTBOTTOMLEFT;
      if (bottom && right)
        return HTBOTTOMRIGHT;
      if (left)
        return HTLEFT;
      if (right)
        return HTRIGHT;
      if (top)
        return HTTOP;
      if (bottom)
        return HTBOTTOM;
    }
    if (s->callbacks.hit_test &&
        s->callbacks.hit_test(point.x, point.y) == hit_region::caption) {
      return HTCAPTION;
    }
    return HTCLIENT;
  }
  case WM_NCACTIVATE:
    // Keeps DWM from drawing the inactive frame over the client area.
    return DefWindowProcW(handle, message, w_param, -1);
  case WM_CLOSE:
    s->running = false;
    return 0;
  case WM_GETMINMAXINFO: {
    auto *info = reinterpret_cast<MINMAXINFO *>(l_param);
    const UINT dpi = window_dpi(handle);
    info->ptMinTrackSize.x = MulDiv(960, int(dpi), 96);
    info->ptMinTrackSize.y = MulDiv(600, int(dpi), 96);
    return 0;
  }
  case WM_SIZE: {
    const int width = LOWORD(l_param);
    const int height = HIWORD(l_param);
    s->dimensions = Rml::Math::Max(Rml::Vector2i{1, 1}, {width, height});
    s->context_dimensions_dirty = true;
    s->buffer_dimensions_dirty = true;
    if (s->gpu_joined && s->render_interface) {
      s->render_interface->SetViewport(s->dimensions.x, s->dimensions.y);
    }
    if (s->context) {
      s->context->SetDimensions(s->dimensions);
    }
    return 0;
  }
  case WM_DPICHANGED: {
    const RECT *suggested = reinterpret_cast<RECT *>(l_param);
    SetWindowPos(handle, nullptr, suggested->left, suggested->top,
                 suggested->right - suggested->left,
                 suggested->bottom - suggested->top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    s->context_dimensions_dirty = true;
    return 0;
  }
  case WM_KEYDOWN:
  case WM_SYSKEYDOWN: {
    Rml::Context *context = s->context;
    const Rml::Input::KeyIdentifier key =
        RmlWin32::ConvertKey(static_cast<int>(w_param));
    const int modifiers = RmlWin32::GetKeyModifierState();
    if (s->callbacks.key_down && !s->callbacks.key_down(key, modifiers, true)) {
      return 0;
    }
    if (!RmlWin32::WindowProcedure(context, s->ime, handle, message, w_param,
                                   l_param)) {
      return 0;
    }
    if (s->callbacks.key_down) {
      s->callbacks.key_down(key, modifiers, false);
    }
    if (message == WM_SYSKEYDOWN) {
      break; // Alt+F4 and Alt+Space keep working
    }
    return 0;
  }
  default:
    if (!RmlWin32::WindowProcedure(s->context, s->ime, handle, message, w_param,
                                   l_param)) {
      return 0;
    }
    break;
  }
  return DefWindowProcW(handle, message, w_param, l_param);
}
} // namespace

window::~window() { close(); }

bool window::open(const std::string &title, int width_dp, int height_dp,
                  window_callbacks callbacks) {
  if (s) {
    return false;
  }
  s = std::make_unique<state>();
  s->callbacks = std::move(callbacks);
  s->instance = GetModuleHandleW(nullptr);
  s->class_name = L"xerces_launcher_window";
  load_dpi_functions();

  WNDCLASSW window_class{};
  window_class.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
  window_class.lpfnWndProc = &window_procedure;
  window_class.hInstance = s->instance;
  window_class.hIcon = LoadIconW(s->instance, MAKEINTRESOURCEW(1));
  if (!window_class.hIcon) {
    window_class.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
  }
  window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  // Black, the C / Signal background: the window paints at once while the
  // GPU starts (the native paint of DESIGN.md 7.1).
  window_class.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
  window_class.lpszClassName = s->class_name.c_str();
  if (!RegisterClassW(&window_class)) {
    s.reset();
    return false;
  }

  const std::wstring name = RmlWin32::ConvertToUTF16(title);
  const DWORD style = WS_OVERLAPPEDWINDOW | WS_CLIPSIBLINGS | WS_CLIPCHILDREN;
  s->handle = CreateWindowExW(WS_EX_APPWINDOW, s->class_name.c_str(),
                              name.c_str(), style, 0, 0, 0, 0, nullptr, nullptr,
                              s->instance, nullptr);
  if (!s->handle) {
    UnregisterClassW(s->class_name.c_str(), s->instance);
    s.reset();
    return false;
  }
  // Keeps the DWM shadow and snap layouts on a window without a frame.
  const MARGINS margins{0, 0, 1, 0};
  DwmExtendFrameIntoClientArea(s->handle, &margins);

  // Size in dp of the monitor the window starts on, centred on its work
  // area, never larger than it.
  const UINT dpi = window_dpi(s->handle);
  MONITORINFO monitor{};
  monitor.cbSize = sizeof(monitor);
  GetMonitorInfoW(MonitorFromWindow(s->handle, MONITOR_DEFAULTTOPRIMARY),
                  &monitor);
  const RECT work = monitor.rcWork;
  const int width = std::min(MulDiv(width_dp, int(dpi), 96),
                             int(work.right - work.left));
  const int height = std::min(MulDiv(height_dp, int(dpi), 96),
                              int(work.bottom - work.top));
  SetWindowPos(s->handle, nullptr,
               work.left + (work.right - work.left - width) / 2,
               work.top + (work.bottom - work.top - height) / 2, width, height,
               SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
  mark("window created");

  s->system_interface.SetWindow(s->handle);
  ShowWindow(s->handle, SW_SHOW);
  SetForegroundWindow(s->handle);
  SetFocus(s->handle);
  UpdateWindow(s->handle);
  mark("window shown (native paint)");

  // Overlapped start: the device and the shaders on a worker while RmlUi,
  // the fonts and the documents load on this thread.
  s->gpu_joined = false;
  s->gpu_thread = std::thread([] {
    if (!create_device()) {
      return;
    }
    mark("d3d11 device (worker)");
    s->render_interface.emplace(s->gpu.device);
    mark("renderer shaders (worker)");
  });
  s->deferred.emplace([]() -> Rml::RenderInterface * {
    ensure_gpu();
    return s->render_interface ? &*s->render_interface : nullptr;
  });
  Rml::SetTextInputHandler(&s->ime);
  return true;
}

void window::close() {
  if (!s) {
    return;
  }
  if (Rml::GetTextInputHandler() == &s->ime) {
    Rml::SetTextInputHandler(nullptr);
  }
  if (s->gpu_thread.joinable()) {
    s->gpu_thread.join();
  }
  s->deferred.reset();
  s->render_interface.reset();
  release_gpu();
  DestroyWindow(s->handle);
  UnregisterClassW(s->class_name.c_str(), s->instance);
  s.reset();
}

Rml::SystemInterface *window::system_interface() {
  return s ? &s->system_interface : nullptr;
}

Rml::RenderInterface *window::render_interface() {
  return s ? &*s->deferred : nullptr;
}

bool window::process_events(Rml::Context *context, int max_wait_ms) {
  if (s->context_dimensions_dirty) {
    s->context_dimensions_dirty = false;
    context->SetDimensions(s->dimensions);
    context->SetDensityIndependentPixelRatio(dp_ratio());
  }
  s->context = context;
  MSG message{};
  bool has_message = PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != 0;
  if (!has_message && max_wait_ms > 0) {
    MsgWaitForMultipleObjects(0, nullptr, FALSE, DWORD(max_wait_ms),
                              QS_ALLINPUT);
    has_message = PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != 0;
  }
  while (has_message) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
    has_message = PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != 0;
  }
  s->context = nullptr;
  const bool running = s->running;
  s->running = true;
  return running;
}

void window::begin_frame() {
  ensure_gpu();
  if (!s->render_interface || !s->gpu.swap_chain) {
    return;
  }
  if (s->buffer_dimensions_dirty) {
    s->buffer_dimensions_dirty = false;
    release_render_target();
    s->gpu.swap_chain->ResizeBuffers(0, s->dimensions.x, s->dimensions.y,
                                     DXGI_FORMAT_UNKNOWN, 0);
    create_render_target();
  }
  s->render_interface->Clear(s->gpu.target);
  s->render_interface->BeginFrame();
}

void window::present() {
  if (!s->render_interface || !s->gpu.swap_chain) {
    return;
  }
  s->render_interface->EndFrame(s->gpu.target);
  if (s->capture_requested.exchange(false)) {
    capture_back_buffer();
  }
  s->gpu.swap_chain->Present(1, 0);
}

void window::minimize() { ShowWindow(s->handle, SW_MINIMIZE); }

void window::toggle_maximize() {
  ShowWindow(s->handle, IsZoomed(s->handle) ? SW_RESTORE : SW_MAXIMIZE);
}

bool window::maximized() const { return s && IsZoomed(s->handle); }

void window::request_close() {
  if (s) {
    s->running = false;
  }
}

void window::wake() {
  if (s) {
    PostMessageW(s->handle, WM_NULL, 0, 0);
  }
}

float window::dp_ratio() const {
  return s ? float(window_dpi(s->handle)) / float(USER_DEFAULT_SCREEN_DPI)
           : 1.f;
}

void *window::native_handle() const { return s ? s->handle : nullptr; }

int window::width() const { return s ? s->dimensions.x : 0; }
int window::height() const { return s ? s->dimensions.y : 0; }

void window::resize_client(int width_px, int height_px) {
  if (!s) {
    return;
  }
  RECT rect{};
  GetWindowRect(s->handle, &rect);
  SetWindowPos(s->handle, nullptr, rect.left, rect.top, width_px, height_px,
               SWP_NOZORDER | SWP_NOACTIVATE);
}

void window::capture_next_frame(std::filesystem::path png) {
  if (!s) {
    return;
  }
  {
    std::lock_guard lock(s->capture_mutex);
    s->capture_path = std::move(png);
  }
  s->capture_requested = true;
}

bool window::capture_pending() const { return s && s->capture_requested; }

std::string window::last_capture_error() const {
  return s ? s->capture_error : std::string();
}
} // namespace xerces_launcher
