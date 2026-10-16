#include "TestFramework.h"
#include "TestSignals.h"

#include "dsp/CaptureBuffer.h"
#include "dsp/effects/Gate.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

using namespace splinter;
using namespace splinter::test;

namespace
{
    constexpr double sampleRate = 48000.0;
    constexpr double samplesPerBeat = 24000.0; // 120 bpm
    constexpr int64_t sixteenth = 6000;
    constexpr int captureCapacity = 12 * 48000;
    constexpr int ramp = 48;                   // 1 ms at 48 kHz
    constexpr double pi = 3.14159265358979323846;

    /** Largest per-sample gain change of a raised-cosine ramp `ramp` samples long. */
    constexpr double maxRampSlope = pi / (2.0 * ramp) + 1.0e-6;

    struct Trigger
    {
        int64_t index; // fires just before the sample at this absolute index is rendered
        StepContext step;
    };

    struct Range
    {
        int64_t begin, end;
    };

    struct ParamChange
    {
        int64_t index;
        GateParams params;
    };

    struct Script
    {
        std::vector<Trigger> triggers;
        std::vector<Range> active; // the effect only runs inside these ranges; empty = always
        std::vector<ParamChange> changes;
    };

    StepContext makeStep (int64_t start, int64_t length, double beatLength = samplesPerBeat)
    {
        StepContext step;
        step.stepStart = start;
        step.stepLength = length;
        step.sampleRate = sampleRate;
        step.samplesPerBeat = beatLength;
        return step;
    }

    Trigger stepAt (int64_t start, int64_t length = sixteenth)
    {
        return { start, makeStep (start, length) };
    }

    Script oneStep (int64_t length)
    {
        return { { stepAt (0, length) }, {}, {} };
    }

    /** Back-to-back steps of `length` samples covering [0, total). */
    Script everyStep (int64_t total, int64_t length = sixteenth)
    {
        Script script;

        for (int64_t start = 0; start < total; start += length)
            script.triggers.push_back (stepAt (start, length));

        return script;
    }

    GateParams makeParams (float divisionBeats, float duty)
    {
        GateParams params;
        params.divisionBeats = divisionBeats;
        params.duty = duty;
        return params;
    }

    bool isActive (const std::vector<Range>& active, int64_t index)
    {
        if (active.empty())
            return true;

        for (const auto& range : active)
            if (index >= range.begin && index < range.end)
                return true;

        return false;
    }

    int64_t nextEvent (const Script& script, int64_t index, int64_t limit)
    {
        int64_t next = limit;

        for (const auto& trigger : script.triggers)
            if (trigger.index > index)
                next = std::min (next, trigger.index);

        for (const auto& range : script.active)
        {
            if (range.begin > index)
                next = std::min (next, range.begin);

            if (range.end > index)
                next = std::min (next, range.end);
        }

        for (const auto& change : script.changes)
            if (change.index > index)
                next = std::min (next, change.index);

        return next;
    }

    /** Drives the gate the way GlitchEngine does: each host block goes into the capture buffer
        first, then it is rendered in segments split at the script's events. Outside the active
        ranges the audio stays dry and the effect is not called. */
    StereoBuffer render (const GateParams& params, const StereoBuffer& input, int blockSize, const Script& script)
    {
        Gate gate;
        gate.prepare (sampleRate, blockSize);
        gate.setParams (params);

        CaptureBuffer capture;
        capture.prepare (captureCapacity);

        StereoBuffer output = input;
        const int total = input.size();
        size_t triggersFired = 0;
        size_t changesApplied = 0;

        for (int blockStart = 0; blockStart < total; blockStart += blockSize)
        {
            const int blockEnd = std::min (blockStart + blockSize, total);
            capture.write (input.left.data() + blockStart, input.right.data() + blockStart, blockEnd - blockStart);

            int64_t segmentStart = blockStart;

            while (segmentStart < blockEnd)
            {
                while (changesApplied < script.changes.size() && script.changes[changesApplied].index <= segmentStart)
                    gate.setParams (script.changes[changesApplied++].params);

                while (triggersFired < script.triggers.size() && script.triggers[triggersFired].index <= segmentStart)
                    gate.trigger (script.triggers[triggersFired++].step);

                const auto segmentEnd = nextEvent (script, segmentStart, blockEnd);

                if (isActive (script.active, segmentStart))
                    gate.process (capture,
                                  output.block (static_cast<int> (segmentStart), static_cast<int> (segmentEnd - segmentStart)),
                                  segmentStart);

                segmentStart = segmentEnd;
            }
        }

        return output;
    }

