#include "TestFramework.h"

#include "dsp/StepSequencer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <vector>

using namespace splinter;
using namespace splinter::test;

namespace
{
    struct StepStart
    {
        int64_t index;  // absolute index of the segment that started the step
        int stepIndex;
        EffectType effect;
        StepContext context;
    };

    /** Everything the sequencer said about a run, sample by sample. */
    struct Timeline
    {
        int64_t firstIndex = 0;
        std::vector<int> stepAt;          // pattern slot of each sample
        std::vector<EffectType> effectAt; // effect of each sample's step
        std::vector<StepStart> starts;
        bool segmentsValid = true;        // contiguous, in order, covering every block, slots in range

        int step (int64_t index) const { return stepAt[static_cast<size_t> (index - firstIndex)]; }
    };

    using TransportAt = std::function<TransportInfo (int64_t blockStart)>;

    TransportInfo playing (double ppq, double bpm)
    {
        TransportInfo transport;
        transport.isPlaying = true;
        transport.hasPpqPosition = true;
        transport.ppqPosition = ppq;
        transport.bpm = bpm;
        return transport;
    }

    TransportInfo stopped (double bpm)
    {
        TransportInfo transport;
        transport.bpm = bpm;
        return transport;
    }

    SequencerParams makeParams (double stepBeats = 0.25, int numSteps = 16, float chance = 1.0f,
                                EffectType everyStep = EffectType::Stutter)
    {
        SequencerParams params;
        params.stepBeats = stepBeats;
        params.numSteps = numSteps;
        params.chance = chance;
        params.steps.fill (everyStep);
        return params;
    }

    /** Runs one block and appends what it produced to the timeline. */
    void runBlock (StepSequencer& sequencer, const SequencerParams& params, const TransportInfo& transport,
                   int64_t blockStart, int numSamples, Timeline& timeline)
    {
        sequencer.beginBlock (params, transport, blockStart, numSamples);
        const int numSteps = std::clamp (params.numSteps, 1, maxSteps);

        StepSegment segment;
        int covered = 0;

        while (sequencer.nextSegment (segment))
        {
            if (segment.offset != covered || segment.length < 1 || segment.offset + segment.length > numSamples
                || segment.stepIndex < 0 || segment.stepIndex >= numSteps || segment.context.stepLength < 1)
                timeline.segmentsValid = false;

            if (segment.startsStep)
                timeline.starts.push_back ({ blockStart + segment.offset, segment.stepIndex, segment.effect, segment.context });

            for (int i = 0; i < segment.length; ++i)
            {
                timeline.stepAt.push_back (segment.stepIndex);
                timeline.effectAt.push_back (segment.effect);
            }

            covered += std::max (segment.length, 1);

            if (covered > numSamples)
                break;
        }

        if (covered != numSamples)
            timeline.segmentsValid = false;
    }

    Timeline run (StepSequencer& sequencer, const SequencerParams& params, int64_t total, int blockSize,
                  const TransportAt& transportAt, int64_t firstIndex = 0)
    {
        Timeline timeline;
        timeline.firstIndex = firstIndex;

        for (int64_t done = 0; done < total; done += blockSize)
        {
            const auto length = static_cast<int> (std::min<int64_t> (blockSize, total - done));
            runBlock (sequencer, params, transportAt (firstIndex + done), firstIndex + done, length, timeline);
        }

        return timeline;
    }

    /** Host playing at a constant tempo from startPpq at absolute index firstIndex. */
    TransportAt hostFrom (double startPpq, double bpm, double sampleRate, int64_t firstIndex = 0)
    {
        const double samplesPerBeat = sampleRate * 60.0 / bpm;
        return [=] (int64_t blockStart) {
            return playing (startPpq + static_cast<double> (blockStart - firstIndex) / samplesPerBeat, bpm);
        };
    }

    TransportAt hostStopped (double bpm)
    {
        return [=] (int64_t) { return stopped (bpm); };
    }

    /** Exact sample offset at which a position `beats` ahead is reached, before rounding up. */
    long double crossingOffset (long double beats, double sampleRate, double bpm)
    {
        return beats * static_cast<long double> (sampleRate) * 60.0L / static_cast<long double> (bpm);
    }

    /** The first whole sample at or after an exact crossing. A crossing a hair past a whole
        sample may round down onto it (the sequencer allows for rounding in host positions). */
    bool landsOn (int64_t actual, long double exactOffset)
    {
        const auto expected = static_cast<int64_t> (std::ceil (exactOffset));
        const bool barelyPast = exactOffset - std::floor (exactOffset) < 1.0e-3L;
        return actual == expected || (barelyPast && actual == expected - 1);
    }

    int64_t positiveModulo (int64_t value, int64_t divisor)
    {
        return ((value % divisor) + divisor) % divisor;
    }

    bool sameStarts (const Timeline& a, const Timeline& b)
    {
        if (a.starts.size() != b.starts.size())
            return false;

        for (size_t i = 0; i < a.starts.size(); ++i)
        {
            const auto& x = a.starts[i];
            const auto& y = b.starts[i];

            if (x.index != y.index || x.stepIndex != y.stepIndex || x.effect != y.effect
                || x.context.stepStart != y.context.stepStart || x.context.stepLength != y.context.stepLength)
                return false;
        }

        return true;
    }

