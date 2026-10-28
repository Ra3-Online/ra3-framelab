// Display-only turret cache epochs. Never used to advance simulation.
// The native last-alpha word is a cache commit marker on the high-FPS path;
// yaw/pitch endpoints are still captured by the original game instructions.
#ifndef FL_HIGH_FPS_TURRET_H
#define FL_HIGH_FPS_TURRET_H
#include "high_fps_schedule.h"

struct FlHighTurretClock {
    unsigned serial = 1, observedAccepted = 0, completions = 0;
    std::uint64_t completionUs = 0;
    bool haveCompletion = false;
    float alpha = 0.0f;

    void observe(const FlHighFpsSchedule& schedule, int phase) {
        if (schedule.accepted != observedAccepted) {
            observedAccepted = schedule.accepted;
            if (!schedule.fault && phase == 6) {
                if (++serial == 0) ++serial; // zero remains the native fresh-cache marker
                ++completions;
                completionUs = schedule.now;
                haveCompletion = true;
            }
        }
        const std::uint64_t elapsed = haveCompletion && schedule.now >= completionUs
            ? schedule.now - completionUs : 0;
        const double value = (double)elapsed / (2 * FlHighFpsSchedule::halfUs);
        alpha = (float)(value < 0.9999 ? value : 0.9999);
    }
};
#endif
