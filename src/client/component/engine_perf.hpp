#pragma once

#include <d3d11.h>
#include <dxgi.h>

namespace engine_perf {
// Called by download_overlay::present_stub before forwarding to the original
// Present. Logs the current sync_interval and flags, optionally overrides
// sync_interval, and applies SetMaximumFrameLatency() once if a target has
// been set via console.
void intercept_present(IDXGISwapChain *swap_chain, UINT &sync_interval,
                       UINT &flags);
}  // namespace engine_perf