    std::vector<bool> firedSteps (const Timeline& timeline)
    {
        std::vector<bool> fired;

        for (const auto& start : timeline.starts)
            fired.push_back (start.effect != EffectType::None);

        return fired;
    }
} // namespace

//==============================================================================
TEST_CASE ("sequencer: host sync puts every boundary on the exact sample")
{
    struct Case
    {
        double sampleRate, bpm, stepBeats, startPpq;
    };

    const Case cases[] = {
        { 48000.0, 120.0, 0.25, 0.0 },
        { 48000.0, 120.0, 0.25, 0.37 },        // not aligned at the block start
        { 44100.0, 133.0, 1.0 / 6.0, 0.1 },    // 1/16 triplets, fractional step lengths
        { 44100.0, 97.3, 1.0 / 3.0, 2.71 },
        { 96000.0, 174.0, 0.125, 0.05 },
        { 48000.0, 60.0, 1.0, 0.999 },
        { 44100.0, 999.0, 0.0625, 0.0 },
        { 88200.0, 87.5, 0.5, 13.3 },
    };

    for (const auto& c : cases)
    {
        for (const int blockSize : { 1, 37, 512 })
        {
            StepSequencer sequencer;
            sequencer.prepare (c.sampleRate);

            const auto stepLength = crossingOffset (static_cast<long double> (c.stepBeats), c.sampleRate, c.bpm);
            const auto total = static_cast<int64_t> (stepLength * 24.0L) + 100;
            const int64_t firstIndex = 1000;
            const auto timeline = run (sequencer, makeParams (c.stepBeats), total, blockSize,
                                       hostFrom (c.startPpq, c.bpm, c.sampleRate, firstIndex), firstIndex);

            CHECK (timeline.segmentsValid);
            CHECK (timeline.starts.size() >= 24);

            if (timeline.starts.empty())
                continue;

            // The first block lands mid-step: that step starts at once, dated back to its real start.
            const auto firstStep = static_cast<int64_t> (std::floor (c.startPpq / c.stepBeats + 1.0e-9));
            const auto& first = timeline.starts.front();
            CHECK (first.index == firstIndex);
            CHECK (first.stepIndex == positiveModulo (firstStep, 16));
            CHECK (landsOn (first.context.stepStart - firstIndex,
                            crossingOffset (static_cast<long double> (firstStep) * c.stepBeats - c.startPpq, c.sampleRate, c.bpm)));

            for (size_t n = 1; n < timeline.starts.size(); ++n)
            {
                const auto stepNumber = firstStep + static_cast<int64_t> (n);
                const auto beatsAhead = static_cast<long double> (stepNumber) * c.stepBeats - c.startPpq;
                const auto& start = timeline.starts[n];

                CHECK (landsOn (start.index - firstIndex, crossingOffset (beatsAhead, c.sampleRate, c.bpm)));
                CHECK (start.context.stepStart == start.index);
                CHECK (start.stepIndex == positiveModulo (stepNumber, 16));
                CHECK_NEAR (start.context.samplesPerBeat, c.sampleRate * 60.0 / c.bpm, 1e-9);
                CHECK_NEAR (start.context.sampleRate, c.sampleRate, 0.0);

                if (n + 1 < timeline.starts.size())
                    CHECK (start.context.stepLength == timeline.starts[n + 1].index - start.index);
            }
        }
    }
}

TEST_CASE ("sequencer: a first block in the middle of a step dates the step back to its real start")
{
    StepSequencer sequencer;
    sequencer.prepare (48000.0);

    // ppq 0.37 at 120 bpm: step 1 began 0.12 beats (2880 samples) ago, step 2 begins in 3120.
    sequencer.beginBlock (makeParams(), playing (0.37, 120.0), 5000, 4096);

    StepSegment segment;
    CHECK (sequencer.nextSegment (segment));
    CHECK (segment.offset == 0);
    CHECK (segment.length == 3120);
    CHECK (segment.startsStep);
    CHECK (segment.stepIndex == 1);
    CHECK (segment.effect == EffectType::Stutter);
    CHECK (segment.context.stepStart == 5000 - 2880);
    CHECK (segment.context.stepLength == 6000);
    CHECK_NEAR (segment.context.samplesPerBeat, 24000.0, 1e-9);
    CHECK_NEAR (segment.context.sampleRate, 48000.0, 0.0);

    CHECK (sequencer.nextSegment (segment));
    CHECK (segment.offset == 3120);
    CHECK (segment.length == 4096 - 3120);
    CHECK (segment.startsStep);
    CHECK (segment.stepIndex == 2);
    CHECK (segment.context.stepStart == 5000 + 3120);
    CHECK (segment.context.stepLength == 6000);

    CHECK (! sequencer.nextSegment (segment));
    CHECK (sequencer.currentStep() == 2);
}

