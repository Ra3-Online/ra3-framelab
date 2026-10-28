// Behavioral schedule tests: native batch trace is the oracle, not a second
// copy of the candidate's deadline algorithm. No game or remote process runs.
static void test_high_fps_schedule() {
    for (int actual : {30, 60, 90, 120, 144, 165, 175, 180, 240}) {
        FlHighFpsSchedule s;
        int phase = 6;
        unsigned batches = 0, mc = 0, xf = 0, destroy = 0, pulses = 0;
        bool exact = true, bounded = true, tokens = true;
        float previous = -1;
        unsigned distinct = 0;
        for (int frame = 0; frame < actual * 10; ++frame) {
            const std::uint64_t time = (std::uint64_t)frame * 1000000 / actual;
            mc += s.takeBoundary(1); xf += s.takeBoundary(2); destroy += s.takeBoundary(4);
            tokens = tokens && !s.takeBoundary(1) && !s.takeBoundary(2) && !s.takeBoundary(4);
            if (s.begin(time, phase, 240)) {
                int first = phase >= 6 ? 1 : phase + 1;
                exact = exact && first == (batches % 2 ? 4 : 1);
                for (int p = first; p <= (batches % 2 ? 6 : 3); ++p)
                    exact = exact && p == (int)(batches % 2 * 3 + p - first + 1);
                phase = batches % 2 ? 6 : 3;
                ++batches;
            }
            s.end(phase);
            pulses += s.newTick;
            const float f = s.fraction(phase);
            bounded = bounded && f >= 0 && f <= 1 &&
                      (phase == 3 ? f <= .5f : f >= .5f);
            if (f != previous) ++distinct;
            previous = f;
        }
        char label[160];
        std::snprintf(label,sizeof label,"HIGH actual=%d: all native half-batches in order, no simulation acceleration",actual);
        check(exact && !s.fault && batches >= 299 && batches <= 304, label);
        std::snprintf(label,sizeof label,"HIGH actual=%d: independent one-use boundary tokens and tick pulse",actual);
        check(tokens && mc == xf && mc >= batches && mc <= batches+1 &&
              destroy >= batches/2 && destroy <= batches/2+1 && pulses == (batches+1)/2,label);
        std::snprintf(label,sizeof label,"HIGH actual=%d: independent bounded draw fraction, extra frames remain smooth",actual);
        check(bounded && (actual <= 90 || (distinct > (unsigned)(actual*7) && s.held > 0)),label);
    }
    {
        FlHighFpsSchedule s;
        check(s.begin(0,6,120),"HIGH initial native tick attempt");
        s.end(7);
        bool refused = true;
        for (unsigned i=1;i<=50;++i) {
            refused = refused && s.begin(i*8000,7,120);
            s.end(7);
            refused = refused && !s.newTick && s.accepted==0 && s.fraction(7)==1;
        }
        check(refused && s.retries==51,"HIGH network refuses: no accepted tick credit, retry without dropping phases");
        check(s.begin(408000,7,120),"HIGH network releases: retry native phase 1"); s.end(3);
        check(s.accepted==1 && s.newTick && !s.retry,"HIGH network release commits exactly one batch");
        check(!s.begin(409000,3,120),"HIGH long network stall cannot turn into catch-up burst"); s.end(3);
        check(!s.newTick,"HIGH render-only frame clears new-tick pulse");
        check(s.begin(441000,3,120),"HIGH second half after native interval"); s.end(6);
        check(s.accepted==2 && !s.fault,"HIGH native second half follows released first half");
        check(s.begin(1000000000,6,120),"HIGH long pause rebases only wall-clock debt"); s.end(3);
        check(s.accepted==3 && !s.begin(1000000001,3,120),"HIGH pause resumes one batch, no repeated next half");
    }
    {
        FlHighFpsSchedule s;
        s.begin(1000,6,240); s.end(3);
        check(s.begin(0,3,240),"HIGH backward clock rebase without unsigned catch-up overflow"); s.end(6);
        check(s.accepted==2 && !s.fault,"HIGH backward clock preserves phase order");
        s.begin(33000,6,240); s.end(2);
        check(s.fault && !s.begin(66000,2,240),"HIGH unexpected dispatcher result stops further experimental phase advances");
    }
    check(fl_high_fps_target(120) && fl_high_fps_target(165) && fl_high_fps_target(240) &&
          !fl_high_fps_target(90) && !fl_high_fps_target(175) && !fl_high_fps_target(255),
          "HIGH supported target policy: 120..240 in 15-FPS steps");
}
