/*
 * The optional NVIDIA DLSS module's interface (docs/DESIGN.md 13.4, D10).
 *
 * MIT licence (src/xerces_dlss/LICENSE): this header is shared by the module,
 * which is built only with premake's --with-nvidia-dlss-sdk and is never part
 * of the GPL release, and by the game client, which loads xerces_dlss.dll at
 * run time when the player picks NVIDIA DLSS. Nothing of NVIDIA's SDK is in
 * this file or in the client.
 *
 * Every texture is the client's and is on the game's device:
 *   color   the scene at the render size, linear, 0 to 1 (DLSS's LDR input)
 *   depth   R32_FLOAT, reversed (1 near, 0 far)
 *   motion  R16G16_FLOAT, in render pixels, from this frame's position to the
 *           last frame's (DLSS's own convention)
 *   output  R16G16B16A16_FLOAT with unordered access, the output size
 * The jitter is the sample's offset from the pixel centre in render pixels, as
 * the projection was moved by it. At an output size equal to the render size
 * the module runs DLAA.
 */
#pragma once

#include <d3d11.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XERCES_DLSS_ABI 2

typedef struct xerces_dlss_frame {
  ID3D11Resource *color;
  ID3D11Resource *depth;
  ID3D11Resource *motion;
  ID3D11Resource *output;
  unsigned render_width;
  unsigned render_height;
  unsigned output_width;
  unsigned output_height;
  float jitter_x;
  float jitter_y;
  float frame_ms;
  float motion_scale; /* multiplies the motion vectors (1 as described) */
  int reset; /* 1 after a cut: a new map, a killcam, a resolution change */
} xerces_dlss_frame;

typedef struct xerces_dlss_api {
  unsigned abi; /* XERCES_DLSS_ABI */
  /* 1 when DLSS can run on this device. NVIDIA's nvngx_dlss.dll is looked
     for in dll_dir; NGX writes its logs under data_dir. On 0, error says
     why. */
  int (*start)(ID3D11Device *device, const wchar_t *dll_dir,
               const wchar_t *data_dir, char *error, unsigned error_size);
  /* Upscales (or, at the same size, anti-aliases) one frame on the
     immediate context. 1 on success. */
  int (*evaluate)(ID3D11DeviceContext *context, const xerces_dlss_frame *frame,
                  char *error, unsigned error_size);
  /* The DLSS mode last used ("DLAA", "Quality", ...), for the log. */
  const char *(*mode)(void);
  void (*stop)(void);
} xerces_dlss_api;

/* The one export: xerces_dlss_get. */
typedef const xerces_dlss_api *(*xerces_dlss_get_fn)(void);

#ifdef __cplusplus
}
#endif
