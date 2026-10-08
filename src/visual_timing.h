// Visual-only clock arithmetic. No game state, allocation or Windows calls.
#ifndef FL_VISUAL_TIMING_H
#define FL_VISUAL_TIMING_H

#include <cstdint>

// The supported engine runs at 15 logic ticks/s. Convert a logic stamp into
// the same units as the start/hold/duration stamps in StructureUnpackUpdate.
// Unsigned arithmetic preserves the engine's wrapping 32-bit frame counter.
inline unsigned fl_construction_clock(unsigned logicFrame, int ratio) {
    return logicFrame * static_cast<unsigned>(ratio);
}

#endif
