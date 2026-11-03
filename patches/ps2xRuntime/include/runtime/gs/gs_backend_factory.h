#pragma once

#include "runtime/gs/gs_backend.h"

#include <memory>

// Builds the raster backend chosen by PS2X_GS (default "cpu", also on unknown values):
//  - "cpu"        GSCpuBackend on the caller's threads (reference, MD5-stable);
//  - "cpu-thread" GSThreadedBackend + GSCpuBackend on a dedicated GS thread (alias "cpu-mt");
//                 PS2X_GS_QUEUE_KB=<n> sets the queue size, PS2X_GS_DEBUG=1 prints "[gs-thread]";
//  - "d3d11"      GSGpuBackend (only when built with PS2X_HAS_GS_GPU). Options: PS2X_GS_VSYNC=1,
//                 PS2X_GS_DEBUG=1 (D3D11 debug layer), PS2X_GS_FILTER=linear.
std::unique_ptr<GSRasterBackend> CreateGSRasterBackendFromEnv();
