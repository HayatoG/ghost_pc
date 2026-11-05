#pragma once

#include "runtime/gs/gs_backend.h"

#include <memory>

// Builds the raster backend chosen by PS2X_GS (default "cpu", also on unknown values):
//  - "cpu"        GSCpuBackend on the caller's threads (reference, MD5-stable);
//  - "cpu-thread" GSThreadedBackend + GSCpuBackend on a dedicated GS thread (alias "cpu-mt");
//                 PS2X_GS_QUEUE_KB=<n> sets the queue size, PS2X_GS_DEBUG=1 prints "[gs-thread]";
//  - "d3d11"      GSGpuBackend (only when built with PS2X_HAS_GS_GPU). Options: PS2X_GS_VSYNC=1,
//                 PS2X_GS_DEBUG=1 (D3D11 debug layer), PS2X_GS_FILTER=linear,
//                 PS2X_GS_GPU_DRAW=0 (stage 1: draw on CPU, only present on GPU), PS2X_GS_WINDOW=0
//                 (hide the D3D11 window).
//  - "hw"         GSHwBackend: hardware renderer (render targets, GPU blend/depth; PSNR-equivalent, not
//                 byte-exact). PS2X_GS_SCALE=1..8, PS2X_GS_WINDOW=0, PS2X_GS_HW_CPUFRAME=N, PS2X_GS_HW_VERBOSE=1
//                 (see gs_backend_factory.cpp and ghost_pc/docs/gs-hw-backend.md).
std::unique_ptr<GSRasterBackend> CreateGSRasterBackendFromEnv();
