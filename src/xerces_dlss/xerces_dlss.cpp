// The optional NVIDIA DLSS module (xerces_dlss.h, docs/DESIGN.md 13.4, D10).
// MIT licence (LICENSE in this folder). Built only with premake's
// --with-nvidia-dlss-sdk=<folder>, against NVIDIA's DLSS SDK, which stays
// under NVIDIA's licence outside the repository; never part of the GPL
// release.

#include "xerces_dlss.h"

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>
#include <nvsdk_ngx_helpers_d3d.h>

#include <cstdio>
#include <string>

namespace {
// NGX's project id for a custom engine: any UUID, Xerces' own.
constexpr char project_id[] = "6f2d8a1e-3c47-4b9e-a5d0-81c9e27b4f13";
constexpr char engine_version[] = "1.0";

ID3D11Device *device_ = nullptr;
NVSDK_NGX_Parameter *params_ = nullptr;
NVSDK_NGX_Handle *feature_ = nullptr;
bool started_ = false;
bool available_ = false;
const char *mode_ = "none";

struct feature_key {
  unsigned render_width = 0;
  unsigned render_height = 0;
  unsigned output_width = 0;
  unsigned output_height = 0;
  int quality = -1;
  int flags = 0;

  bool operator==(const feature_key &) const = default;
};
feature_key created_{};

void say(char *error, unsigned size, const std::string &text) {
  if (error && size) {
    std::snprintf(error, size, "%s", text.c_str());
  }
}

std::string result_text(const NVSDK_NGX_Result result) {
  char text[64];
  std::snprintf(text, sizeof(text), "NGX result 0x%08X",
                static_cast<unsigned>(result));
  return text;
}

const char *quality_name(const int quality) {
  switch (quality) {
  case NVSDK_NGX_PerfQuality_Value_DLAA:
    return "DLAA";
  case NVSDK_NGX_PerfQuality_Value_UltraQuality:
    return "Ultra Quality";
  case NVSDK_NGX_PerfQuality_Value_MaxQuality:
    return "Quality";
  case NVSDK_NGX_PerfQuality_Value_Balanced:
    return "Balanced";
  case NVSDK_NGX_PerfQuality_Value_MaxPerf:
    return "Performance";
  case NVSDK_NGX_PerfQuality_Value_UltraPerformance:
    return "Ultra Performance";
  default:
    return "?";
  }
}

// The DLSS mode whose render size range holds the game's render size: DLAA
// at the same size, else the first mode (best quality first) whose minimum
// and maximum render sizes for this output contain it. -1 when none does.
int pick_quality(const xerces_dlss_frame &frame) {
  if (frame.render_width >= frame.output_width &&
      frame.render_height >= frame.output_height) {
    return NVSDK_NGX_PerfQuality_Value_DLAA;
  }
  const int order[] = {NVSDK_NGX_PerfQuality_Value_UltraQuality,
                       NVSDK_NGX_PerfQuality_Value_MaxQuality,
                       NVSDK_NGX_PerfQuality_Value_Balanced,
                       NVSDK_NGX_PerfQuality_Value_MaxPerf,
                       NVSDK_NGX_PerfQuality_Value_UltraPerformance};
  for (const int quality : order) {
    unsigned optimal_w = 0, optimal_h = 0, max_w = 0, max_h = 0, min_w = 0,
             min_h = 0;
    float sharpness = 0.0f;
    const NVSDK_NGX_Result result = NGX_DLSS_GET_OPTIMAL_SETTINGS(
        params_, frame.output_width, frame.output_height,
        static_cast<NVSDK_NGX_PerfQuality_Value>(quality), &optimal_w,
        &optimal_h, &max_w, &max_h, &min_w, &min_h, &sharpness);
    if (NVSDK_NGX_FAILED(result) || optimal_w == 0) {
      continue;
    }
    if (max_w == 0) {
      max_w = optimal_w;
      max_h = optimal_h;
      min_w = optimal_w;
      min_h = optimal_h;
    }
    if (frame.render_width >= min_w && frame.render_width <= max_w &&
        frame.render_height >= min_h && frame.render_height <= max_h) {
      return quality;
    }
  }
  return -1;
}

void release_feature() {
  if (feature_) {
    NVSDK_NGX_D3D11_ReleaseFeature(feature_);
    feature_ = nullptr;
  }
  created_ = {};
}

int start(ID3D11Device *device, const wchar_t *dll_dir, const wchar_t *data_dir,
          char *error, const unsigned error_size) {
  if (started_) {
    if (!available_) {
      say(error, error_size, "DLSS was not available when the module started");
    }
    return available_ ? 1 : 0;
  }
  started_ = true;
  device_ = device;
  const wchar_t *paths[] = {dll_dir};
  NVSDK_NGX_FeatureCommonInfo info{};
  info.PathListInfo.Path = paths;
  info.PathListInfo.Length = dll_dir ? 1 : 0;
  NVSDK_NGX_Result result = NVSDK_NGX_D3D11_Init_with_ProjectID(
      project_id, NVSDK_NGX_ENGINE_TYPE_CUSTOM, engine_version, data_dir,
      device, &info, NVSDK_NGX_Version_API);
  if (NVSDK_NGX_FAILED(result)) {
    say(error, error_size,
        "NGX did not start (" + result_text(result) +
            "; it needs an NVIDIA RTX GPU and a current driver)");
    return 0;
  }
  result = NVSDK_NGX_D3D11_GetCapabilityParameters(&params_);
  if (NVSDK_NGX_FAILED(result) || !params_) {
    say(error, error_size,
        "NGX gave no capability parameters (" + result_text(result) + ")");
    return 0;
  }
  int supported = 0;
  NVSDK_NGX_Parameter_GetI(params_, NVSDK_NGX_Parameter_SuperSampling_Available,
                           &supported);
  if (!supported) {
    int needs_driver = 0;
    int init_result = 0;
    NVSDK_NGX_Parameter_GetI(
        params_, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver,
        &needs_driver);
    NVSDK_NGX_Parameter_GetI(
        params_, NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult,
        &init_result);
    say(error, error_size,
        needs_driver
            ? std::string("DLSS needs a newer NVIDIA driver")
            : "DLSS is not available on this GPU (" +
                  result_text(static_cast<NVSDK_NGX_Result>(init_result)) +
                  "; nvngx_dlss.dll missing or not an RTX GPU)");
    return 0;
  }
  available_ = true;
  return 1;
}

int evaluate(ID3D11DeviceContext *context, const xerces_dlss_frame *frame,
             char *error, const unsigned error_size) {
  if (!available_ || !frame || !context) {
    say(error, error_size, "DLSS is not running");
    return 0;
  }
  feature_key key;
  key.render_width = frame->render_width;
  key.render_height = frame->render_height;
  key.output_width = frame->output_width;
  key.output_height = frame->output_height;
  key.flags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
              NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
  if (!(feature_ && key.render_width == created_.render_width &&
        key.render_height == created_.render_height &&
        key.output_width == created_.output_width &&
        key.output_height == created_.output_height)) {
    key.quality = pick_quality(*frame);
    if (key.quality < 0) {
      say(error, error_size,
          "no DLSS mode takes this render size for this output size");
      return 0;
    }
  } else {
    key.quality = created_.quality;
  }
  bool reset = frame->reset != 0;
  if (!feature_ || !(key == created_)) {
    release_feature();
    NVSDK_NGX_DLSS_Create_Params create{};
    create.Feature.InWidth = key.render_width;
    create.Feature.InHeight = key.render_height;
    create.Feature.InTargetWidth = key.output_width;
    create.Feature.InTargetHeight = key.output_height;
    create.Feature.InPerfQualityValue =
        static_cast<NVSDK_NGX_PerfQuality_Value>(key.quality);
    create.InFeatureCreateFlags = key.flags;
    const NVSDK_NGX_Result result =
        NGX_D3D11_CREATE_DLSS_EXT(context, &feature_, params_, &create);
    if (NVSDK_NGX_FAILED(result) || !feature_) {
      feature_ = nullptr;
      say(error, error_size,
          "DLSS could not be created (" + result_text(result) + ")");
      return 0;
    }
    created_ = key;
    mode_ = quality_name(key.quality);
    reset = true;
  }
  NVSDK_NGX_D3D11_DLSS_Eval_Params eval{};
  eval.Feature.pInColor = frame->color;
  eval.Feature.pInOutput = frame->output;
  eval.pInDepth = frame->depth;
  eval.pInMotionVectors = frame->motion;
  eval.InJitterOffsetX = frame->jitter_x;
  eval.InJitterOffsetY = frame->jitter_y;
  eval.InRenderSubrectDimensions.Width = frame->render_width;
  eval.InRenderSubrectDimensions.Height = frame->render_height;
  eval.InReset = reset ? 1 : 0;
  const float motion = frame->motion_scale != 0.0f ? frame->motion_scale : 1.0f;
  eval.InMVScaleX = motion;
  eval.InMVScaleY = motion;
  eval.InPreExposure = 1.0f;
  eval.InExposureScale = 1.0f;
  eval.InFrameTimeDeltaInMsec = frame->frame_ms;
  const NVSDK_NGX_Result result =
      NGX_D3D11_EVALUATE_DLSS_EXT(context, feature_, params_, &eval);
  if (NVSDK_NGX_FAILED(result)) {
    say(error, error_size,
        "DLSS failed on a frame (" + result_text(result) + ")");
    return 0;
  }
  return 1;
}

const char *mode() { return mode_; }

void stop() {
  release_feature();
  if (params_) {
    NVSDK_NGX_D3D11_DestroyParameters(params_);
    params_ = nullptr;
  }
  if (started_ && device_) {
    NVSDK_NGX_D3D11_Shutdown1(device_);
  }
  device_ = nullptr;
  started_ = false;
  available_ = false;
}

const xerces_dlss_api api = {XERCES_DLSS_ABI, start, evaluate, mode, stop};
} // namespace

extern "C" __declspec(dllexport) const xerces_dlss_api *xerces_dlss_get() {
  return &api;
}
