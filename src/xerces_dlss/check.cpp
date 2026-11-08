// xerces_dlss_check: loads xerces_dlss.dll from its own folder, starts DLSS
// on the first hardware adapter and runs one frame each of DLAA (1920x1080)
// and an upscale (2560x1440 to 3840x2160) on blank textures. Prints what
// happened; exit code 0 when both frames ran. MIT licence (LICENSE here).

#include "xerces_dlss.h"

#include <windows.h>

#include <cstdio>
#include <string>

namespace {
std::wstring own_folder() {
  wchar_t path[MAX_PATH]{};
  GetModuleFileNameW(nullptr, path, MAX_PATH);
  std::wstring folder(path);
  return folder.substr(0, folder.find_last_of(L"\\/"));
}

ID3D11Texture2D *texture(ID3D11Device *device, UINT width, UINT height,
                         DXGI_FORMAT format, UINT bind) {
  D3D11_TEXTURE2D_DESC desc{};
  desc.Width = width;
  desc.Height = height;
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = format;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = bind;
  ID3D11Texture2D *out = nullptr;
  device->CreateTexture2D(&desc, nullptr, &out);
  return out;
}

bool one_frame(const xerces_dlss_api *api, ID3D11Device *device,
               ID3D11DeviceContext *context, UINT rw, UINT rh, UINT ow,
               UINT oh) {
  ID3D11Texture2D *color =
      texture(device, rw, rh, DXGI_FORMAT_R16G16B16A16_FLOAT,
              D3D11_BIND_SHADER_RESOURCE);
  ID3D11Texture2D *depth = texture(device, rw, rh, DXGI_FORMAT_R32_FLOAT,
                                   D3D11_BIND_SHADER_RESOURCE);
  ID3D11Texture2D *motion = texture(device, rw, rh, DXGI_FORMAT_R16G16_FLOAT,
                                    D3D11_BIND_SHADER_RESOURCE);
  ID3D11Texture2D *output =
      texture(device, ow, oh, DXGI_FORMAT_R16G16B16A16_FLOAT,
              D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
  bool ok = false;
  if (color && depth && motion && output) {
    xerces_dlss_frame frame{};
    frame.color = color;
    frame.depth = depth;
    frame.motion = motion;
    frame.output = output;
    frame.render_width = rw;
    frame.render_height = rh;
    frame.output_width = ow;
    frame.output_height = oh;
    frame.frame_ms = 16.6f;
    frame.motion_scale = 1.0f;
    frame.reset = 1;
    char error[512]{};
    ok = api->evaluate(context, &frame, error, sizeof(error)) == 1;
    context->Flush();
    std::printf("%ux%u -> %ux%u: %s (mode %s) %s\n", rw, rh, ow, oh,
                ok ? "ran" : "failed", api->mode(), error);
  } else {
    std::printf("%ux%u -> %ux%u: textures could not be made\n", rw, rh, ow, oh);
  }
  for (ID3D11Texture2D *t : {color, depth, motion, output}) {
    if (t) {
      t->Release();
    }
  }
  return ok;
}
} // namespace

int main() {
  const std::wstring folder = own_folder();
  HMODULE module = LoadLibraryW((folder + L"\\xerces_dlss.dll").c_str());
  if (!module) {
    std::printf("xerces_dlss.dll not found next to this program\n");
    return 2;
  }
  const auto get = reinterpret_cast<xerces_dlss_get_fn>(
      GetProcAddress(module, "xerces_dlss_get"));
  const xerces_dlss_api *api = get ? get() : nullptr;
  if (!api || api->abi != XERCES_DLSS_ABI) {
    std::printf("xerces_dlss.dll has no matching xerces_dlss_get\n");
    return 2;
  }
  ID3D11Device *device = nullptr;
  ID3D11DeviceContext *context = nullptr;
  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                               nullptr, 0, D3D11_SDK_VERSION, &device, nullptr,
                               &context))) {
    std::printf("no hardware Direct3D 11 device\n");
    return 2;
  }
  char error[512]{};
  const int started =
      api->start(device, folder.c_str(), folder.c_str(), error, sizeof(error));
  std::printf("start: %s %s\n", started ? "DLSS available" : "no DLSS", error);
  int code = 1;
  if (started) {
    const bool dlaa = one_frame(api, device, context, 1920, 1080, 1920, 1080);
    const bool upscale =
        one_frame(api, device, context, 2560, 1440, 3840, 2160);
    code = dlaa && upscale ? 0 : 1;
  }
  api->stop();
  context->Release();
  device->Release();
  return code;
}
