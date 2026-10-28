// Experimental >90 FPS clock, independent of Windows/game memory.
// Native retail pacing is two 33 ms client updates per logic tick. Preserve
// those batches (1,2,3 / 4,5,6); only their wall-clock placement may vary.
// Time never chooses an object, random value, phase order or simulation delta.
#ifndef FL_HIGH_FPS_SCHEDULE_H
#define FL_HIGH_FPS_SCHEDULE_H

#include <cstdint>

inline bool fl_high_fps_target(int fps) {
    return fps >= 120 && fps <= 240 && fps % 15 == 0;
}

struct FlHighFpsSchedule {
    static const std::uint64_t halfUs = 33000;
    std::uint64_t due = 0, plannedDue = 0, tickStart = 0, now = 0, lastNow = 0;
    std::uint64_t frameUs = 0;
    bool initialized = false, attempted = false, retry = false, newTick = false;
    bool haveTick = false, fault = false;
    int nextPhase = 0;
    unsigned boundary = 7; // independent one-use MC / transform / destroy tokens
    unsigned attempts = 0, accepted = 0, held = 0, retries = 0, rebases = 0;

    // Called once per engine update, after its pre-update housekeeping.
    bool begin(std::uint64_t timeUs, int phase, int fps) {
        newTick = false;
        attempted = false;
        now = timeUs;
        if (!initialized) {
            due = lastNow = now;
            initialized = true;
            frameUs = 1000000u / static_cast<unsigned>(fps > 0 ? fps : 120);
        } else {
            // A backward clock cannot turn into an enormous catch-up budget.
            frameUs = now >= lastNow ? now - lastNow : 0;
            if (now < lastNow) { due = now; tickStart = now; ++rebases; }
            lastNow = now;
            if (frameUs > halfUs) frameUs = halfUs;
        }
        if (fault || phase < 0) { fault = true; ++held; return false; }
        if (now < due) { ++held; return false; }
        // At most one native batch per display frame, including after a pause.
        // Drop wall-clock debt, never a simulation phase or a network command.
        plannedDue = due;
        if (now - due > 4 * halfUs || retry) { plannedDue = now; ++rebases; }
        nextPhase = phase >= 6 ? 1 : phase + 1;
        attempted = true;
        ++attempts;
        return true;
    }

    // Observe the native dispatcher. A network refusal keeps its phase >=7;
    // no deadline/tick credit is consumed until native execution succeeds.
    void end(int phase) {
        if (!attempted) return;
        attempted = false;
        if (nextPhase == 1 && phase >= 7) {
            retry = true;
            due = now;
            ++retries;
            return;
        }
        const int expected = nextPhase <= 3 ? 3 : 6;
        if (phase != expected) { fault = true; return; }
        retry = false;
        due = plannedDue + halfUs;
        ++accepted;
        boundary |= phase == 6 ? 7u : 3u;
        if (phase == 3) {
            tickStart = plannedDue;
            haveTick = true;
            newTick = true;
        }
    }

    bool takeBoundary(unsigned bit) {
        const bool allow = (boundary & bit) != 0;
        boundary &= ~bit;
        return allow;
    }

    float fraction(int phase) const {
        if (!haveTick || phase >= 7 || fault) return 1.0f;
        const std::uint64_t elapsed = now >= tickStart ? now - tickStart : 0;
        double value = static_cast<double>(elapsed + frameUs) / (2 * halfUs);
        // Never interpolate across a half whose native batch has not run.
        const double low = phase >= 6 ? 0.5 : 0.0;
        const double high = phase >= 6 ? 1.0 : 0.5;
        if (value < low) value = low;
        if (value > high) value = high;
        return static_cast<float>(value);
    }
};

#endif
