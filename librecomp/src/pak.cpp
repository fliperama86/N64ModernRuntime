#include "ultramodern/ultra64.h"
#include "ultramodern/ultramodern.hpp"

#include "recomp.h"
#include "helpers.hpp"

#include <cstdio>
#include <cstring>
#include <mutex>

// ── Virtual Controller Pak (Memory Pak) ────────────────────────────
//
// Implements the N64 PFS (Pak File System) API for save game support.
// Uses a simple flat file table backed by the runtime's save buffer.
//
// Layout (32KB = 0x8000):
//   0x0000-0x00FF: Directory (16 entries × 16 bytes)
//   0x0100-0x7FFF: File data area
//
// The save buffer is managed by the runtime's save_context (pi.cpp),
// which handles persistence to disk via background thread.

static constexpr int PFS_MAX_FILES = 16;
static constexpr int PFS_DIR_SIZE = PFS_MAX_FILES * 16;  // 256 bytes
// N64 Controller Pak layout: pages 0-4 = system, pages 5-127 = data
// Data starts at page 5 (offset 0x500), 123 pages × 256 bytes = 31488 bytes
static constexpr int PFS_PAGE_SIZE = 256;
static constexpr int PFS_DATA_START_PAGE = 5;
static constexpr int PFS_DATA_PAGES = 123;  // pages 5-127
static constexpr int PFS_DATA_START = PFS_DATA_START_PAGE * PFS_PAGE_SIZE; // 0x500
static constexpr int PFS_TOTAL_SIZE = 0x8000; // 32KB
static constexpr int PFS_DATA_SIZE = PFS_DATA_PAGES * PFS_PAGE_SIZE; // 31488 bytes

// PFS error codes (from N64 SDK)
static constexpr int PFS_ERR_NOPACK = 1;
static constexpr int PFS_ERR_ID_FATAL = 11;
static constexpr int PFS_ERR_INCONSISTENT = 14;

// Read/write flags
static constexpr int PFS_READ = 0;
static constexpr int PFS_WRITE = 1;

struct PfsFileEntry {
    uint16_t company_code;
    uint32_t game_code;
    uint8_t  game_name[4];
    uint8_t  ext_name[4];
    uint16_t data_offset;   // relative to PFS_DATA_START
    uint16_t data_size;     // allocated size
};

// Save buffer accessors (defined in pi.cpp)
extern void save_write_ptr(const void* in, uint32_t offset, uint32_t count);
extern void save_read_ptr(void* out, uint32_t offset, uint32_t count);

// In-memory directory cache (mirrors what's in save buffer)
static PfsFileEntry pfs_dir[PFS_MAX_FILES] = {};
static bool pfs_initialized = false;
static std::mutex pfs_mutex;

static void pfs_load_dir() {
    uint8_t buf[PFS_DIR_SIZE];
    save_read_ptr(buf, 0, PFS_DIR_SIZE);
    memcpy(pfs_dir, buf, sizeof(pfs_dir));
}

static void pfs_save_dir() {
    save_write_ptr(pfs_dir, 0, sizeof(pfs_dir));
}

