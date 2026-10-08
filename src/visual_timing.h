// Visual-only decisions. No game state, allocation or Windows calls.
#ifndef FL_VISUAL_TIMING_H
#define FL_VISUAL_TIMING_H

#include <cstdint>

#include <cstring>

// Creation params copied by native sub_6C0530, including its final byte/padding.
// Word 9 is lifetime; word 10 is the creation stamp and must remain unchanged.
struct FlGpuParticleParams {
    std::uint32_t words[12];
};
static_assert(sizeof(FlGpuParticleParams) == 48, "native creation parameter layout");

static const std::uint32_t kFlSovietPowerGlowId = 0xBD8CD4C6u;

inline std::uint32_t fl_visual_word(const void* object, unsigned offset) {
    std::uint32_t value;
    std::memcpy(&value, static_cast<const unsigned char*>(object) + offset, sizeof value);
    return value;
}

// These fields were checked against the retail asset and native template ctor.
// Changed timing/type fields (including mods using this ID) retain native life.
inline bool fl_retail_power_glow(const void* tpl, unsigned lifetime) {
    return tpl && lifetime == 15 &&
        fl_visual_word(tpl, 256) == kFlSovietPowerGlowId &&
        static_cast<const unsigned char*>(tpl)[72] == 0 && // not one-shot
        fl_visual_word(tpl, 80) == 5 &&                   // GPU_PARTICLE
        fl_visual_word(tpl, 112) == 0 &&                  // persistent system
        fl_visual_word(tpl, 100) == 1 &&
        fl_visual_word(tpl, 104) == 0x41700000u &&         // lifetime low/high = 15
        fl_visual_word(tpl, 108) == 0x41700000u &&
        fl_visual_word(tpl, 156) == 1 &&
        fl_visual_word(tpl, 160) == 0x41700000u &&         // burst delay low/high = 15
        fl_visual_word(tpl, 164) == 0x41700000u &&
        fl_visual_word(tpl, 168) == 1 &&
        fl_visual_word(tpl, 172) == 0x3F800000u &&         // burst count low/high = 1
        fl_visual_word(tpl, 176) == 0x3F800000u;
}

#endif