    StereoBuffer makeConstant (int numSamples, float value)
    {
        StereoBuffer buffer (numSamples);
        std::fill (buffer.left.begin(), buffer.left.end(), value);
        std::fill (buffer.right.begin(), buffer.right.end(), value);
        return buffer;
    }

    /** The gate's gain curve: what it does to a DC input of 1. */
    std::vector<float> gainCurve (const GateParams& params, int total, const Script& script, int blockSize = 512)
    {
        return render (params, makeConstant (total, 1.0f), blockSize, script).left;
    }

    float maxDifference (const std::vector<float>& a, const std::vector<float>& b, size_t begin = 0, size_t end = SIZE_MAX)
    {
        float result = 0.0f;
        end = std::min ({ end, a.size(), b.size() });

        for (size_t i = begin; i < end; ++i)
            result = std::max (result, std::fabs (a[i] - b[i]));

        return result;
    }

    double mean (const std::vector<float>& samples, size_t begin, size_t end)
    {
        double sum = 0.0;

        for (size_t i = begin; i < end; ++i)
            sum += static_cast<double> (samples[i]);

        return sum / static_cast<double> (end - begin);
    }

    /** Positions (with linear sub-sample interpolation) where the curve rises through 0.5. */
    std::vector<double> risingHalfGainCrossings (const std::vector<float>& curve)
    {
        std::vector<double> crossings;

        for (size_t i = 1; i < curve.size(); ++i)
            if (curve[i - 1] < 0.5f && curve[i] >= 0.5f)
                crossings.push_back (static_cast<double> (i - 1)
                                     + (0.5 - static_cast<double> (curve[i - 1])) / static_cast<double> (curve[i] - curve[i - 1]));

        return crossings;
    }

    size_t at (int64_t index)
    {
        return static_cast<size_t> (index);
    }
} // namespace

TEST_CASE ("gate duty and timing measured on a DC input")
{
    // 1/32 note at 120 bpm and 48 kHz: 3000-sample cycles.
    constexpr int cycle = 3000;
    constexpr int numCycles = 4;

    for (const float duty : { 0.05f, 0.25f, 0.5f, 0.75f, 0.95f })
    {
        const auto curve = gainCurve (makeParams (0.125f, duty), cycle * numCycles, oneStep (cycle * numCycles));
        const auto open = static_cast<int> (std::lround (duty * cycle));

        for (int cycleIndex = 0; cycleIndex < numCycles; ++cycleIndex)
        {
            const auto start = static_cast<size_t> (cycleIndex * cycle);

            // The mean gain over a cycle is exactly the duty.
            CHECK_NEAR (mean (curve, start, start + cycle), duty, 1.0e-4);

            // Both edges sit inside the cycle: closed at its first and last samples.
            CHECK (curve[start] == 0.0f);
            CHECK (curve[start + 1] > 0.0f);
            CHECK (curve[start + cycle - 1] == 0.0f);

            // Half-gain points are half a ramp after the attack and release starts.
            CHECK_NEAR (curve[start + ramp / 2], 0.5, 1.0e-6);
            CHECK_NEAR (curve[start + static_cast<size_t> (open + ramp / 2)], 0.5, 1.0e-5); // duty is a float

            int aboveHalf = 0;
            bool fullyOpen = true;
            bool fullyClosed = true;

            for (int position = 0; position < cycle; ++position)
            {
                const float gain = curve[start + static_cast<size_t> (position)];
                aboveHalf += gain > 0.5f ? 1 : 0;

                if (position >= ramp && position < open)
                    fullyOpen = fullyOpen && gain == 1.0f;

                if (position >= open + ramp)
                    fullyClosed = fullyClosed && gain < 1.0e-6f;
            }

            CHECK (std::abs (aboveHalf - open) <= 1);
            CHECK (fullyOpen);
            CHECK (fullyClosed);
        }
    }
}