// N64 Controller Pak formatted header — ID area + inode table.
// Extracted from a mupen64plus .mpk file (real formatted pak).
// Without this, the game's osPfsChecker sees all-zeros and reports "abnormality".
static const uint8_t pak_format_header[] = {
    // Page 0: ID area (serial, device_id, banks, checksum)
    0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
    0x7C,0x82,0x0C,0x60, 0xD4,0x88,0x73,0xF7, 0xAA,0x95,0xE4,0x07, 0x78,0x66,0x6D,0x69,
    0xC3,0x14,0xC4,0x41, 0x35,0x34,0xFC,0xA5, 0x00,0x01,0x01,0x00, 0xFF,0xFB,0xFF,0xF7,
    0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
    0x7C,0x82,0x0C,0x60, 0xD4,0x88,0x73,0xF7, 0xAA,0x95,0xE4,0x07, 0x78,0x66,0x6D,0x69,
    0xC3,0x14,0xC4,0x41, 0x35,0x34,0xFC,0xA5, 0x00,0x01,0x01,0x00, 0xFF,0xFB,0xFF,0xF7,
    // Page 1: ID area mirror
    0x7C,0x82,0x0C,0x60, 0xD4,0x88,0x73,0xF7, 0xAA,0x95,0xE4,0x07, 0x78,0x66,0x6D,0x69,
    0xC3,0x14,0xC4,0x41, 0x35,0x34,0xFC,0xA5, 0x00,0x01,0x01,0x00, 0xFF,0xFB,0xFF,0xF7,
    0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
    0x7C,0x82,0x0C,0x60, 0xD4,0x88,0x73,0xF7, 0xAA,0x95,0xE4,0x07, 0x78,0x66,0x6D,0x69,
    0xC3,0x14,0xC4,0x41, 0x35,0x34,0xFC,0xA5, 0x00,0x01,0x01,0x00, 0xFF,0xFB,0xFF,0xF7,
    0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
    // Page 2: Inode table (0x00 0x71 = checksum, then 0x00 0x03 = free pages)
    0x00,0x71,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x03, 0x00,0x03,0x00,0x03,
    0x00,0x03,0x00,0x03, 0x00,0x03,0x00,0x03, 0x00,0x03,0x00,0x03, 0x00,0x03,0x00,0x03,
    0x00,0x03,0x00,0x03, 0x00,0x03,0x00,0x03, 0x00,0x03,0x00,0x03, 0x00,0x03,0x00,0x03,
    0x00,0x03,0x00,0x03, 0x00,0x03,0x00,0x03, 0x00,0x03,0x00,0x03, 0x00,0x03,0x00,0x03,
    0x00,0x03,0x00,0x03, 0x00,0x03,0x00,0x03, 0x00,0x03,0x00,0x03, 0x00,0x03,0x00,0x03,
    0x00,0x03,0x00,0x03, 0x00,0x03,0x00,0x03, 0x00,0x03,0x00,0x03, 0x00,0x03,0x00,0x03,
    0x00,0x03,0x00,0x03, 0x00,0x03,0x00,0x03, 0x00,0x03,0x00,0x03, 0x00,0x03,0x00,0x03,
    0x00,0x03,0x00,0x03, 0x00,0x03,0x00,0x03, 0x00,0x03,0x00,0x03, 0x00,0x03,0x00,0x03,
    0x00,0x03,0x00,0x03, 0x00,0x03,0x00,0x03, 0x00,0x03,0x00,0x03, 0x00,0x03,0x00,0x03,
    0x00,0x03,0x00,0x03, 0x00,0x03,0x00,0x03, 0x00,0x03,0x00,0x03, 0x00,0x03,0x00,0x03,
    0x00,0x03,0x00,0x03, 0x00,0x03,0x00,0x03, 0x00,0x03,0x00,0x03, 0x00,0x03,0x00,0x03,
    0x00,0x03,0x00,0x03, 0x00,0x03,0x00,0x03, 0x00,0x03,0x00,0x03, 0x00,0x03,0x00,0x03,
    0x00,0x03,0x00,0x03, 0x00,0x03,0x00,0x03, 0x00,0x03,0x00,0x03, 0x00,0x03,0x00,0x03,
    0x00,0x03,0x00,0x03, 0x00,0x03,0x00,0x03, 0x00,0x03,0x00,0x03, 0x00,0x03,0x00,0x03,
    0x00,0x03,0x00,0x03, 0x00,0x03,0x00,0x03, 0x00,0x03,0x00,0x03, 0x00,0x03,0x00,0x03,
    0x00,0x03,0x00,0x03, 0x00,0x03,0x00,0x03, 0x00,0x03,0x00,0x03, 0x00,0x03,0x00,0x03,
};

