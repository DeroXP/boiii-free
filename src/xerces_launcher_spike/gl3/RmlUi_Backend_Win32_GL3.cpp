// Win32 platform with RmlUi's OpenGL 3 renderer, for the launcher spike
// (DESIGN.md 7.1). RmlUi 6.3 ships GL3 only with GLFW, SDL and Wayland; this
// pairs its Win32 platform with the GL3 renderer. Adapted from RmlUi's
// RmlUi_Backend_Win32_GL2.cpp (RmlUi, MIT licence, (c) 2008-2010 CodePoint Ltd,
// Shift Technology Ltd, and contributors; (c) 2019-2023 The RmlUi Team, and
// contributors).
//
// With XERCES_SPIKE_ASYNC=1 the window is shown at once with a native black
// paint while a worker thread warms up the OpenGL driver; the real context,
// the GL functions and the shaders are created on the main thread before the
// first frame (see WarmUpDriver).

#include "RmlUi_Backend.h"
#include "RmlUi_Include_Windows.h"
#include "RmlUi_Platform_Win32.h"
#include "RmlUi_Renderer_GL3.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/Input.h>
#include <RmlUi/Core/Log.h>
#include <RmlUi/Core/Profiling.h>

#include <xerces_launcher_spike/deferred_render.hpp>

#include <cstdlib>
#include <optional>
#include <thread>

void xerces_spike_mark(const char *step);

#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((HANDLE) - 4)
#endif
#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