TEST_CASE ("gate duty is clamped to its range")
{
    constexpr int total = 12000;
    const auto script = oneStep (total);

    const auto shortest = gainCurve (makeParams (0.125f, 0.05f), total, script);
    const auto longest = gainCurve (makeParams (0.125f, 0.95f), total, script);
    const auto half = gainCurve (makeParams (0.125f, 0.5f), total, script);

    CHECK (maxDifference (gainCurve (makeParams (0.125f, 0.0f), total, script), shortest) == 0.0f);
    CHECK (maxDifference (gainCurve (makeParams (0.125f, -2.0f), total, script), shortest) == 0.0f);
    CHECK (maxDifference (gainCurve (makeParams (0.125f, 1.0f), total, script), longest) == 0.0f);
    CHECK (maxDifference (gainCurve (makeParams (0.125f, 7.0f), total, script), longest) == 0.0f);
    CHECK (maxDifference (gainCurve (makeParams (0.125f, std::nanf ("")), total, script), half) == 0.0f);
}

TEST_CASE ("gate edges ramp over about a millisecond")
{
    constexpr int cycle = 3000;
    constexpr int open = 1500;
    const auto curve = gainCurve (makeParams (0.125f, 0.5f), cycle * 2, oneStep (cycle * 2));

    bool attackRises = true;
    bool releaseFalls = true;
    bool symmetric = true;
    int attackInBetween = 0;
    int releaseInBetween = 0;

    for (int p = 0; p < ramp; ++p)
    {
        const auto attack = static_cast<size_t> (p);
        const auto release = static_cast<size_t> (open + p);

        attackRises = attackRises && curve[attack + 1] > curve[attack];
        releaseFalls = releaseFalls && curve[release + 1] < curve[release];
        symmetric = symmetric && std::fabs (curve[release] - (1.0f - curve[attack])) < 1.0e-6f;
        attackInBetween += curve[attack] > 0.0f && curve[attack] < 1.0f ? 1 : 0;
        releaseInBetween += curve[release] > 0.0f && curve[release] < 1.0f ? 1 : 0;
    }

    CHECK (attackRises);
    CHECK (releaseFalls);
    CHECK (symmetric);
    CHECK (attackInBetween == ramp - 1);
    CHECK (releaseInBetween == ramp - 1);

    // A raised cosine: gentle at both ends, steepest in the middle, never a step.
    CHECK (curve[1] < 0.01f);
    CHECK (curve[ramp - 1] > 0.99f);
    CHECK (maxJump (curve) <= static_cast<float> (maxRampSlope));
}

TEST_CASE ("gate ramps shrink to fit short open and closed sections")
{
    // 2 ms cycles: the open and closed sections are shorter than two 1 ms ramps.
    constexpr int total = 4800;

    for (const float duty : { 0.05f, 0.5f, 0.95f })
    {
        const auto curve = gainCurve (makeParams (0.001f, duty), total, oneStep (total));
        constexpr int cycle = 96;

        // Every cycle still starts closed and the release has (nearly) finished by its end, so
        // neither ramp spills into the next cycle.
        bool closedAtBoundaries = true;
        bool periodic = true;

        for (int start = 0; start + cycle < total; start += cycle)
        {
            closedAtBoundaries = closedAtBoundaries && curve[static_cast<size_t> (start)] == 0.0f
                                 && curve[static_cast<size_t> (start + cycle - 1)] < 0.15f;

            for (int p = 0; p < cycle; ++p)
                periodic = periodic && std::fabs (curve[static_cast<size_t> (start + p)] - curve[static_cast<size_t> (p)]) < 1.0e-5f;
        }

        CHECK (closedAtBoundaries);
        CHECK (periodic);
        CHECK_NEAR (mean (curve, 0, total), duty, 2.0e-3);
        CHECK (peak (curve) > 0.99f);
    }
}