TEST_CASE ("sequencer: step index is floor (ppq / stepBeats) mod numSteps")
{
    struct Case
    {
        double ppq, stepBeats;
        int numSteps, expectedIndex;
    };

    const Case cases[] = {
        { 0.0, 0.25, 16, 0 },
        { 3.99, 0.25, 16, 15 },
        { 4.0, 0.25, 16, 0 },
        { 7.9, 0.5, 5, 0 },          // floor (15.8) = 15
        { 7.9, 0.5, 7, 1 },
        { 10.2, 1.0 / 3.0, 16, 14 }, // floor (30.6) = 30
        { 100.0, 0.125, 1, 0 },
        { 2.0, 1.0, 3, 2 },
    };

    for (const auto& c : cases)
    {
        StepSequencer sequencer;
        sequencer.prepare (44100.0);
        sequencer.beginBlock (makeParams (c.stepBeats, c.numSteps), playing (c.ppq, 128.0), 0, 64);

        StepSegment segment;
        CHECK (sequencer.nextSegment (segment));
        CHECK (segment.startsStep);
        CHECK (segment.stepIndex == c.expectedIndex);
        CHECK (sequencer.currentStep() == c.expectedIndex);
    }
}

TEST_CASE ("sequencer: negative ppq in pre-roll counts up into step 0 at ppq 0")
{
    StepSequencer sequencer;
    sequencer.prepare (48000.0);

    // From one beat before the downbeat: steps 12..15, then 0 exactly where ppq reaches 0.
    const auto timeline = run (sequencer, makeParams(), 48000, 500, hostFrom (-1.0, 120.0, 48000.0));

    CHECK (timeline.segmentsValid);
    CHECK (timeline.starts.size() == 8);

    const int expectedSteps[] = { 12, 13, 14, 15, 0, 1, 2, 3 };

    for (size_t n = 0; n < std::min<size_t> (timeline.starts.size(), 8); ++n)
    {
        CHECK (timeline.starts[n].index == static_cast<int64_t> (n) * 6000);
        CHECK (timeline.starts[n].stepIndex == expectedSteps[n]);
    }

    // Part-way into a negative step: ppq -0.1 is in step -1 (slot 15), which began at ppq -0.25.
    StepSequencer midStep;
    midStep.prepare (48000.0);
    midStep.beginBlock (makeParams(), playing (-0.1, 120.0), 0, 6000);

    StepSegment segment;
    CHECK (midStep.nextSegment (segment));
    CHECK (segment.stepIndex == 15);
    CHECK (segment.context.stepStart == -3600);
    CHECK (segment.length == 2400);
    CHECK (midStep.nextSegment (segment));
    CHECK (segment.stepIndex == 0);
    CHECK (segment.offset == 2400);
}

TEST_CASE ("sequencer: free-runs from step 0 after reset while the host is stopped")
{
    for (const int blockSize : { 1, 64, 1000 })
    {
        StepSequencer sequencer;
        sequencer.prepare (48000.0);

        // Stopped hosts often still report a position; it must be ignored.
        const auto timeline = run (sequencer, makeParams(), 6000 * 20, blockSize, [] (int64_t) {
            auto transport = stopped (120.0);
            transport.hasPpqPosition = true;
            transport.ppqPosition = 37.3;
            return transport;
        });

        CHECK (timeline.segmentsValid);
        CHECK (timeline.starts.size() == 20);

        for (size_t n = 0; n < timeline.starts.size(); ++n)
        {
            CHECK (timeline.starts[n].index == static_cast<int64_t> (n) * 6000);
            CHECK (timeline.starts[n].context.stepStart == static_cast<int64_t> (n) * 6000);
            CHECK (timeline.starts[n].stepIndex == static_cast<int> (n % 16));
            CHECK (timeline.starts[n].context.stepLength == 6000);
        }
    }

    // Playing but without a position (some standalone hosts) also free-runs, at fractional step lengths.
    StepSequencer sequencer;
    sequencer.prepare (44100.0);

    auto noPosition = stopped (133.0);
    noPosition.isPlaying = true;

    const auto timeline = run (sequencer, makeParams (1.0 / 6.0), 200000, 300, [&] (int64_t) { return noPosition; });
    CHECK (timeline.segmentsValid);

    for (size_t n = 0; n < timeline.starts.size(); ++n)
        CHECK (landsOn (timeline.starts[n].index, crossingOffset (static_cast<long double> (n) / 6.0L, 44100.0, 133.0)));
}

TEST_CASE ("sequencer: free-run sanitises the host tempo")
{
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();

    struct Case
    {
        double bpm, expectedBpm;
    };

    const Case cases[] = { { nan, 120.0 }, { inf, 120.0 }, { -inf, 120.0 }, { 0.0, 120.0 }, { -50.0, 120.0 },
                           { 5.0, 20.0 }, { 5000.0, 999.0 }, { 140.0, 140.0 } };

    for (const auto& c : cases)
    {
        CHECK_NEAR (StepSequencer::sanitiseBpm (c.bpm), c.expectedBpm, 0.0);

        StepSequencer sequencer;
        sequencer.prepare (48000.0);
        const auto timeline = run (sequencer, makeParams(), 300000, 512, hostStopped (c.bpm));

        CHECK (timeline.segmentsValid);
        CHECK (timeline.starts.size() >= 2);

        if (timeline.starts.size() >= 2)
        {
            CHECK_NEAR (timeline.starts[0].context.samplesPerBeat, 48000.0 * 60.0 / c.expectedBpm, 1e-9);
            CHECK (landsOn (timeline.starts[1].index, crossingOffset (0.25L, 48000.0, c.expectedBpm)));
        }
    }

    // NaN tempo while the host plays: positions still come from the PPQ, steps at 120 bpm.
    StepSequencer sequencer;
    sequencer.prepare (48000.0);
    const auto timeline = run (sequencer, makeParams(), 30000, 256, [] (int64_t blockStart) {
        return playing (static_cast<double> (blockStart) / 24000.0, std::numeric_limits<double>::quiet_NaN());
    });

    CHECK (timeline.segmentsValid);
    CHECK (timeline.starts.size() == 5);

    for (size_t n = 0; n < timeline.starts.size(); ++n)
        CHECK (timeline.starts[n].index == static_cast<int64_t> (n) * 6000);
}