// Generate a properly formatted empty N64 Controller Pak.
// Layout (32KB = 128 pages × 256 bytes):
//   Page 0   (0x000-0x0FF): ID area (serial, device info, checksums) — 4 copies of 32-byte blocks
//   Page 1   (0x100-0x1FF): Inode table (128 entries × 2 bytes)
//   Page 2   (0x200-0x2FF): Inode table backup
//   Pages 3-4 (0x300-0x4FF): Directory (16 note entries × 32 bytes)
//   Pages 5-127 (0x500-0x7FFF): Data area (123 pages free)
static void format_empty_pak() {
    // --- Page 0: ID area ---
    uint8_t id_block[32];
    memset(id_block, 0, 32);
    // Bytes 0x19-0x1A: Device ID
    id_block[0x19] = 0x00; id_block[0x1A] = 0x01;
    // Byte 0x1B: Banks (1 bank)
    id_block[0x1B] = 0x00;
    // Byte 0x1C-0x1D: Checksum, 0x1E-0x1F: Inverse checksum
    // Checksum = sum of bytes 0-0x1B inverted bitwise
    uint8_t sum1 = 0, sum2 = 0;
    for (int i = 0; i < 0x1C; i += 2) {
        sum1 += id_block[i];
        sum2 += id_block[i + 1];
    }
    id_block[0x1C] = ~sum1; id_block[0x1D] = ~sum2;
    id_block[0x1E] = sum1;  id_block[0x1F] = sum2;

    // Write 4 copies of the ID block to fill page 0 (256 bytes)
    for (int i = 0; i < 8; i++) {
        save_write_ptr(id_block, i * 32, 32);
    }

    // --- Pages 1-2: Inode table + backup ---
    uint8_t inode_table[256];
    memset(inode_table, 0, 256);
    // Entries 0-4: system pages (0x0001 = start page / end of chain)
    for (int i = 1; i <= 4; i++) {
        inode_table[i * 2 + 0] = 0x00;
        inode_table[i * 2 + 1] = 0x01; // PFS_EOF
    }
    // Entries 5-127: free pages (0x0003)
    for (int i = 5; i < 128; i++) {
        inode_table[i * 2 + 0] = 0x00;
        inode_table[i * 2 + 1] = 0x03; // PFS_PAGE_FREE
    }
    // Entry 0: checksum (sum of all other entries)
    uint8_t csum_hi = 0, csum_lo = 0;
    for (int i = 1; i < 128; i++) {
        uint16_t prev = (csum_hi << 8) | csum_lo;
        uint16_t val = (inode_table[i * 2] << 8) | inode_table[i * 2 + 1];
        uint16_t s = prev + val;
        csum_hi = s >> 8;
        csum_lo = s & 0xFF;
    }
    inode_table[0] = csum_hi;
    inode_table[1] = csum_lo;

    save_write_ptr(inode_table, 0x100, 256); // Page 1: inode table
    save_write_ptr(inode_table, 0x200, 256); // Page 2: inode backup

    // --- Pages 3-4: Directory (all zeros = empty) ---
    uint8_t zeros[512];
    memset(zeros, 0, 512);
    save_write_ptr(zeros, 0x300, 512);

    fprintf(stderr, "[PAK] Formatted empty Controller Pak (32KB, 123 free pages, checksum=0x%02X%02X)\n",
            csum_hi, csum_lo);
}

static void pfs_init() {
    if (pfs_initialized) return;
    fprintf(stderr, "[PAK] pfs_init: checking save buffer...\n");
    uint8_t check[4];
    save_read_ptr(check, 0x100, 4);  // read inode table checksum
    fprintf(stderr, "[PAK] pfs_init: inode[0]=[%02X %02X %02X %02X]\n", check[0], check[1], check[2], check[3]);
    if (check[0] == 0 && check[1] == 0 && check[2] == 0 && check[3] == 0) {
        format_empty_pak();
    } else {
        fprintf(stderr, "[PAK] Save buffer already has data\n");
    }
    pfs_initialized = true;
}

static int pfs_find(uint16_t company, uint32_t game, const uint8_t* name, const uint8_t* ext) {
    for (int i = 0; i < PFS_MAX_FILES; i++) {
        if (pfs_dir[i].data_size == 0) continue;
        if (pfs_dir[i].company_code == company &&
            pfs_dir[i].game_code == game &&
            memcmp(pfs_dir[i].game_name, name, 4) == 0 &&
            memcmp(pfs_dir[i].ext_name, ext, 4) == 0) {
            return i;
        }
    }
    return -1;
}

static int pfs_free_bytes() {
    int used = 0;
    for (int i = 0; i < PFS_MAX_FILES; i++) {
        used += pfs_dir[i].data_size;
    }
    return PFS_DATA_SIZE - used;
}

static int pfs_next_offset() {
    int offset = 0;
    for (int i = 0; i < PFS_MAX_FILES; i++) {
        int end = pfs_dir[i].data_offset + pfs_dir[i].data_size;
        if (end > offset) offset = end;
    }
    return offset;
}

// ── Low-level pak read/write (called from PIF command handler) ────

// Check if the save system is ready (buffer allocated)
extern bool pak_save_ready();

// Probe area at 0x8000-0x80FF: 256-byte RAM that echoes writes back.
// Used by the game's pak detection (write test pattern, read back, compare).
// On real N64 Controller Pak, this maps to the SRAM control register area.
static uint8_t pak_probe_area[256] = {};