TEST_CASE ("gate cycle follows the division, the tempo and a 2 ms minimum")
{
    // 44.1 kHz at 120 bpm: 22050 samples per beat, so a 1/16 cycle is a fractional 5512.5 samples.
    struct Case
    {
        float divisionBeats;
        double beatLength;
        double expectedCycle;
    };

    const std::vector<Case> cases { { 0.25f, 22050.0, 5512.5 },
                                    { 0.125f, 24000.0, 3000.0 },
                                    { 0.03125f, 24000.0, 750.0 },
                                    { 0.5f, 18000.0, 9000.0 },     // 160 bpm at 48 kHz
                                    { 0.03125f, 1000.0, 96.0 },    // would be 31.25 samples: held at 2 ms
                                    { 0.0f, 24000.0, 3000.0 } };   // invalid division: back to 1/32

    for (const auto& testCase : cases)
    {
        constexpr int total = 40000;
        Script script { { { 0, makeStep (0, total, testCase.beatLength) } }, {}, {} };
        const auto curve = gainCurve (makeParams (testCase.divisionBeats, 0.5f), total, script);
        const auto crossings = risingHalfGainCrossings (curve);

        CHECK (crossings.size() >= 4);
        bool onGrid = true;

        for (size_t k = 0; k < crossings.size(); ++k)
        {
            // The rising half-gain point sits half a ramp (or half the shortened ramp) into each cycle.
            const double rampLength = std::min (48.0, 0.5 * testCase.expectedCycle);
            const double expected = static_cast<double> (k) * testCase.expectedCycle + 0.5 * rampLength;
            onGrid = onGrid && std::fabs (crossings[k] - expected) < 1.0;
        }

        CHECK (onGrid);
    }
}

TEST_CASE ("gate cycles start at the step start")
{
    constexpr int total = 20000;
    const auto params = makeParams (0.125f, 0.4f);

    const auto fromZero = gainCurve (params, total, oneStep (total));
    const auto shifted = gainCurve (params, total, { { stepAt (1234, total - 1234) }, { { 1234, total } }, {} });

    // Dry before the step, then the same pattern moved along.
    CHECK (*std::min_element (shifted.begin(), shifted.begin() + 1234) == 1.0f);
    CHECK (shifted[1234] == 0.0f);

    float worst = 0.0f;

    for (size_t i = 1234; i < shifted.size(); ++i)
        worst = std::max (worst, std::fabs (shifted[i] - fromZero[i - 1234]));

    CHECK (worst < 1.0e-6f);
}

TEST_CASE ("gate gain stays between zero and one")
{
    const auto input = makeNoise (24000, 8, 1.0f);
    const auto output = render (makeParams (0.0625f, 0.3f), input, 256, everyStep (24000, 4321));
    bool attenuates = true;

    for (size_t i = 0; i < input.left.size(); ++i)
    {
        attenuates = attenuates && std::fabs (output.left[i]) <= std::fabs (input.left[i]);
        attenuates = attenuates && std::fabs (output.right[i]) <= std::fabs (input.right[i]);

        // Same gain on both channels.
        if (input.left[i] != 0.0f && input.right[i] != 0.0f)
            attenuates = attenuates && std::fabs (output.left[i] / input.left[i] - output.right[i] / input.right[i]) < 1.0e-5f;
    }

    CHECK (attenuates);
}