TEST_CASE ("sequencer: a free-run tempo change keeps the position continuous")
{
    StepSequencer sequencer;
    sequencer.prepare (48000.0);
    const auto params = makeParams();

    // 9000 samples at 120 bpm reach beat 0.375, mid step 1. At 60 bpm beat 0.5 is 6000 samples further.
    Timeline timeline;
    runBlock (sequencer, params, stopped (120.0), 0, 9000, timeline);
    runBlock (sequencer, params, stopped (60.0), 9000, 12000, timeline);

    CHECK (timeline.segmentsValid);
    CHECK (timeline.starts.size() == 3);

    if (timeline.starts.size() == 3)
    {
        CHECK (timeline.starts[1].index == 6000);
        CHECK (timeline.starts[2].index == 15000);
        CHECK (timeline.starts[2].stepIndex == 2);
        CHECK (timeline.starts[2].context.stepLength == 12000);
    }
}

TEST_CASE ("sequencer: a loop jump re-syncs at once and flags a new step")
{
    StepSequencer sequencer;
    sequencer.prepare (48000.0);
    const auto params = makeParams();

    Timeline timeline;
    int64_t index = 0;

    // Four beats from ppq 0, then the host loops back to ppq 1.1.
    for (; index < 96000; index += 480)
        runBlock (sequencer, params, playing (static_cast<double> (index) / 24000.0, 120.0), index, 480, timeline);

    const auto startsBeforeJump = timeline.starts.size();
    CHECK (startsBeforeJump == 16);

    runBlock (sequencer, params, playing (1.1, 120.0), index, 4096, timeline);

    CHECK (timeline.segmentsValid);
    CHECK (timeline.starts.size() == startsBeforeJump + 2);

    if (timeline.starts.size() == startsBeforeJump + 2)
    {
        const auto& resync = timeline.starts[startsBeforeJump];
        CHECK (resync.index == index);
        CHECK (resync.stepIndex == 4);
        CHECK (resync.context.stepStart == index - 2400);
        CHECK (resync.context.stepLength == 6000);

        const auto& next = timeline.starts[startsBeforeJump + 1];
        CHECK (next.index == index + 3600);
        CHECK (next.stepIndex == 5);
    }

    // A seek that stays inside the same step still retriggers it.
    const double expectedPpq = 1.1 + 4096.0 / 24000.0; // 1.2707, inside step 5
    index += 4096;
    const auto startsBeforeSeek = timeline.starts.size();
    runBlock (sequencer, params, playing (expectedPpq + 0.02, 120.0), index, 256, timeline);

    CHECK (timeline.starts.size() == startsBeforeSeek + 1);

    if (timeline.starts.size() == startsBeforeSeek + 1)
    {
        CHECK (timeline.starts.back().index == index);
        CHECK (timeline.starts.back().stepIndex == 5);
        CHECK (timeline.starts.back().context.stepStart == index - static_cast<int64_t> (std::lround ((expectedPpq + 0.02 - 1.25) * 24000.0)));
    }
}

TEST_CASE ("sequencer: host jitter below the jump tolerance does not retrigger")
{
    StepSequencer sequencer;
    sequencer.prepare (48000.0);
    const auto params = makeParams();

    // Positions wobble by up to ~2 ms against an ideal timeline that starts half a step in,
    // so the wobble never reaches across the first or the last boundary.
    Timeline timeline;
    uint32_t noise = 12345;

    for (int64_t index = 0; index < 240000; index += 480)
    {
        noise ^= noise << 13;
        noise ^= noise >> 17;
        noise ^= noise << 5;
        const double wobbleSamples = (static_cast<double> (noise) / 4294967295.0 * 2.0 - 1.0) * 90.0;
        runBlock (sequencer, params, playing ((static_cast<double> (index) + 3000.0 + wobbleSamples) / 24000.0, 120.0),
                  index, 480, timeline);
    }

    CHECK (timeline.segmentsValid);
    CHECK (timeline.starts.size() == 41);

    // Every step exactly once, in order, each within the wobble of its ideal position.
    for (size_t n = 0; n < timeline.starts.size(); ++n)
    {
        CHECK (timeline.starts[n].stepIndex == static_cast<int> (n % 16));

        if (n > 0)
            CHECK (std::abs (timeline.starts[n].index - (static_cast<int64_t> (n) * 6000 - 3000)) <= 91);
    }
}