extern "C" void pak_read(uint16_t addr, uint8_t* out, int count) {
    if (!pak_save_ready()) { memset(out, 0, count); return; }
    std::lock_guard lock{pfs_mutex};
    pfs_init();
    if (addr + count <= PFS_TOTAL_SIZE) {
        save_read_ptr(out, addr, count);
    } else if (addr >= 0x8000 && addr < 0x8100) {
        // Probe area: return previously written data
        int offset = addr - 0x8000;
        int safe = (offset + count <= 256) ? count : 256 - offset;
        if (safe > 0) memcpy(out, pak_probe_area + offset, safe);
        if (safe < count) memset(out + safe, 0, count - safe);
    } else {
        memset(out, 0, count);
    }
}

extern "C" void pak_write(uint16_t addr, const uint8_t* in, int count) {
    if (!pak_save_ready()) return;
    std::lock_guard lock{pfs_mutex};
    pfs_init();
    if (addr + count <= PFS_TOTAL_SIZE) {
        save_write_ptr(in, addr, count);
    } else if (addr >= 0x8000 && addr < 0x8100) {
        // Probe area: store data for read-back
        int offset = addr - 0x8000;
        int safe = (offset + count <= 256) ? count : 256 - offset;
        if (safe > 0) memcpy(pak_probe_area + offset, in, safe);
    }
}

// ── PFS API implementations ───────────────────────────────────────

extern "C" void osPfsInitPak_recomp(uint8_t* rdram, recomp_context* ctx) {
    // s32 osPfsInitPak(OSMesgQueue *queue, OSPfs *pfs, int channel)
    if (!pak_save_ready()) { ctx->r2 = 1; return; } // not ready yet
    std::lock_guard lock{pfs_mutex};
    pfs_init();

    uint32_t pfs_addr = (uint32_t)ctx->r5;
    int channel = (int)ctx->r6;

    fprintf(stderr, "[PAK] osPfsInitPak pfs=0x%08X channel=%d\n", pfs_addr, channel);

    if (pfs_addr >= 0x80000000 && pfs_addr < 0x80800000) {
        OSPfs* pfs = TO_PTR(OSPfs, (gpr)(int32_t)pfs_addr);
        // Status bits: 0x01 = PFS_INITIALIZED, 0x04 = PFS_ID_DONE
        // All subsequent osPfs functions check (status & 0x1) before proceeding.
        pfs->status = 0x05; // initialized + ID checked
        pfs->channel = channel;
    }
    ctx->r2 = 0; // success
}

extern "C" void osPfsFreeBlocks_recomp(uint8_t* rdram, recomp_context* ctx) {
    // s32 osPfsFreeBlocks(OSPfs *pfs, s32 *bytes_not_used)
    std::lock_guard lock{pfs_mutex};
    pfs_init();

    s32* bytes_out = _arg<1, s32*>(rdram, ctx);
    int free = pfs_free_bytes();
    *bytes_out = free;
    static int c = 0; if (++c <= 5) fprintf(stderr, "[PAK] osPfsFreeBlocks: %d bytes free (ptr=%p)\n", free, (void*)bytes_out);
    ctx->r2 = 0;
}

extern "C" void osPfsAllocateFile_recomp(uint8_t* rdram, recomp_context* ctx) {
    // s32 osPfsAllocateFile(OSPfs *pfs, u16 company, u32 game_code,
    //                       u8 *game_name, u8 *ext_name, int length, s32 *file_no)
    std::lock_guard lock{pfs_mutex};
    pfs_init();

    uint16_t company = (uint16_t)ctx->r5;
    uint32_t game_code = (uint32_t)ctx->r6;
    gpr name_addr = (gpr)(int32_t)(uint32_t)ctx->r7;
    gpr ext_addr = (gpr)(int32_t)(uint32_t)MEM_W(0x10, ctx->r29);
    int length = (int)MEM_W(0x14, ctx->r29);
    PTR(s32) file_no_ptr = MEM_W(0x18, ctx->r29);

    uint8_t name[4], ext[4];
    for (int i = 0; i < 4; i++) {
        name[i] = MEM_BU(i, name_addr);
        ext[i] = MEM_BU(i, ext_addr);
    }

    // Check if already exists
    int existing = pfs_find(company, game_code, name, ext);
    if (existing >= 0) {
        if (file_no_ptr) *TO_PTR(s32, file_no_ptr) = existing;
        ctx->r2 = 0;
        return;
    }

    // Find free slot
    int slot = -1;
    for (int i = 0; i < PFS_MAX_FILES; i++) {
        if (pfs_dir[i].data_size == 0) { slot = i; break; }
    }
    if (slot < 0 || pfs_free_bytes() < length) {
        ctx->r2 = 4; // PFS_ERR_FULL
        return;
    }

    // Align length to 256-byte pages (N64 PFS page size)
    length = (length + 255) & ~255;

    pfs_dir[slot].company_code = company;
    pfs_dir[slot].game_code = game_code;
    memcpy(pfs_dir[slot].game_name, name, 4);
    memcpy(pfs_dir[slot].ext_name, ext, 4);
    pfs_dir[slot].data_offset = pfs_next_offset();
    pfs_dir[slot].data_size = length;

    pfs_save_dir();

    if (file_no_ptr) *TO_PTR(s32, file_no_ptr) = slot;

    fprintf(stderr, "[PAK] AllocateFile slot=%d company=0x%04X game=0x%08X size=%d → OK\n",
            slot, company, game_code, length);
    ctx->r2 = 0;
}