TEST_CASE ("gate does not click on a low sine")
{
    constexpr int total = 48000;
    constexpr double frequency = 60.0;
    constexpr float amplitude = 0.8f;
    const auto input = makeSine (total, frequency, sampleRate, amplitude);

    const auto sineSlope = static_cast<float> (2.0 * pi * frequency / sampleRate) * amplitude;

    for (const auto& params : { makeParams (0.125f, 0.5f), makeParams (0.03125f, 0.05f), makeParams (0.0001f, 0.95f),
                                makeParams (0.5f, 0.75f), makeParams (1.0f / 6.0f, 0.33f) })
    {
        // A hard gate would jump by up to the full amplitude; the ramps limit it to the sine's own
        // slope plus amplitude times the steepest ramp slope. Short open or closed sections
        // shorten the ramps to fit, which allows a steeper (but still ramped) edge.
        const double cycle = std::max (static_cast<double> (params.divisionBeats) * samplesPerBeat, 0.002 * sampleRate);
        const double duty = static_cast<double> (params.duty);
        const double rampLength = std::min ({ static_cast<double> (ramp), duty * cycle, (1.0 - duty) * cycle });
        const auto bound = sineSlope + amplitude * static_cast<float> (pi / (2.0 * rampLength) + 1.0e-6);
        CHECK (bound < 0.4f * amplitude); // far below the jump of an unramped gate
        // Steps every 1/16 (retriggers on and off the cycle grid) and steps of an awkward length.
        for (const auto& script : { everyStep (total), everyStep (total, 4999), oneStep (total) })
        {
            const auto output = render (params, input, 512, script);
            CHECK (maxJump (output.left) <= bound);
            CHECK (maxJump (output.right) <= bound);
        }
    }
}

TEST_CASE ("gate retrigger while open does not dip")
{
    // 1/8 cycles (12000 samples) and 1/16 steps: the gate is still open when the next step starts.
    constexpr int total = 24000;
    const auto curve = gainCurve (makeParams (0.5f, 0.75f), total, { { stepAt (0), stepAt (6000, 18000) }, {}, {} });

    // Fully open across the step boundary, no fresh attack from zero.
    CHECK (*std::min_element (curve.begin() + 100, curve.begin() + 6000 + 2000) == 1.0f);

    // The new step's own cycle then closes at 6000 + 0.75 * 12000.
    CHECK (curve[6000 + 9000 + ramp] == 0.0f);
    CHECK (curve[6000 + 9000 - 1] == 1.0f);
    CHECK (maxJump (curve) <= static_cast<float> (maxRampSlope));
}

TEST_CASE ("gate retrigger during a release turns round smoothly")
{
    // The release runs from 5990, so the step boundary at 6000 lands in the middle of it.
    constexpr int total = 18000;
    const auto curve = gainCurve (makeParams (0.5f, 5990.0f / 12000.0f), total, everyStep (total));

    CHECK (curve[5999] < 1.0f);
    CHECK (curve[5999] > 0.9f);
    CHECK (curve[6000] == curve[5999]); // the new attack starts from the gain reached
    CHECK (curve[6000 + ramp] == 1.0f);
    CHECK (maxJump (curve) <= static_cast<float> (maxRampSlope));
}

TEST_CASE ("gate late retrigger blends from the current gain")
{
    // A transport jump: the trigger for a step that began at 5400 arrives at 7000, while the gate
    // is sounding. The old pattern is open there; the new one is closed (position 1600 of 3000).
    constexpr int total = 12000;
    const Script script { { stepAt (0, 6000), { 7000, makeStep (5400, 6000) } }, {}, {} };
    const auto curve = gainCurve (makeParams (0.125f, 0.5f), total, script);

    CHECK (curve[6999] == 1.0f);
    CHECK (curve[7000] == 1.0f);
    CHECK (curve[7000 + ramp] == 0.0f);
    CHECK (maxJump (curve) <= static_cast<float> (maxRampSlope));

    // ...after which it follows the new pattern: the next cycle starts at 5400 + 3000.
    CHECK (curve[8400] == 0.0f);
    CHECK (curve[8400 + 100] == 1.0f);
}