TEST_CASE ("sequencer: small drift across a boundary neither repeats nor skips a step")
{
    const auto params = makeParams();

    {
        // Block A crosses into step 1 on its last sample; block B's host position is 20 samples
        // earlier, back in step 0. Step 1 carries on, without a second trigger.
        StepSequencer sequencer;
        sequencer.prepare (48000.0);
        Timeline timeline;
        runBlock (sequencer, params, playing (0.0, 120.0), 0, 6001, timeline);
        runBlock (sequencer, params, playing (5981.0 / 24000.0, 120.0), 6001, 6000, timeline);

        CHECK (timeline.segmentsValid);
        CHECK (timeline.starts.size() == 2);
        CHECK (timeline.starts.back().index == 6000);
        CHECK (timeline.step (6001) == 1);
        CHECK (timeline.step (12000) == 1);
    }

    {
        // Block A ends 10 samples before the boundary; block B's host position is 30 samples
        // later, already in step 1. Step 1 starts at B's first sample, dated back 20 samples.
        StepSequencer sequencer;
        sequencer.prepare (48000.0);
        Timeline timeline;
        runBlock (sequencer, params, playing (0.0, 120.0), 0, 5990, timeline);
        runBlock (sequencer, params, playing (6020.0 / 24000.0, 120.0), 5990, 4000, timeline);

        CHECK (timeline.segmentsValid);
        CHECK (timeline.starts.size() == 2);

        if (timeline.starts.size() == 2)
        {
            CHECK (timeline.starts[1].index == 5990);
            CHECK (timeline.starts[1].stepIndex == 1);
            CHECK (timeline.starts[1].context.stepStart == 5990 - 20);
        }
    }
}

TEST_CASE ("sequencer: starting the host switches to its position immediately")
{
    StepSequencer sequencer;
    sequencer.prepare (48000.0);
    const auto params = makeParams();

    Timeline timeline;
    runBlock (sequencer, params, stopped (120.0), 0, 10000, timeline); // free-run: steps 0 and 1
    runBlock (sequencer, params, playing (2.6, 120.0), 10000, 6000, timeline);

    CHECK (timeline.segmentsValid);
    CHECK (timeline.starts.size() == 4);

    if (timeline.starts.size() == 4)
    {
        CHECK (timeline.starts[1].index == 6000);
        CHECK (timeline.starts[2].index == 10000);
        CHECK (timeline.starts[2].stepIndex == 10);
        CHECK (timeline.starts[2].context.stepStart == 10000 - 2400);
        CHECK (timeline.starts[3].index == 10000 + 3600);
        CHECK (timeline.starts[3].stepIndex == 11);
    }
}

TEST_CASE ("sequencer: stopping the host keeps the clock running without a retrigger")
{
    StepSequencer sequencer;
    sequencer.prepare (48000.0);
    const auto params = makeParams();

    Timeline timeline;
    runBlock (sequencer, params, playing (2.0, 120.0), 0, 9000, timeline);   // steps 8 and 9
    runBlock (sequencer, params, stopped (120.0), 9000, 15000, timeline);   // carries on: 10 at 12000, 11 at 18000

    CHECK (timeline.segmentsValid);
    CHECK (timeline.starts.size() == 4);

    if (timeline.starts.size() == 4)
    {
        CHECK (timeline.starts[1].index == 6000);
        CHECK (timeline.starts[2].index == 12000);
        CHECK (timeline.starts[2].stepIndex == 10);
        CHECK (timeline.starts[3].index == 18000);
        CHECK (timeline.starts[3].stepIndex == 11);
    }

    // Playing again re-syncs at the host's position.
    runBlock (sequencer, params, playing (0.0, 120.0), 24000, 100, timeline);
    CHECK (timeline.starts.size() == 5);
    CHECK (timeline.starts.back().index == 24000);
    CHECK (timeline.starts.back().stepIndex == 0);
}

TEST_CASE ("sequencer: chance 1 fires every non-empty step, chance 0 none, empty steps never")
{
    for (const float chance : { 0.0f, 1.0f, 0.5f })
    {
        auto params = makeParams (0.25, 16, chance);

        for (size_t slot = 0; slot < params.steps.size(); ++slot)
            params.steps[slot] = slot % 3 == 0 ? EffectType::None : static_cast<EffectType> (1 + slot % 6);

        StepSequencer sequencer;
        sequencer.prepare (48000.0);
        const auto timeline = run (sequencer, params, 6000 * 64, 512, hostFrom (0.0, 120.0, 48000.0));

        CHECK (timeline.starts.size() == 64);

        for (const auto& start : timeline.starts)
        {
            const auto slotEffect = params.steps[static_cast<size_t> (start.stepIndex)];

            if (slotEffect == EffectType::None || chance == 0.0f)
                CHECK (start.effect == EffectType::None);
            else if (chance == 1.0f)
                CHECK (start.effect == slotEffect);
            else
                CHECK (start.effect == slotEffect || start.effect == EffectType::None);
        }
    }
}