extern "C" void osPfsDeleteFile_recomp(uint8_t* rdram, recomp_context* ctx) {
    // s32 osPfsDeleteFile(OSPfs *pfs, u16 company, u32 game_code, u8 *name, u8 *ext)
    std::lock_guard lock{pfs_mutex};
    pfs_init();

    uint16_t company = (uint16_t)ctx->r5;
    uint32_t game_code = (uint32_t)ctx->r6;
    gpr name_addr = (gpr)(int32_t)(uint32_t)ctx->r7;
    gpr ext_addr = (gpr)(int32_t)(uint32_t)MEM_W(0x10, ctx->r29);

    uint8_t name[4], ext[4];
    for (int i = 0; i < 4; i++) {
        name[i] = MEM_BU(i, name_addr);
        ext[i] = MEM_BU(i, ext_addr);
    }

    int slot = pfs_find(company, game_code, name, ext);
    if (slot < 0) {
        ctx->r2 = 5; // PFS_ERR_INVALID
        return;
    }

    memset(&pfs_dir[slot], 0, sizeof(PfsFileEntry));
    pfs_save_dir();
    ctx->r2 = 0;
}

extern "C" void osPfsFileState_recomp(uint8_t* rdram, recomp_context* ctx) {
    // s32 osPfsFileState(OSPfs *pfs, s32 file_no, OSPfsState *state)
    std::lock_guard lock{pfs_mutex};
    pfs_init();

    int file_no = (int)ctx->r5;
    // OSPfsState: { u32 file_size; u32 game_code; u16 company_code; char game_name[4]; char ext_name[4]; }
    // Total 18 bytes

    if (file_no < 0 || file_no >= PFS_MAX_FILES || pfs_dir[file_no].data_size == 0) {
        ctx->r2 = 5; // PFS_ERR_INVALID
        return;
    }

    PTR(void) state_ptr = (gpr)ctx->r6;
    // Write OSPfsState in big-endian (game reads via MEM_W)
    MEM_W(0x00, state_ptr) = pfs_dir[file_no].data_size;
    MEM_W(0x04, state_ptr) = pfs_dir[file_no].game_code;
    MEM_H(0x08, state_ptr) = pfs_dir[file_no].company_code;
    for (int i = 0; i < 4; i++) {
        MEM_B(0x0A + i, state_ptr) = pfs_dir[file_no].game_name[i];
        MEM_B(0x0E + i, state_ptr) = pfs_dir[file_no].ext_name[i];
    }
    ctx->r2 = 0;
}