TEST_CASE ("gate trigger after a gap starts its attack from zero")
{
    // 1/8 cycles keep the gate open at 5999, but the engine plays something else from 6000 to
    // 9000, so the next Gate step starts fresh instead of carrying the old gain.
    constexpr int total = 15000;
    const Script script { { stepAt (0), stepAt (9000) }, { { 0, 6000 }, { 9000, total } }, {} };
    const auto curve = gainCurve (makeParams (0.5f, 0.75f), total, script);

    CHECK (curve[5999] == 1.0f);
    CHECK (curve[6000] == 1.0f); // dry
    CHECK (curve[9000] == 0.0f);
    CHECK (curve[9000 + ramp] == 1.0f);

    // A late trigger after a gap renders the pattern where it is; the engine crossfades into it.
    const Script late { { stepAt (0), { 9100, makeStep (9000, 6000) } }, { { 0, 6000 }, { 9100, total } }, {} };
    const auto lateCurve = gainCurve (makeParams (0.5f, 0.75f), total, late);
    CHECK (lateCurve[9100] == 1.0f);
}

TEST_CASE ("gate parameters apply from the next trigger")
{
    constexpr int total = 12000;
    const Script script { { stepAt (0), stepAt (6000) }, {}, { { 1000, makeParams (0.25f, 0.2f) } } };
    const auto curve = gainCurve (makeParams (0.125f, 0.5f), total, script);
    const auto reference = gainCurve (makeParams (0.125f, 0.5f), 6000, oneStep (6000));

    // The first step keeps the parameters it started with...
    CHECK (maxDifference (curve, reference, 0, 6000) == 0.0f);

    // ...the second uses the new ones: 1/16 cycles (6000 samples), open for 1200.
    CHECK (curve[6000 + 1100] == 1.0f);
    CHECK (curve[6000 + 1200 + ramp] < 1.0e-6f);
    CHECK (curve[6000 + 3000] == 0.0f);
}

TEST_CASE ("gate only uses the current input")
{
    constexpr int total = 30000;
    constexpr int64_t divergeAt = 14321;
    const auto first = makeNoise (total, 2, 0.9f);
    auto second = first;
    const auto other = makeNoise (total, 77, 0.9f);

    for (size_t i = at (divergeAt); i < second.left.size(); ++i)
    {
        second.left[i] = other.left[i];
        second.right[i] = other.right[i];
    }

    const Script script { { stepAt (0), stepAt (6000), { 12500, makeStep (12000, 6000) }, stepAt (18000), stepAt (24000) },
                          { { 0, 9000 }, { 12500, total } },
                          {} };

    for (const int blockSize : { 1, 64, 1000 })
    {
        const auto outputFirst = render (makeParams (0.0625f, 0.6f), first, blockSize, script);
        const auto outputSecond = render (makeParams (0.0625f, 0.6f), second, blockSize, script);

        CHECK (maxDifference (outputFirst.left, outputSecond.left, 0, at (divergeAt)) == 0.0f);
        CHECK (maxDifference (outputFirst.right, outputSecond.right, 0, at (divergeAt)) == 0.0f);
        CHECK (maxDifference (outputFirst.left, outputSecond.left, at (divergeAt)) > 0.0f);
    }
}

