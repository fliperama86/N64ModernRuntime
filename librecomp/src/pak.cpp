#include "ultramodern/ultra64.h"
#include "ultramodern/ultramodern.hpp"

#include "recomp.h"
#include "helpers.hpp"

// Controller Pak (Memory Pak) stubs.
// Returns success for init/checker so the game sees a valid pak,
// but reports no files found so it shows "no save data" dialog.

#define PFS_ERR_NOPACK      1
#define PFS_ERR_ID_FATAL    11
#define PFS_ERR_INCONSISTENT 14

extern "C" void osPfsInitPak_recomp(uint8_t * rdram, recomp_context* ctx) {
    // s32 osPfsInitPak(OSMesgQueue *queue, OSPfs *pfs, int channel)
    // Return 0 = success. The game will proceed to check for save files.
    ctx->r2 = 0;
}

extern "C" void osPfsFreeBlocks_recomp(uint8_t * rdram, recomp_context * ctx) {
    // s32 osPfsFreeBlocks(OSPfs *pfs, s32 *bytes_not_used)
    // Report all blocks free (empty pak). 32KB pak = 128 pages of 256 bytes.
    s32* bytes_not_used = _arg<1, s32*>(rdram, ctx);
    *bytes_not_used = 128 * 256; // 32768 bytes free
    ctx->r2 = 0;
}

extern "C" void osPfsAllocateFile_recomp(uint8_t * rdram, recomp_context * ctx) {
    ctx->r2 = PFS_ERR_NOPACK; // Not implemented yet
}

extern "C" void osPfsDeleteFile_recomp(uint8_t * rdram, recomp_context * ctx) {
    ctx->r2 = PFS_ERR_NOPACK;
}

extern "C" void osPfsFileState_recomp(uint8_t * rdram, recomp_context * ctx) {
    ctx->r2 = PFS_ERR_NOPACK;
}

extern "C" void osPfsFindFile_recomp(uint8_t * rdram, recomp_context * ctx) {
    // s32 osPfsFindFile(OSPfs *pfs, u16 company_code, u32 game_code,
    //                   u8 *game_name, u8 *ext_name, s32 *file_no)
    ctx->r2 = 5; // PFS_ERR_INVALID = file not found
}

extern "C" void osPfsReadWriteFile_recomp(uint8_t * rdram, recomp_context * ctx) {
    ctx->r2 = PFS_ERR_NOPACK;
}

extern "C" void osPfsChecker_recomp(uint8_t * rdram, recomp_context * ctx) {
    // Return success — pak filesystem is "valid" (just empty)
    ctx->r2 = 0;
}

extern "C" void osPfsNumFiles_recomp(uint8_t * rdram, recomp_context * ctx) {
    s32* max_files = _arg<1, s32*>(rdram, ctx);
    s32* files_used = _arg<2, s32*>(rdram, ctx);

    *max_files = 16;  // Standard Controller Pak has 16 file slots
    *files_used = 0;  // No files on the pak

    _return<s32>(ctx, 0); // Success
}

extern "C" void osPfsRepairId_recomp(uint8_t * rdram, recomp_context * ctx) {
    _return<s32>(ctx, 0); // Success
}