extern "C" void osPfsFindFile_recomp(uint8_t* rdram, recomp_context* ctx) {
    // s32 osPfsFindFile(OSPfs *pfs, u16 company, u32 game_code, u8 *name, u8 *ext, s32 *file_no)
    std::lock_guard lock{pfs_mutex};
    pfs_init();

    uint16_t company = (uint16_t)ctx->r5;
    uint32_t game_code = (uint32_t)ctx->r6;
    gpr name_addr = (gpr)(int32_t)(uint32_t)ctx->r7;
    gpr ext_addr = (gpr)(int32_t)(uint32_t)MEM_W(0x10, ctx->r29);
    PTR(s32) file_no_ptr = MEM_W(0x14, ctx->r29);

    uint8_t name[4], ext[4];
    for (int i = 0; i < 4; i++) {
        name[i] = MEM_BU(i, name_addr);
        ext[i] = MEM_BU(i, ext_addr);
    }

    int slot = pfs_find(company, game_code, name, ext);
    if (slot < 0) {
        // The game's save screen (gs=4) calls FindFile 3 times:
        //   1. Init check → "No save found" menu shown
        //   2. Re-check via func_800A16A0
        //   3. After user selects "Create Note Now" → InitPak + FindFile
        // The NI overlay's allocate wrapper is never called (game bug or
        // missing call path), so we auto-create the file on the 3rd miss.
        static int find_miss_count = 0;
        find_miss_count++;
        if (find_miss_count >= 3) {
            int free_slot = -1;
            for (int i = 0; i < PFS_MAX_FILES; i++) {
                if (pfs_dir[i].data_size == 0) { free_slot = i; break; }
            }
            if (free_slot >= 0) {
                int alloc_len = 8192;
                alloc_len = (alloc_len + 255) & ~255;
                pfs_dir[free_slot].company_code = company;
                pfs_dir[free_slot].game_code = game_code;
                memcpy(pfs_dir[free_slot].game_name, name, 4);
                memcpy(pfs_dir[free_slot].ext_name, ext, 4);
                pfs_dir[free_slot].data_offset = pfs_next_offset();
                pfs_dir[free_slot].data_size = alloc_len;
                pfs_save_dir();
                slot = free_slot;
                find_miss_count = 0;
                fprintf(stderr, "[PAK] Auto-created save file slot=%d size=%d\n", slot, alloc_len);
            } else {
                ctx->r2 = 5; // PFS_ERR_FULL
                return;
            }
        } else {
            ctx->r2 = 5; // PFS_ERR_INVALID (file not found)
            return;
        }
    }

    if (file_no_ptr) *TO_PTR(s32, file_no_ptr) = slot;
    ctx->r2 = 0;
}

extern "C" void osPfsReadWriteFile_recomp(uint8_t* rdram, recomp_context* ctx) {
    // s32 osPfsReadWriteFile(OSPfs *pfs, s32 file_no, u8 flag, int offset, int nbytes, u8 *data)
    std::lock_guard lock{pfs_mutex};
    pfs_init();

    int file_no = (int)ctx->r5;
    uint8_t flag = (uint8_t)ctx->r6;
    int offset = (int)ctx->r7;
    int nbytes = (int)MEM_W(0x10, ctx->r29);
    PTR(void) data_ptr = MEM_W(0x14, ctx->r29);

    if (file_no < 0 || file_no >= PFS_MAX_FILES || pfs_dir[file_no].data_size == 0) {
        ctx->r2 = 5; // PFS_ERR_INVALID
        return;
    }

    if (offset + nbytes > pfs_dir[file_no].data_size) {
        ctx->r2 = 5;
        return;
    }

    uint32_t buf_offset = PFS_DATA_START + pfs_dir[file_no].data_offset + offset;

    if (flag == PFS_WRITE) {
        // Read from RDRAM, write to save buffer
        uint8_t temp[256];
        int remaining = nbytes;
        int src_off = 0;
        while (remaining > 0) {
            int chunk = remaining > 256 ? 256 : remaining;
            for (int i = 0; i < chunk; i++) {
                temp[i] = MEM_BU(src_off + i, data_ptr);
            }
            save_write_ptr(temp, buf_offset + src_off, chunk);
            src_off += chunk;
            remaining -= chunk;
        }
    } else {
        // Read from save buffer, write to RDRAM
        uint8_t temp[256];
        int remaining = nbytes;
        int dst_off = 0;
        while (remaining > 0) {
            int chunk = remaining > 256 ? 256 : remaining;
            save_read_ptr(temp, buf_offset + dst_off, chunk);
            for (int i = 0; i < chunk; i++) {
                MEM_B(dst_off + i, data_ptr) = temp[i];
            }
            dst_off += chunk;
            remaining -= chunk;
        }
    }

    ctx->r2 = 0;
}

extern "C" void osPfsChecker_recomp(uint8_t* rdram, recomp_context* ctx) {
    ctx->r2 = 0; // pak is always valid
}

extern "C" void osPfsNumFiles_recomp(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard lock{pfs_mutex};
    pfs_init();

    s32* max_files = _arg<1, s32*>(rdram, ctx);
    s32* files_used = _arg<2, s32*>(rdram, ctx);

    *max_files = PFS_MAX_FILES;
    int used = 0;
    for (int i = 0; i < PFS_MAX_FILES; i++) {
        if (pfs_dir[i].data_size != 0) used++;
    }
    *files_used = used;
    _return<s32>(ctx, 0);
}

extern "C" void osPfsRepairId_recomp(uint8_t* rdram, recomp_context* ctx) {
    _return<s32>(ctx, 0);
}
