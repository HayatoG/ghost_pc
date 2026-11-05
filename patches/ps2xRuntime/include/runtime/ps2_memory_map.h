#ifndef PS2_MEMORY_MAP_H
#define PS2_MEMORY_MAP_H

// Constantes do mapa de memoria do PS2 (extraidas de ps2_memory.h para o codigo gerado
// nao depender do restante do subsistema de memoria).
#include <cstddef>
#include <cstdint>

constexpr uint32_t PS2_RAM_SIZE = 32u * 1024u * 1024u; // 32MB
constexpr uint32_t PS2_RAM_MASK = PS2_RAM_SIZE - 1u;   // Mask for 32MB alignment
constexpr uint32_t PS2_RAM_BASE = 0x00000000;          // Physical base of RDRAM
constexpr uint32_t PS2_SCRATCHPAD_BASE = 0x70000000;
constexpr uint32_t PS2_SCRATCHPAD_ALIAS_BASE = 0xF0000000;
constexpr uint32_t PS2_SCRATCHPAD_SIZE = 16u * 1024u;  // 16KB
constexpr uint32_t PS2_IO_BASE = 0x10000000;           // Base for many I/O regs (Timers, DMAC, INTC)
constexpr uint32_t PS2_IO_SIZE = 0x10000;              // 64KB
constexpr uint32_t PS2_BIOS_BASE = 0x1FC00000;         // Or BFC00000 depending on KSEG
constexpr uint32_t PS2_BIOS_SIZE = 4u * 1024u * 1024u; // 4MB

constexpr uint32_t PS2_VU0_CODE_BASE = 0x11000000; // Base address as seen from EE
constexpr uint32_t PS2_VU0_DATA_BASE = 0x11004000;
constexpr uint32_t PS2_VU0_CODE_SIZE = 4u * 1024u; // 4KB Micro Memory
constexpr uint32_t PS2_VU0_DATA_SIZE = 4u * 1024u; // 4KB Data Memory (VU Mem)

constexpr uint32_t PS2_VU1_CODE_BASE = 0x11008000;
constexpr uint32_t PS2_VU1_DATA_BASE = 0x1100C000;
constexpr uint32_t PS2_VU1_MEM_BASE = PS2_VU1_CODE_BASE; // Alias used by older code paths
constexpr uint32_t PS2_VU1_CODE_SIZE = 16u * 1024u;      // 16KB Micro Memory
constexpr uint32_t PS2_VU1_DATA_SIZE = 16u * 1024u;      // 16KB Data Memory (VU Mem)

constexpr uint32_t PS2_GS_BASE = 0x12000000;
constexpr uint32_t PS2_GS_PRIV_REG_BASE = PS2_GS_BASE; // GS Privileged Registers
constexpr uint32_t PS2_GS_PRIV_REG_SIZE = 0x2000;
constexpr size_t PS2_GS_VRAM_SIZE = 4u * 1024u * 1024u; // 4MB GS VRAM

inline constexpr uint32_t PS2_FIO_O_RDONLY = 0x0001;
inline constexpr uint32_t PS2_FIO_O_WRONLY = 0x0002;
inline constexpr uint32_t PS2_FIO_O_RDWR = 0x0003;
inline constexpr uint32_t PS2_FIO_O_NBLOCK = 0x0010;
inline constexpr uint32_t PS2_FIO_O_APPEND = 0x0100;
inline constexpr uint32_t PS2_FIO_O_CREAT = 0x0200;
inline constexpr uint32_t PS2_FIO_O_TRUNC = 0x0400;
inline constexpr uint32_t PS2_FIO_O_EXCL = 0x0800;
inline constexpr uint32_t PS2_FIO_O_NOWAIT = 0x8000;

inline constexpr uint32_t PS2_FIO_SEEK_SET = 0;
inline constexpr uint32_t PS2_FIO_SEEK_CUR = 1;
inline constexpr uint32_t PS2_FIO_SEEK_END = 2;

inline constexpr uint32_t PS2_FIO_S_IFDIR = 0x1000;
inline constexpr uint32_t PS2_FIO_S_IFREG = 0x2000;

static_assert((PS2_RAM_SIZE & (PS2_RAM_SIZE - 1u)) == 0u, "PS2_RAM_SIZE must be a power of two");
static_assert(PS2_RAM_MASK == (PS2_RAM_SIZE - 1u), "PS2_RAM_MASK must match PS2_RAM_SIZE");

#endif // PS2_MEMORY_MAP_H
