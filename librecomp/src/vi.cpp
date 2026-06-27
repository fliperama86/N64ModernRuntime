#include <ultramodern/ultramodern.hpp>
#include "recomp.h"
#include "helpers.hpp"

#ifndef LOD_ENABLE_ISSUE23_TRACE
#define LOD_ENABLE_ISSUE23_TRACE 0
#endif

#if LOD_ENABLE_ISSUE23_TRACE
extern "C" uint32_t lod_current_map_overlay_rom();
extern "C" int lod_current_map_overlay_load_count();
#endif

extern "C" void osViSetYScale_recomp(uint8_t* rdram, recomp_context * ctx) {
    osViSetYScale(ctx->f12.fl);
}

extern "C" void osViSetXScale_recomp(uint8_t* rdram, recomp_context * ctx) {
    osViSetXScale(ctx->f12.fl);
}

extern "C" void osCreateViManager_recomp(uint8_t* rdram, recomp_context* ctx) {
    ;
}

extern "C" void osViBlack_recomp(uint8_t* rdram, recomp_context* ctx) {
    static int vb_count = 0;
    vb_count++;
    if (vb_count <= 20 || (uint32_t)ctx->r4 != 0) {
        fprintf(stderr, "[osViBlack] #%d active=%d\n", vb_count, (uint32_t)ctx->r4);
    }
#if LOD_ENABLE_ISSUE23_TRACE
    {
        const uint32_t map_rom = lod_current_map_overlay_rom();
        if (map_rom == 0x007D4420 || map_rom == 0x0082E330) {
            static int issue23_vi_count = 0;
            issue23_vi_count++;
            if (issue23_vi_count <= 24 || (uint32_t)ctx->r4 != 0 ||
                (issue23_vi_count % 120) == 0) {
                fprintf(stderr,
                        "[ISSUE23_VI_BLACK] #%d vi#%d map#%d map=0x%08X active=%u\n",
                        issue23_vi_count, vb_count, lod_current_map_overlay_load_count(),
                        map_rom, (uint32_t)ctx->r4);
            }
        }
    }
#endif
    osViBlack((uint32_t)ctx->r4);
}

extern "C" void osViRepeatLine_recomp(uint8_t* rdram, recomp_context* ctx) {
    osViRepeatLine(_arg<0, u8>(rdram, ctx));
}

extern "C" void osViSetSpecialFeatures_recomp(uint8_t* rdram, recomp_context* ctx) {
    osViSetSpecialFeatures((uint32_t)ctx->r4);
}

extern "C" void osViGetCurrentFramebuffer_recomp(uint8_t* rdram, recomp_context* ctx) {
    ctx->r2 = (gpr)(int32_t)osViGetCurrentFramebuffer();
}

extern "C" void osViGetNextFramebuffer_recomp(uint8_t* rdram, recomp_context* ctx) {
    ctx->r2 = (gpr)(int32_t)osViGetNextFramebuffer();
}

extern "C" void osViSwapBuffer_recomp(uint8_t* rdram, recomp_context* ctx) {
    osViSwapBuffer(rdram, (int32_t)ctx->r4);
}

extern "C" void osViSetMode_recomp(uint8_t* rdram, recomp_context* ctx) {
    fprintf(stderr, "[osViSetMode] mode_ptr=0x%08X\n", (uint32_t)ctx->r4);
    osViSetMode(rdram, (int32_t)ctx->r4);
}

extern uint64_t total_vis;

extern "C" void wait_one_frame(uint8_t* rdram, recomp_context* ctx) {
    uint64_t cur_vis = total_vis;
    while (cur_vis == total_vis) {
        std::this_thread::yield();
    }
}
