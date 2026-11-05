// Shared declarations of the gsreplay harness (see main.cpp).
#pragma once

#include "runtime/gs/gs_backend.h"

#include <cstdint>
#include <cstdio>
#include <vector>

struct Rec
{
    uint8_t type;
    std::vector<uint8_t> data;
};

// Reads the initial VRAM (4 MiB) and every record of a PS2X_GS_RECORD file, without a backend.
bool LoadRecordingRaw(const char *path, std::vector<uint8_t> &vram, std::vector<Rec> &records);

// Hardware-backend commands (hw_tools.cpp): returns -1 when `cmd` is not one of them.
int RunHwCommand(int argc, char **argv);