TEST_CASE ("sequencer: chance fires about that fraction of steps, reproducibly")
{
    // 999 bpm 1/64 notes: about 180 samples a step, so thousands of steps run quickly.
    const double sampleRate = 48000.0;
    const double bpm = 999.0;
    const int64_t total = 180 * 5000;

    for (const float chance : { 0.25f, 0.5f, 0.8f })
    {
        const auto params = makeParams (0.0625, 16, chance);

        StepSequencer sequencer;
        sequencer.prepare (sampleRate);
        const auto first = run (sequencer, params, total, 4096, hostStopped (bpm));
        const auto fired = firedSteps (first);

        CHECK (fired.size() > 4000);

        const auto hits = static_cast<double> (std::count (fired.begin(), fired.end(), true));
        CHECK_NEAR (hits / static_cast<double> (fired.size()), chance, 0.04);

        // No runs much longer than chance alone would give (about log n / log (1 / p) for the likelier outcome).
        const double likelier = std::max (static_cast<double> (chance), 1.0 - chance);
        const double typicalLongestRun = std::log (static_cast<double> (fired.size())) / -std::log (likelier);
        int longestRun = 0;

        for (size_t n = 0, runLength = 0; n < fired.size(); ++n)
        {
            runLength = n > 0 && fired[n] == fired[n - 1] ? runLength + 1 : 1;
            longestRun = std::max (longestRun, static_cast<int> (runLength));
        }

        CHECK (longestRun < 2.0 * typicalLongestRun);

        // The same after reset(), and whatever the block size.
        sequencer.reset();
        CHECK (firedSteps (run (sequencer, params, total, 4096, hostStopped (bpm))) == fired);

        StepSequencer other;
        other.prepare (sampleRate);
        CHECK (firedSteps (run (other, params, total, 77, hostStopped (bpm))) == fired);
    }
}

TEST_CASE ("sequencer: raising chance only adds hits")
{
    std::vector<std::vector<bool>> patterns;

    for (const float chance : { 0.2f, 0.5f, 0.9f })
    {
        StepSequencer sequencer;
        sequencer.prepare (48000.0);
        patterns.push_back (firedSteps (run (sequencer, makeParams (0.25, 16, chance), 6000 * 500, 1024, hostStopped (120.0))));
    }

    for (size_t level = 1; level < patterns.size(); ++level)
    {
        CHECK (patterns[level].size() == patterns[0].size());

        for (size_t n = 0; n < std::min (patterns[level].size(), patterns[level - 1].size()); ++n)
            if (patterns[level - 1][n])
                CHECK (patterns[level][n]);
    }
}

TEST_CASE ("sequencer: changing chance mid-play does not reshuffle the later steps")
{
    // Every non-empty step consumes one roll whatever the chance, so after a change the steps
    // fire exactly as if that chance had been set all along.
    const int64_t switchAt = 6000 * 100;
    const int64_t total = 6000 * 300;

    for (const float before : { 1.0f, 0.0f, 0.3f })
    {
        StepSequencer changed;
        changed.prepare (48000.0);
        Timeline timeline;

        for (int64_t index = 0; index < total; index += 1000)
        {
            const auto params = makeParams (0.25, 16, index < switchAt ? before : 0.6f);
            runBlock (changed, params, stopped (120.0), index, 1000, timeline);
        }

        StepSequencer constant;
        constant.prepare (48000.0);
        const auto reference = run (constant, makeParams (0.25, 16, 0.6f), total, 1000, hostStopped (120.0));

        const auto firedChanged = firedSteps (timeline);
        const auto firedReference = firedSteps (reference);
        CHECK (firedChanged.size() == firedReference.size());
        CHECK (std::equal (firedChanged.begin() + 100, firedChanged.end(), firedReference.begin() + 100));
    }
}

TEST_CASE ("sequencer: changing the step count mid-play keeps the index in range")
{
    StepSequencer sequencer;
    sequencer.prepare (48000.0);
    auto params = makeParams();

    // ppq 3.3 is step 13. Shrinking to 8 steps moves it to slot 5: that slot starts now,
    // still dated to step 13's real start.
    Timeline timeline;
    runBlock (sequencer, params, playing (3.3, 120.0), 0, 480, timeline);
    CHECK (timeline.starts.size() == 1);
    CHECK (sequencer.currentStep() == 13);
    const auto stepStart = timeline.starts.back().context.stepStart;
    CHECK (stepStart == -1200);

    params.numSteps = 8;
    runBlock (sequencer, params, playing (3.3 + 480.0 / 24000.0, 120.0), 480, 480, timeline);
    CHECK (timeline.starts.size() == 2);
    CHECK (timeline.starts.back().index == 480);
    CHECK (timeline.starts.back().stepIndex == 5);
    CHECK (timeline.starts.back().context.stepStart == stepStart);

    // 14 steps puts step 13 back in slot 13: another slot change, so another re-sync.
    params.numSteps = 14;
    runBlock (sequencer, params, playing (3.3 + 960.0 / 24000.0, 120.0), 960, 480, timeline);
    CHECK (timeline.starts.size() == 3);
    CHECK (sequencer.currentStep() == 13);

    // Growing to 16 keeps it in slot 13: no retrigger.
    params.numSteps = 16;
    runBlock (sequencer, params, playing (3.3 + 1440.0 / 24000.0, 120.0), 1440, 480, timeline);
    CHECK (timeline.starts.size() == 3);
    CHECK (sequencer.currentStep() == 13);

    // Then hammer it: a different step count every block, from 1 to 16.
    uint32_t noise = 99;
    int64_t index = 1920;

    for (int block = 0; block < 2000; ++block)
    {
        noise ^= noise << 13;
        noise ^= noise >> 17;
        noise ^= noise << 5;
        params.numSteps = 1 + static_cast<int> (noise % 16);

        Timeline blockTimeline;
        runBlock (sequencer, params, playing (3.3 + static_cast<double> (index) / 24000.0, 120.0), index, 97, blockTimeline);
        CHECK (blockTimeline.segmentsValid);
        CHECK (sequencer.currentStep() >= 0 && sequencer.currentStep() < params.numSteps);
        index += 97;
    }
}

