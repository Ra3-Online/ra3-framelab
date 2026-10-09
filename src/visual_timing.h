// Visual-only decisions. No game state, allocation or Windows calls.
#ifndef FL_VISUAL_TIMING_H
#define FL_VISUAL_TIMING_H

#include <cstdint>

#include <cstring>

// The native pool needs an integer lifetime, but each GPU vertex carries a
// float lifetime. At 60/90 Hz an emitter whose countdown is 15 replaces the
// particle on step 16. The last preceding display frame is at age 15.5/15.667;
// replacement is at about 16.0. Keep the pool until replacement, then clip the
// old quad before the new one is drawn. Birth time must not be shifted.
static const unsigned kFlGlowPoolLifetime = 16;
static const float kFlGlowVisibleLifetime = 15.75f;

// Object -> Drawable model conditions are queued by the logic side. Between
// logic boundaries a freshly placed object can already be unpacking while its
// draw module still has the default (complete) model. Prepare only that pending
// transition, in a local copy, with the same remove/add override order as the
// native Drawable. Never edit the object, Drawable or the world's queue here.
static const std::uint32_t kFlStructureUnpacking = 0x1000u;
inline bool fl_pending_construction_flags(const std::uint32_t* pending,
                                         const std::uint32_t* displayed,
                                         const std::uint32_t* remove,
                                         const std::uint32_t* add,
                                         std::uint32_t* result) {
    if (!(pending[0] & kFlStructureUnpacking) ||
        (displayed[0] & kFlStructureUnpacking)) return false;
    for (unsigned i = 0; i < 15; ++i)
        result[i] = (pending[i] & ~remove[i]) | add[i];
    return (result[0] & kFlStructureUnpacking) != 0;
}

// Work in logic-frame units throughout; never subtract a display-frame count
// from a converted logic timestamp. Fraction is read from the same engine
// interpolation used to draw the frame. A held build has no fractional advance.
inline float fl_construction_progress(std::uint32_t now, std::uint32_t start,
                                     std::uint32_t hold, std::uint32_t duration,
                                     float fraction, bool held) {
    if (!(fraction >= 0.0f && fraction <= 1.0f)) fraction = 0.0f;
    const std::int32_t ticks = static_cast<std::int32_t>((held ? hold : now) - start);
    const double elapsed = static_cast<double>(ticks) + (held ? 0.0 : fraction);
    if (elapsed < 0.0) return 0.0f;
    if (!duration) return 1.0f;
    const double value = elapsed / duration;
    return value < 1.0 ? static_cast<float>(value) : 1.0f;
}

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