TEST_CASE ("gate output does not depend on block size")
{
    constexpr int total = 36000;
    const auto input = makeNoise (total, 23, 0.9f);

    // On-grid and off-grid retriggers, a retrigger mid-release, a gap, a late trigger, a parameter change.
    const Script script { { stepAt (0, 5990), stepAt (5990, 6010), stepAt (12000), { 20111, makeStep (18000, 6000) },
                            stepAt (24000), stepAt (27777, 8223) },
                          { { 0, 15000 }, { 20111, total } },
                          { { 25000, makeParams (1.0f / 6.0f, 0.8f) } } };

    for (const auto& params : { makeParams (0.125f, 0.5f), makeParams (0.5f, 0.499f), makeParams (0.0001f, 0.05f) })
    {
        const auto reference = render (params, input, total, script);

        for (const int blockSize : { 1, 7, 64, 512 })
        {
            const auto output = render (params, input, blockSize, script);
            CHECK (maxDifference (output.left, reference.left) == 0.0f);
            CHECK (maxDifference (output.right, reference.right) == 0.0f);
        }
    }
}

TEST_CASE ("gate stays finite for silence, DC, full-scale noise, denormals and extreme parameters")
{
    constexpr int total = 8000;
    const float nan = std::nanf ("");
    const float infinity = std::numeric_limits<float>::infinity();
    const float denormal = std::numeric_limits<float>::denorm_min() * 100.0f;

    const std::vector<StereoBuffer> inputs { StereoBuffer (total), makeConstant (total, 1.0f), makeConstant (total, -1.0f),
                                             makeNoise (total, 5, 1.0f), makeConstant (total, denormal) };

    const std::vector<GateParams> paramSets { makeParams (0.03125f, 0.05f), makeParams (0.5f, 0.95f), makeParams (0.0f, 0.0f),
                                              makeParams (-1.0f, -1.0f), makeParams (1.0e-9f, 0.5f), makeParams (1.0e9f, 0.5f),
                                              makeParams (nan, nan), makeParams (infinity, infinity), makeParams (1.0f, 2.0f) };

    const std::vector<double> beatLengths { 24000.0, 0.0, -5.0, 1.0e15, std::nan (""), 1.0 };

    for (const auto& input : inputs)
    {
        const float inputPeak = std::max (peak (input.left), peak (input.right));

        for (const auto& params : paramSets)
        {
            for (const double beatLength : beatLengths)
            {
                const Script script { { { 0, makeStep (0, 3000, beatLength) },
                                        { 3000, makeStep (3000, 1, beatLength) },
                                        { 3001, makeStep (3001, 2999, beatLength) },
                                        { 6500, makeStep (6000, 2000, beatLength) } },
                                      { { 0, 6000 }, { 6500, total } },
                                      {} };

                const auto output = render (params, input, 97, script);
                CHECK (allFinite (output.left));
                CHECK (allFinite (output.right));
                CHECK (peak (output.left) <= inputPeak);
                CHECK (peak (output.right) <= inputPeak);
            }
        }
    }
}

TEST_CASE ("gate processed without a trigger free-runs at 120 bpm")
{
    Gate gate;
    gate.prepare (sampleRate, 512);
    gate.setParams (makeParams (0.125f, 0.5f));

    CaptureBuffer capture;
    capture.prepare (8192);
    auto buffer = makeConstant (6000, 1.0f);

    for (int start = 0; start < 6000; start += 500)
    {
        capture.write (buffer.left.data() + start, buffer.right.data() + start, 500);
        gate.process (capture, buffer.block (start, 500), start + 1000);
    }

    // Cycles of 3000 samples from the first processed sample.
    CHECK (buffer.left[0] == 0.0f);
    CHECK (buffer.left[1000] == 1.0f);
    CHECK (buffer.left[2000] == 0.0f);
    CHECK (buffer.left[3000] == 0.0f);
    CHECK (buffer.left[4000] == 1.0f);
    CHECK (allFinite (buffer.left));

    // reset() drops the running pattern; a fresh trigger starts from zero gain.
    gate.reset();
    gate.trigger (makeStep (0, 6000));
    auto again = makeConstant (64, 1.0f);
    capture.reset();
    capture.write (again.left.data(), again.right.data(), 64);
    gate.process (capture, again.block(), 0);
    CHECK (again.left[0] == 0.0f);
    CHECK (again.left[ramp] == 1.0f);
}