TEST_CASE ("sequencer: changing the step length mid-play re-syncs to the new grid")
{
    StepSequencer sequencer;
    sequencer.prepare (48000.0);
    auto params = makeParams (0.25);

    Timeline timeline;
    runBlock (sequencer, params, playing (0.0, 120.0), 0, 7200, timeline); // to ppq 0.3, in step 1

    params.stepBeats = 0.125; // ppq 0.3 is step 2 of the 1/32 grid, which began at ppq 0.25
    runBlock (sequencer, params, playing (0.3, 120.0), 7200, 6000, timeline);

    CHECK (timeline.segmentsValid);
    CHECK (timeline.starts.size() == 5);

    if (timeline.starts.size() == 5)
    {
        CHECK (timeline.starts[2].index == 7200);
        CHECK (timeline.starts[2].stepIndex == 2);
        CHECK (timeline.starts[2].context.stepStart == 6000);
        CHECK (timeline.starts[2].context.stepLength == 3000);
        CHECK (timeline.starts[3].index == 9000);
        CHECK (timeline.starts[4].index == 12000);
    }
}

TEST_CASE ("sequencer: invalid parameters and transport values are sanitised")
{
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    const float floatNan = std::numeric_limits<float>::quiet_NaN();

    const int stepCounts[] = { 0, -5, 1, 16, 99 };
    const double stepLengths[] = { nan, inf, -inf, 0.0, -1.0, 1.0e-9, 100.0, 0.25 };
    const float chances[] = { floatNan, -1.0f, 5.0f, 0.5f };
    const double positions[] = { nan, inf, 1.0e300, -1.0e300, -3.7, 1.0e7 };

    for (const int numSteps : stepCounts)
    {
        for (const double stepBeats : stepLengths)
        {
            for (const float chance : chances)
            {
                auto params = makeParams (stepBeats, numSteps, chance);
                params.steps[3] = static_cast<EffectType> (42);
                params.steps[5] = static_cast<EffectType> (-1);

                for (const double ppq : positions)
                {
                    StepSequencer sequencer;
                    sequencer.prepare (44100.0);

                    Timeline timeline;
                    runBlock (sequencer, params, playing (ppq, nan), 0, 3000, timeline);
                    runBlock (sequencer, params, stopped (-1.0), 3000, 3000, timeline);

                    CHECK (timeline.segmentsValid);
                    CHECK (! timeline.starts.empty());

                    for (const auto& start : timeline.starts)
                    {
                        const auto effect = static_cast<int> (start.effect);
                        CHECK (effect >= 0 && effect < numEffectTypes);
                        CHECK (start.context.stepLength >= 1);
                    }
                }
            }
        }
    }

    // Out-of-range step lengths clamp to a quarter note .. 1/64 note; garbage falls back to a 1/16.
    const struct
    {
        double stepBeats;
        int64_t expectedLength;
    } lengths[] = { { 100.0, 24000 }, { 1.0e-9, 1500 }, { nan, 6000 }, { -2.0, 6000 } };

    for (const auto& c : lengths)
    {
        StepSequencer sequencer;
        sequencer.prepare (48000.0);
        sequencer.beginBlock (makeParams (c.stepBeats), stopped (120.0), 0, 16);

        StepSegment segment;
        CHECK (sequencer.nextSegment (segment));
        CHECK (segment.context.stepLength == c.expectedLength);
    }

    // A slot holding an unknown effect plays nothing.
    StepSequencer sequencer;
    sequencer.prepare (48000.0);
    auto params = makeParams();
    params.steps[0] = static_cast<EffectType> (42);
    sequencer.beginBlock (params, stopped (120.0), 0, 16);

    StepSegment segment;
    CHECK (sequencer.nextSegment (segment));
    CHECK (segment.effect == EffectType::None);
}

TEST_CASE ("sequencer: segments do not depend on the block size")
{
    auto params = makeParams (0.25, 13, 0.5f);

    for (size_t slot = 0; slot < params.steps.size(); ++slot)
        params.steps[slot] = static_cast<EffectType> (slot % numEffectTypes);

    const int64_t total = 200000;

    const TransportAt transports[] = { hostFrom (0.43, 133.0, 44100.0), hostFrom (-0.77, 97.0, 44100.0), hostStopped (133.0) };

    for (const auto& transportAt : transports)
    {
        StepSequencer reference;
        reference.prepare (44100.0);
        const auto whole = run (reference, params, total, static_cast<int> (total), transportAt);
        CHECK (whole.segmentsValid);
        CHECK (whole.starts.size() > 20);

        for (const int blockSize : { 1, 7, 64, 512 })
        {
            StepSequencer sequencer;
            sequencer.prepare (44100.0);
            const auto split = run (sequencer, params, total, blockSize, transportAt);

            CHECK (split.segmentsValid);
            CHECK (split.stepAt == whole.stepAt);
            CHECK (split.effectAt == whole.effectAt);
            CHECK (sameStarts (split, whole));
        }
    }
}