namespace {
using ProcSetProcessDpiAwarenessContext = BOOL(WINAPI *)(HANDLE value);
using ProcGetDpiForWindow = UINT(WINAPI *)(HWND hwnd);
using ProcAdjustWindowRectExForDpi = BOOL(WINAPI *)(LPRECT, DWORD, BOOL, DWORD,
                                                    UINT);
using ProcCreateContextAttribs = HGLRC(WINAPI *)(HDC, HGLRC, const int *);
using ProcSwapInterval = BOOL(WINAPI *)(int);

constexpr int WGL_CONTEXT_MAJOR_VERSION_ARB = 0x2091;
constexpr int WGL_CONTEXT_MINOR_VERSION_ARB = 0x2092;
constexpr int WGL_CONTEXT_PROFILE_MASK_ARB = 0x9126;
constexpr int WGL_CONTEXT_CORE_PROFILE_BIT_ARB = 0x00000001;

bool has_dpi_support = false;
ProcSetProcessDpiAwarenessContext set_dpi_awareness = nullptr;
ProcGetDpiForWindow get_dpi_for_window = nullptr;
ProcAdjustWindowRectExForDpi adjust_window_rect_for_dpi = nullptr;

bool env_flag(const char *name) {
  char value[8] = {};
  return GetEnvironmentVariableA(name, value, sizeof(value)) > 0 &&
         value[0] == '1';
}

void InitializeDpiSupport() {
  const HMODULE user32 = GetModuleHandleW(L"User32.dll");
  set_dpi_awareness = (ProcSetProcessDpiAwarenessContext)(void *)GetProcAddress(
      user32, "SetProcessDpiAwarenessContext");
  get_dpi_for_window =
      (ProcGetDpiForWindow)(void *)GetProcAddress(user32, "GetDpiForWindow");
  adjust_window_rect_for_dpi =
      (ProcAdjustWindowRectExForDpi)(void *)GetProcAddress(
          user32, "AdjustWindowRectExForDpi");
  if (!has_dpi_support && set_dpi_awareness && get_dpi_for_window &&
      adjust_window_rect_for_dpi &&
      set_dpi_awareness(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
    has_dpi_support = true;
  }
}

UINT GetWindowDpi(HWND window) {
  if (has_dpi_support) {
    if (const UINT dpi = get_dpi_for_window(window)) {
      return dpi;
    }
  }
  return USER_DEFAULT_SCREEN_DPI;
}

float GetDensityIndependentPixelRatio(HWND window) {
  return float(GetWindowDpi(window)) / float(USER_DEFAULT_SCREEN_DPI);
}

struct BackendData {
  SystemInterface_Win32 system_interface;
  std::optional<RenderInterface_GL3> render_interface;
  TextInputMethodEditor_Win32 text_input_method_editor;

  HINSTANCE instance_handle = nullptr;
  std::wstring instance_name;
  HWND window_handle = nullptr;
  HDC device_context = nullptr;
  HGLRC render_context = nullptr;

  bool context_dimensions_dirty = true;
  Rml::Vector2i window_dimensions;
  bool running = true;

  Rml::Context *context = nullptr;
  KeyDownCallback key_down_callback = nullptr;

  // Overlapped start (XERCES_SPIKE_ASYNC=1).
  bool async = false;
  bool gpu_joined = true;
  bool gpu_ok = false;
  std::thread gpu_thread;
  std::optional<deferred_render_interface> deferred;
};
Rml::UniquePtr<BackendData> data;

LRESULT CALLBACK WindowProcedureHandler(HWND window, UINT message,
                                        WPARAM w_param, LPARAM l_param) {
  if (!data) {
    return DefWindowProcW(window, message, w_param, l_param);
  }
  switch (message) {
  case WM_CLOSE:
    data->running = false;
    return 0;
  case WM_SIZE: {
    data->window_dimensions = {LOWORD(l_param), HIWORD(l_param)};
    // During an async start the context is not current on this thread yet.
    if (data->gpu_joined && data->render_interface) {
      data->render_interface->SetViewport(data->window_dimensions.x,
                                          data->window_dimensions.y);
    }
    if (data->context) {
      data->context->SetDimensions(data->window_dimensions);
    }
    return 0;
  }
  case WM_DPICHANGED: {
    const RECT *new_pos = reinterpret_cast<RECT *>(l_param);
    SetWindowPos(window, nullptr, new_pos->left, new_pos->top,
                 new_pos->right - new_pos->left, new_pos->bottom - new_pos->top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    if (data->context && has_dpi_support) {
      data->context->SetDensityIndependentPixelRatio(
          GetDensityIndependentPixelRatio(window));
    }
    return 0;
  }
  case WM_KEYDOWN: {
    Rml::Context *context = data->context;
    const KeyDownCallback callback = data->key_down_callback;
    const Rml::Input::KeyIdentifier key = RmlWin32::ConvertKey((int)w_param);
    const int modifiers = RmlWin32::GetKeyModifierState();
    const float dp_ratio = GetDensityIndependentPixelRatio(window);
    if (callback && !callback(context, key, modifiers, dp_ratio, true)) {
      return 0;
    }
    if (!RmlWin32::WindowProcedure(context, data->text_input_method_editor,
                                   window, message, w_param, l_param)) {
      return 0;
    }
    if (callback) {
      callback(context, key, modifiers, dp_ratio, false);
    }
    return 0;
  }
  default:
    if (!RmlWin32::WindowProcedure(data->context,
                                   data->text_input_method_editor, window,
                                   message, w_param, l_param)) {
      return 0;
    }
    break;
  }
  return DefWindowProcW(window, message, w_param, l_param);
}

HWND InitializeWindow(HINSTANCE instance, const std::wstring &name, int &width,
                      int &height, bool allow_resize) {
  WNDCLASSW window_class{};
  window_class.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
  window_class.lpfnWndProc = &WindowProcedureHandler;
  window_class.hInstance = instance;
  window_class.hIcon = LoadIcon(nullptr, IDI_WINLOGO);
  window_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
  // Black, the C / Signal background, so the window paints at once.
  window_class.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
  window_class.lpszClassName = name.data();
  if (!RegisterClassW(&window_class)) {
    Rml::Log::Message(Rml::Log::LT_ERROR, "Failed to register window class");
    return nullptr;
  }
  HWND window = CreateWindowExW(
      WS_EX_APPWINDOW | WS_EX_WINDOWEDGE, name.data(), name.data(),
      WS_CLIPSIBLINGS | WS_CLIPCHILDREN | WS_OVERLAPPEDWINDOW, 0, 0, 0, 0,
      nullptr, nullptr, instance, nullptr);
  if (!window) {
    Rml::Log::Message(Rml::Log::LT_ERROR, "Failed to create window");
    return nullptr;
  }
  const UINT dpi = GetWindowDpi(window);
  width = (width * (int)dpi) / USER_DEFAULT_SCREEN_DPI;
  height = (height * (int)dpi) / USER_DEFAULT_SCREEN_DPI;

  const DWORD style =
      allow_resize ? WS_OVERLAPPEDWINDOW
                   : (WS_OVERLAPPEDWINDOW & ~WS_SIZEBOX & ~WS_MAXIMIZEBOX);
  const DWORD extended_style = WS_EX_APPWINDOW | WS_EX_WINDOWEDGE;
  RECT rect{0, 0, width, height};
  if (has_dpi_support) {
    adjust_window_rect_for_dpi(&rect, style, FALSE, extended_style, dpi);
  } else {
    AdjustWindowRectEx(&rect, style, FALSE, extended_style);
  }
  SetWindowLongW(window, GWL_EXSTYLE, extended_style);
  SetWindowLongW(window, GWL_STYLE, style);

  const Rml::Vector2i screen = {GetSystemMetrics(SM_CXSCREEN),
                                GetSystemMetrics(SM_CYSCREEN)};
  const Rml::Vector2i size = {int(rect.right - rect.left),
                              int(rect.bottom - rect.top)};
  const Rml::Vector2i position =
      Rml::Math::Max((screen - size) / 2, Rml::Vector2i(0));
  SetWindowPos(window, HWND_TOP, position.x, position.y, size.x, size.y,
               SWP_NOACTIVATE);
  return window;
}

// A 3.3 core context when the driver offers wglCreateContextAttribsARB, else
// the legacy context (which drivers give as the newest compatibility profile).
// The context is current on the calling thread afterwards.
bool AttachToNative(HWND window, HDC &out_dc, HGLRC &out_rc) {
  HDC dc = GetDC(window);
  if (!dc) {
    Rml::Log::Message(Rml::Log::LT_ERROR, "Failed to get device context");
    return false;
  }
  PIXELFORMATDESCRIPTOR pfd{};
  pfd.nSize = sizeof(pfd);
  pfd.nVersion = 1;
  pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
  pfd.iPixelType = PFD_TYPE_RGBA;
  pfd.cColorBits = 32;
  pfd.cAlphaBits = 8;
  pfd.cDepthBits = 24;
  pfd.cStencilBits = 8;
  const int format = ChoosePixelFormat(dc, &pfd);
  if (!format || !SetPixelFormat(dc, format, &pfd)) {
    Rml::Log::Message(Rml::Log::LT_ERROR, "Failed to set a pixel format");
    return false;
  }
  HGLRC legacy = wglCreateContext(dc);
  if (!legacy || !wglMakeCurrent(dc, legacy)) {
    Rml::Log::Message(Rml::Log::LT_ERROR, "Failed to create an OpenGL context");
    return false;
  }
  HGLRC context = legacy;
  if (const auto create = (ProcCreateContextAttribs)(void *)wglGetProcAddress(
          "wglCreateContextAttribsARB")) {
    const int attributes[] = {WGL_CONTEXT_MAJOR_VERSION_ARB,
                              3,
                              WGL_CONTEXT_MINOR_VERSION_ARB,
                              3,
                              WGL_CONTEXT_PROFILE_MASK_ARB,
                              WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
                              0};
    if (HGLRC core = create(dc, nullptr, attributes)) {
      wglMakeCurrent(nullptr, nullptr);
      wglDeleteContext(legacy);
      wglMakeCurrent(dc, core);
      context = core;
    }
  }
  // Vsync on unless XERCES_SPIKE_VSYNC=0 (for uncapped frame-time runs).
  if (const auto swap_interval =
          (ProcSwapInterval)(void *)wglGetProcAddress("wglSwapIntervalEXT")) {
    char value[8] = {};
    const bool off = GetEnvironmentVariableA("XERCES_SPIKE_VSYNC", value,
                                             sizeof(value)) > 0 &&
                     value[0] == '0';
    swap_interval(off ? 0 : 1);
  }
  out_dc = dc;
  out_rc = context;
  return true;
}

void DetachFromNative(HWND window, HDC dc, HGLRC rc) {
  if (rc) {
    wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(rc);
  }
  if (dc) {
    ReleaseDC(window, dc);
  }
}

// Context, GL functions and shaders; runs on the main thread or the worker.
bool CreateRenderer() {
  if (!AttachToNative(data->window_handle, data->device_context,
                      data->render_context)) {
    return false;
  }
  xerces_spike_mark("wgl context");
  Rml::String message;
  if (!RmlGL3::Initialize(&message)) {
    Rml::Log::Message(Rml::Log::LT_ERROR, "%s", message.c_str());
    return false;
  }
  xerces_spike_mark("gl functions loaded");
  data->render_interface.emplace();
  if (!*data->render_interface) {
    Rml::Log::Message(Rml::Log::LT_ERROR, "GL3 shaders failed to compile");
    return false;
  }
  xerces_spike_mark("renderer (shaders)");
  return true;
}

// Loads and initializes the OpenGL driver with a throwaway context on a hidden
// window of the calling thread. Moving a context between threads crashed
// NVIDIA's driver (nvoglv64.dll), so the worker only warms the driver up and
// the real context is created on the main thread afterwards.
void WarmUpDriver() {
  WNDCLASSW window_class{};
  window_class.lpfnWndProc = DefWindowProcW;
  window_class.hInstance = GetModuleHandleW(nullptr);
  window_class.lpszClassName = L"xerces_spike_gl_warmup";
  RegisterClassW(&window_class);
  HWND window =
      CreateWindowExW(0, window_class.lpszClassName, L"", WS_POPUP, 0, 0, 1, 1,
                      nullptr, nullptr, window_class.hInstance, nullptr);
  if (!window) {
    return;
  }
  HDC dc = GetDC(window);
  PIXELFORMATDESCRIPTOR pfd{};
  pfd.nSize = sizeof(pfd);
  pfd.nVersion = 1;
  pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
  pfd.iPixelType = PFD_TYPE_RGBA;
  pfd.cColorBits = 32;
  pfd.cAlphaBits = 8;
  pfd.cDepthBits = 24;
  pfd.cStencilBits = 8;
  const int format = ChoosePixelFormat(dc, &pfd);
  if (format && SetPixelFormat(dc, format, &pfd)) {
    if (HGLRC context = wglCreateContext(dc)) {
      wglMakeCurrent(dc, context);
      wglMakeCurrent(nullptr, nullptr);
      wglDeleteContext(context);
    }
  }
  ReleaseDC(window, dc);
  DestroyWindow(window);
  UnregisterClassW(window_class.lpszClassName, window_class.hInstance);
}

void EnsureGpu() {
  if (!data || data->gpu_joined) {
    return;
  }
  data->gpu_thread.join();
  data->gpu_joined = true;
  xerces_spike_mark("driver warm-up joined");
  data->gpu_ok = CreateRenderer();
  if (data->gpu_ok) {
    RECT client{};
    GetClientRect(data->window_handle, &client);
    data->window_dimensions = {int(client.right), int(client.bottom)};
    data->render_interface->SetViewport(data->window_dimensions.x,
                                        data->window_dimensions.y);
  }
}

bool NextEvent(MSG &message, UINT timeout) {
  if (timeout != 0) {
    const UINT_PTR timer = SetTimer(nullptr, 0, timeout, nullptr);
    const BOOL result = GetMessageW(&message, nullptr, 0, 0);
    KillTimer(nullptr, timer);
    if (message.message != WM_TIMER || message.hwnd != nullptr ||
        message.wParam != timer) {
      return result;
    }
  }
  return PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE);
}
} // namespace

bool Backend::Initialize(const char *window_name, int width, int height,
                         bool allow_resize) {
  RMLUI_ASSERT(!data);
  const std::wstring name = RmlWin32::ConvertToUTF16(Rml::String(window_name));
  const HINSTANCE instance = GetModuleHandleW(nullptr);
  InitializeDpiSupport();

  data = Rml::MakeUnique<BackendData>();
  data->instance_handle = instance;
  data->instance_name = name;
  HWND window = InitializeWindow(instance, name, width, height, allow_resize);
  if (!window) {
    data.reset();
    return false;
  }
  xerces_spike_mark("window created");
  data->window_handle = window;
  data->system_interface.SetWindow(window);
  RECT client{};
  GetClientRect(window, &client);
  data->window_dimensions = {int(client.right - client.left),
                             int(client.bottom - client.top)};

  if (env_flag("XERCES_SPIKE_ASYNC")) {
    data->async = true;
    ShowWindow(window, SW_SHOW);
    SetForegroundWindow(window);
    SetFocus(window);
    UpdateWindow(window);
    xerces_spike_mark("window shown (native paint)");
    data->gpu_joined = false;
    data->gpu_thread = std::thread([] {
      WarmUpDriver();
      xerces_spike_mark("driver warm-up (worker)");
    });
    data->deferred.emplace([]() -> Rml::RenderInterface * {
      EnsureGpu();
      return &*data->render_interface;
    });
    Rml::SetTextInputHandler(&data->text_input_method_editor);
    return true;
  }

  if (!CreateRenderer()) {
    data->render_interface.reset();
    RmlGL3::Shutdown();
    DetachFromNative(window, data->device_context, data->render_context);
    DestroyWindow(window);
    data.reset();
    return false;
  }
  data->render_interface->SetViewport(data->window_dimensions.x,
                                      data->window_dimensions.y);
  ShowWindow(window, SW_SHOW);
  SetForegroundWindow(window);
  SetFocus(window);
  xerces_spike_mark("window shown");
  Rml::SetTextInputHandler(&data->text_input_method_editor);
  return true;
}

void Backend::Shutdown() {
  RMLUI_ASSERT(data);
  EnsureGpu();
  if (Rml::GetTextInputHandler() == &data->text_input_method_editor) {
    Rml::SetTextInputHandler(nullptr);
  }
  const HWND window = data->window_handle;
  const HDC dc = data->device_context;
  const HGLRC rc = data->render_context;
  const std::wstring name = data->instance_name;
  const HINSTANCE instance = data->instance_handle;
  data->deferred.reset();
  data->render_interface.reset();
  data.reset();
  RmlGL3::Shutdown();
  DetachFromNative(window, dc, rc);
  DestroyWindow(window);
  UnregisterClassW(name.data(), instance);
}

Rml::SystemInterface *Backend::GetSystemInterface() {
  RMLUI_ASSERT(data);
  return &data->system_interface;
}

Rml::RenderInterface *Backend::GetRenderInterface() {
  RMLUI_ASSERT(data);
  if (data->async) {
    return &*data->deferred;
  }
  return &*data->render_interface;
}

bool Backend::ProcessEvents(Rml::Context *context,
                            KeyDownCallback key_down_callback,
                            bool power_save) {
  RMLUI_ASSERT(data && context);
  if (data->context_dimensions_dirty) {
    data->context_dimensions_dirty = false;
    context->SetDimensions(data->window_dimensions);
    context->SetDensityIndependentPixelRatio(
        GetDensityIndependentPixelRatio(data->window_handle));
  }
  data->context = context;
  data->key_down_callback = key_down_callback;

  MSG message;
  bool has_message = NextEvent(
      message,
      power_save
          ? static_cast<UINT>(
                Rml::Math::Min(context->GetNextUpdateDelay(), 10.0) * 1000.0)
          : 0);
  while (has_message) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
    has_message = NextEvent(message, 0);
  }

  data->context = nullptr;
  data->key_down_callback = nullptr;
  const bool result = data->running;
  data->running = true;
  return result;
}

void Backend::RequestExit() {
  RMLUI_ASSERT(data);
  data->running = false;
}

void Backend::BeginFrame() {
  RMLUI_ASSERT(data);
  EnsureGpu();
  data->render_interface->Clear();
  data->render_interface->BeginFrame();
}

void Backend::PresentFrame() {
  RMLUI_ASSERT(data);
  data->render_interface->EndFrame();
  SwapBuffers(data->device_context);
  RMLUI_FrameMark;
}
