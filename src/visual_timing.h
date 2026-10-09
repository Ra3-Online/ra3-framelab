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
// Retail GPUDrawModule.GeometryType = 2. The native table C19244[2] is 5:
// CENTERED_QUAD includes its centre vertex, not just the four corners.
static const unsigned kFlGlowVertexCount = 5;

static const std::uint32_t kFlStructureUnpacking = 0x1000u;
static const unsigned kFlMatchUnpacking = 7;
// Retail asset IDs, not installation paths. Only the six live wall pieces are
// eligible; debris, collapse objects and other structures keep native alpha.
inline bool fl_live_wall_template(std::uint32_t type, std::uint32_t instance) {
    if (type != 0x942FFF2Du) return false; // GameObject
    switch (instance) {
    case 0x296799CFu: // AlliedWallPiece
    case 0x09435832u: // AlliedWallSegmentPiece
    case 0xF8C50039u: // JapanWallPiece
    case 0xBF93CE00u: // JapanWallSegmentPiece
    case 0xA82CF003u: // SovietWallPiece
    case 0x0895CAE6u: // SovietWallSegmentPiece
        return true;
    default: return false;
    }
}
// Native sub_90D620 / sub_8F48A0 starts MATCH_UNPACKING at frame 0, with
// previousFrame = currentFrame - 0.00001f. sub_90ECF0 consumes that sentinel.
// Model selection also stamps module+200 with the current time, which can
// suppress the first render's calculation. Only prime this native initial
// state (or a pending native model restart); do not select a different model.
inline bool fl_initial_unpacking_track(float current, float previous) {
    return current == 0.0f && previous == -0.00001f;
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
static const std::uint32_t kFlJapanPowerGlowId = 0x565063F6u;
static const std::uint32_t kFlJapanPowerPlantLightId = 0x56C6A9ADu;

struct FlPowerGlowProfile {
    std::uint32_t id;
    unsigned nativeLifetime, poolLifetime;
    float visibleLifetime;
    unsigned index;
};
static const unsigned kFlPowerGlowProfiles = 3;
static const FlPowerGlowProfile kFlPowerGlows[kFlPowerGlowProfiles] = {
    {kFlSovietPowerGlowId, 15, kFlGlowPoolLifetime, kFlGlowVisibleLifetime, 0},
    {kFlJapanPowerGlowId, 15, kFlGlowPoolLifetime, kFlGlowVisibleLifetime, 1},
    {kFlJapanPowerPlantLightId, 1, 2, 1.75f, 2},
};
inline const FlPowerGlowProfile* fl_power_glow_profile(std::uint32_t id) {
    for (unsigned i = 0; i < kFlPowerGlowProfiles; ++i)
        if (kFlPowerGlows[i].id == id) return &kFlPowerGlows[i];
    return nullptr;
}

inline std::uint32_t fl_visual_word(const void* object, unsigned offset) {
    std::uint32_t value;
    std::memcpy(&value, static_cast<const unsigned char*>(object) + offset, sizeof value);
    return value;
}

// These fields were checked against the retail asset and native template ctor.
// Changed timing/type fields (including mods using this ID) retain native life.
inline bool fl_retail_power_glow(const void* tpl, unsigned lifetime) {
    if (!tpl) return false;
    const FlPowerGlowProfile* profile = fl_power_glow_profile(fl_visual_word(tpl, 256));
    if (!profile || lifetime != profile->nativeLifetime) return false;
    const unsigned lifeBits = profile->nativeLifetime == 1 ? 0x3F800000u : 0x41700000u;
    return
        static_cast<const unsigned char*>(tpl)[72] == 0 && // not one-shot
        fl_visual_word(tpl, 80) == 5 &&                   // GPU_PARTICLE
        fl_visual_word(tpl, 112) == 0 &&                  // persistent system
        fl_visual_word(tpl, 100) == 1 &&
        fl_visual_word(tpl, 104) == lifeBits &&           // exact retail lifetime low/high
        fl_visual_word(tpl, 108) == lifeBits &&
        fl_visual_word(tpl, 156) == 1 &&
        fl_visual_word(tpl, 160) == lifeBits &&           // exact matching retail burst delay
        fl_visual_word(tpl, 164) == lifeBits &&
        fl_visual_word(tpl, 168) == 1 &&
        fl_visual_word(tpl, 172) == 0x3F800000u &&         // burst count low/high = 1
        fl_visual_word(tpl, 176) == 0x3F800000u;
}

#endif