TEST_CASE ("sequencer: a block with many short steps is split at each one")
{
    StepSequencer sequencer;
    sequencer.prepare (44100.0);

    // 999 bpm 1/64 notes: 165.6 samples a step, about 25 steps in one block.
    Timeline timeline;
    runBlock (sequencer, makeParams (0.0625), playing (0.0, 999.0), 0, 4096, timeline);

    CHECK (timeline.segmentsValid);
    CHECK (timeline.starts.size() == 25);

    for (size_t n = 0; n < timeline.starts.size(); ++n)
    {
        CHECK (landsOn (timeline.starts[n].index, crossingOffset (static_cast<long double> (n) * 0.0625L, 44100.0, 999.0)));
        CHECK (timeline.starts[n].stepIndex == static_cast<int> (n % 16));
    }
}

TEST_CASE ("sequencer: very large host positions still land boundaries exactly")
{
    StepSequencer sequencer;
    sequencer.prepare (48000.0);

    const double startPpq = 1.0e6 + 0.1;
    const auto timeline = run (sequencer, makeParams(), 6000 * 10, 333, hostFrom (startPpq, 120.0, 48000.0));

    CHECK (timeline.segmentsValid);
    CHECK (timeline.starts.size() == 11);

    for (size_t n = 1; n < timeline.starts.size(); ++n)
    {
        CHECK (timeline.starts[n].index == 3600 + static_cast<int64_t> (n - 1) * 6000);
        CHECK (timeline.starts[n].stepIndex == static_cast<int> ((4000000 + n) % 16));
    }
}

TEST_CASE ("sequencer: tempo automation follows the host without retriggering")
{
    StepSequencer sequencer;
    sequencer.prepare (48000.0);
    const auto params = makeParams();

    // The host integrates its own tempo ramp, 120 -> 180 bpm, and reports the result each block.
    Timeline timeline;
    double ppq = 0.0;
    std::vector<double> blockPpq;
    std::vector<double> blockBpm;
    const int blockSize = 256;

    for (int block = 0; block < 1500; ++block)
    {
        const double bpm = 120.0 + 60.0 * std::min (1.0, block / 1000.0);
        blockPpq.push_back (ppq);
        blockBpm.push_back (bpm);
        runBlock (sequencer, params, playing (ppq, bpm), static_cast<int64_t> (block) * blockSize, blockSize, timeline);
        ppq += blockSize * bpm / (60.0 * 48000.0);
    }

    CHECK (timeline.segmentsValid);

    // Each step exactly once, in order, starting where that block's ppq + bpm puts the crossing.
    const auto expectedSteps = static_cast<size_t> (std::floor (ppq / 0.25)) + 1;
    CHECK (timeline.starts.size() == expectedSteps);

    for (size_t n = 0; n < timeline.starts.size(); ++n)
    {
        const auto& start = timeline.starts[n];
        CHECK (start.stepIndex == static_cast<int> (n % 16));

        const auto block = static_cast<size_t> (start.index / blockSize);
        const auto offset = crossingOffset (static_cast<long double> (n) * 0.25L - blockPpq[block], 48000.0, blockBpm[block]);
        CHECK (landsOn (start.index - static_cast<int64_t> (block) * blockSize, offset));
    }
}

TEST_CASE ("sequencer: zero-length blocks and reset")
{
    StepSequencer sequencer;
    sequencer.prepare (48000.0);
    CHECK (sequencer.currentStep() == -1);
    CHECK (sequencer.currentEffect() == EffectType::None);

    StepSegment segment;
    sequencer.beginBlock (makeParams(), playing (1.0, 120.0), 0, 0);
    CHECK (! sequencer.nextSegment (segment));
    CHECK (sequencer.currentStep() == -1);

    sequencer.beginBlock (makeParams(), playing (1.0, 120.0), 0, 10);
    CHECK (sequencer.nextSegment (segment));
    CHECK (segment.startsStep);
    CHECK (segment.length == 10);
    CHECK (sequencer.currentStep() == 4);
    CHECK (sequencer.currentEffect() == EffectType::Stutter);
    CHECK (! sequencer.nextSegment (segment));

    // A zero-length block in the middle changes nothing.
    sequencer.beginBlock (makeParams(), playing (1.0 + 10.0 / 24000.0, 120.0), 10, 0);
    CHECK (! sequencer.nextSegment (segment));
    sequencer.beginBlock (makeParams(), playing (1.0 + 10.0 / 24000.0, 120.0), 10, 10);
    CHECK (sequencer.nextSegment (segment));
    CHECK (! segment.startsStep);

    sequencer.reset();
    CHECK (sequencer.currentStep() == -1);
    sequencer.beginBlock (makeParams(), stopped (120.0), 0, 10);
    CHECK (sequencer.nextSegment (segment));
    CHECK (segment.startsStep);
    CHECK (segment.stepIndex == 0);
    CHECK (segment.context.stepStart == 0);
}
