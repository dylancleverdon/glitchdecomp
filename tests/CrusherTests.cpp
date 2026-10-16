#include "TestFramework.h"
#include "TestSignals.h"

#include "dsp/CaptureBuffer.h"
#include "dsp/effects/Crusher.h"

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
        int64_t index; // applied just before the sample at this absolute index is rendered
        CrusherParams params;
    };

    /** What happens during a render, besides the audio. */
    struct Script
    {
        std::vector<Trigger> triggers;
        std::vector<Range> active; // the effect only runs inside these ranges; empty = always
        std::vector<ParamChange> changes;
    };

    StepContext makeStep (int64_t start, int64_t length)
    {
        StepContext step;
        step.stepStart = start;
        step.stepLength = length;
        step.sampleRate = sampleRate;
        step.samplesPerBeat = samplesPerBeat;
        return step;
    }

    Trigger stepAt (int64_t start, int64_t length = sixteenth)
    {
        return { start, makeStep (start, length) };
    }

    /** A single step starting at 0, with the effect active throughout. */
    Script oneStep (int64_t length)
    {
        return { { stepAt (0, length) }, {}, {} };
    }

    CrusherParams makeParams (float bits, float downsample)
    {
        CrusherParams params;
        params.bits = bits;
        params.downsample = downsample;
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

    /** First script event after `index`, or `limit` if there is none before it. */
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

    /** Drives the crusher the way GlitchEngine does: each host block goes into the capture
        buffer first, then it is rendered in segments split at the script's events. Outside
        the active ranges the audio stays dry and the effect is not called. */
    StereoBuffer render (const CrusherParams& params, const StereoBuffer& input, int blockSize, const Script& script = {})
    {
        Crusher crusher;
        crusher.prepare (sampleRate, blockSize);
        crusher.setParams (params);

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
                    crusher.setParams (script.changes[changesApplied++].params);

                while (triggersFired < script.triggers.size() && script.triggers[triggersFired].index <= segmentStart)
                    crusher.trigger (script.triggers[triggersFired++].step);

                const auto segmentEnd = nextEvent (script, segmentStart, blockEnd);

                if (isActive (script.active, segmentStart))
                    crusher.process (capture,
                                     output.block (static_cast<int> (segmentStart), static_cast<int> (segmentEnd - segmentStart)),
                                     segmentStart);

                segmentStart = segmentEnd;
            }
        }

        return output;
    }

    float maxDifference (const std::vector<float>& a, const std::vector<float>& b, size_t begin = 0, size_t end = SIZE_MAX)
    {
        float result = 0.0f;
        end = std::min ({ end, a.size(), b.size() });

        for (size_t i = begin; i < end; ++i)
            result = std::max (result, std::fabs (a[i] - b[i]));

        return result;
    }

    StereoBuffer makeConstant (int numSamples, float value)
    {
        StereoBuffer buffer (numSamples);
        std::fill (buffer.left.begin(), buffer.left.end(), value);
        std::fill (buffer.right.begin(), buffer.right.end(), value);
        return buffer;
    }

    bool isMultipleOf (float value, float step)
    {
        const double ratio = static_cast<double> (value) / static_cast<double> (step);
        return std::fabs (ratio - std::round (ratio)) < 1.0e-3;
    }

    size_t at (int64_t index)
    {
        return static_cast<size_t> (index);
    }
} // namespace

TEST_CASE ("crusher at 16 bits without downsampling is near transparent")
{
    const auto params = makeParams (16.0f, 1.0f);
    const float halfStep = 0.5f * Crusher::quantiserStep (16.0f);

    for (const auto& input : { makeSine (20000, 440.0, sampleRate, 0.9f), makeNoise (20000, 7, 0.99f) })
    {
        const auto output = render (params, input, 512, oneStep (20000));

        CHECK (maxDifference (output.left, input.left) <= halfStep + 1.0e-7f);
        CHECK (maxDifference (output.right, input.right) <= halfStep + 1.0e-7f);
        CHECK (halfStep < 2.0e-5f);
    }
}

TEST_CASE ("crusher quantiser step falls smoothly with bit depth")
{
    CHECK_NEAR (Crusher::quantiserStep (1.0f), 2.0 / 3.0, 1e-7);
    CHECK_NEAR (Crusher::quantiserStep (2.0f), 0.4, 1e-7);
    CHECK_NEAR (Crusher::quantiserStep (6.0f), 2.0 / 65.0, 1e-7);
    CHECK_NEAR (Crusher::quantiserStep (16.0f), 2.0 / 65537.0, 1e-10);

    float previous = Crusher::quantiserStep (1.0f);
    bool decreasing = true;
    bool smooth = true;

    for (int i = 1; i <= 1500; ++i)
    {
        const float bits = 1.0f + 0.01f * static_cast<float> (i);
        const float step = Crusher::quantiserStep (bits);
        decreasing = decreasing && step < previous;
        smooth = smooth && step > previous * 0.99f; // 0.01 bit never changes the step by more than ~0.7 %
        previous = step;
    }

    CHECK (decreasing);
    CHECK (smooth);

    // Out-of-range and non-finite bit depths are clamped to the documented range.
    CHECK (Crusher::quantiserStep (0.0f) == Crusher::quantiserStep (1.0f));
    CHECK (Crusher::quantiserStep (-8.0f) == Crusher::quantiserStep (1.0f));
    CHECK (Crusher::quantiserStep (40.0f) == Crusher::quantiserStep (16.0f));
    CHECK (Crusher::quantiserStep (std::nanf ("")) == Crusher::quantiserStep (16.0f));
    CHECK (Crusher::quantiserStep (std::numeric_limits<float>::infinity()) == Crusher::quantiserStep (16.0f));
}

TEST_CASE ("crusher quantiser is mid-tread, symmetric and keeps silence silent")
{
    for (const float bits : { 1.0f, 2.5f, 6.0f, 16.0f })
    {
        const float step = Crusher::quantiserStep (bits);

        CHECK (Crusher::quantise (0.0f, step) == 0.0f);
        CHECK (Crusher::quantise (-0.0f, step) == 0.0f);
        CHECK (Crusher::quantise (1.0e-6f, step) == 0.0f);
        CHECK (Crusher::quantise (std::numeric_limits<float>::denorm_min(), step) == 0.0f);
        CHECK (Crusher::quantise (0.49f * step, step) == 0.0f);
        CHECK (Crusher::quantise (-0.49f * step, step) == 0.0f);
        CHECK (Crusher::quantise (std::nanf (""), step) == 0.0f);
        CHECK (Crusher::quantise (std::numeric_limits<float>::infinity(), step) == 0.0f);

        auto noise = makeNoise (4000, 3, 1.0f);
        bool symmetric = true;
        bool onGrid = true;

        for (const float x : noise.left)
        {
            const float level = Crusher::quantise (x, step);
            symmetric = symmetric && Crusher::quantise (-x, step) == -level;

            // Every level is a multiple of the step, except where the clamp pins it to +-1.
            onGrid = onGrid && (std::fabs (level) == 1.0f || isMultipleOf (level, step));
        }

        CHECK (symmetric);
        CHECK (onGrid);
    }
}

TEST_CASE ("crusher never adds gain")
{
    for (const float bits : { 1.0f, 1.3f, 2.0f, 4.0f, 6.0f, 9.7f, 16.0f })
    {
        const float step = Crusher::quantiserStep (bits);
        auto noise = makeNoise (4000, 11, 1.0f);
        bool inRange = true;
        bool hotNotLouder = true;
        bool hotClose = true;

        for (const float x : noise.left)
        {
            inRange = inRange && std::fabs (Crusher::quantise (x, step)) <= 1.0f;

            // Input that is already out of range is quantised but never clamped down to 1, nor raised.
            const float hot = 1.0f + 3.0f * std::fabs (x);
            const float level = Crusher::quantise (hot, step);
            hotNotLouder = hotNotLouder && level <= hot;
            hotClose = hotClose && std::fabs (level - hot) <= 0.5f * step + 1.0e-6f;
        }

        CHECK (inRange);
        CHECK (hotNotLouder);
        CHECK (hotClose);

        // Full scale lands on the nearest level at or below 1 (exactly 1 when the clamp catches it).
        CHECK (Crusher::quantise (1.0f, step) <= 1.0f);
        CHECK (Crusher::quantise (1.0f, step) >= 1.0f - 0.5f * step);
        CHECK (Crusher::quantise (-1.0f, step) == -Crusher::quantise (1.0f, step));
    }

    // Through the effect: full-scale noise stays within full scale.
    const auto noise = makeNoise (20000, 5, 1.0f);
    const auto output = render (makeParams (1.0f, 3.0f), noise, 256, oneStep (20000));
    CHECK (peak (output.left) <= 1.0f);
    CHECK (peak (output.right) <= 1.0f);

    // A hot DC input is not clamped to 1 either.
    const auto hot = render (makeParams (4.0f, 1.0f), makeConstant (1000, 1.5f), 256, oneStep (1000));
    CHECK (peak (hot.left) <= 1.5f);
    CHECK (peak (hot.left) > 1.3f);
}

TEST_CASE ("crusher at one bit is stable and keeps three levels")
{
    const auto input = makeSine (24000, 110.0, sampleRate, 0.5f);
    const auto output = render (makeParams (1.0f, 1.0f), input, 512, oneStep (24000));
    constexpr float level = 2.0f / 3.0f;

    bool threeLevels = true;
    bool signFollowsInput = true;
    int nonZero = 0;

    for (size_t i = 0; i < output.left.size(); ++i)
    {
        const float y = output.left[i];
        threeLevels = threeLevels && (y == 0.0f || std::fabs (std::fabs (y) - level) < 1.0e-6f);

        if (y != 0.0f)
        {
            ++nonZero;
            signFollowsInput = signFollowsInput && (y > 0.0f) == (input.left[i] > 0.0f);
        }
    }

    CHECK (threeLevels);
    CHECK (signFollowsInput);

    // A -6 dBFS sine is crushed into a pulse wave, not silenced: |sin| > 2/3 for about half the time.
    CHECK (nonZero > 10000);
    CHECK (nonZero < 14000);

    // Low-level noise around zero does not chatter.
    const auto quiet = render (makeParams (1.0f, 1.0f), makeNoise (4000, 9, 0.3f), 64, oneStep (4000));
    CHECK (peak (quiet.left) == 0.0f);
    CHECK (peak (quiet.right) == 0.0f);
}

TEST_CASE ("crusher integer downsample holds each sample for exactly that many samples")
{
    const auto input = makeRamp (6000, 0.01f, 1.0f / 8192.0f);
    const float step = Crusher::quantiserStep (16.0f);

    for (const int64_t stepStart : { int64_t { 0 }, int64_t { 1003 } })
    {
        for (const int factor : { 1, 2, 8, 13, 64 })
        {
            const auto output = render (makeParams (16.0f, static_cast<float> (factor)), input, 512,
                                        { { stepAt (stepStart, 6000 - stepStart) }, { { stepStart, 6000 } }, {} });
            bool held = true;

            for (int64_t n = stepStart; n < 6000; ++n)
            {
                const int64_t source = stepStart + (n - stepStart) / factor * factor;
                held = held && output.left[at (n)] == Crusher::quantise (input.left[at (source)], step);
                held = held && output.right[at (n)] == Crusher::quantise (input.right[at (source)], step);
            }

            CHECK (held);
        }
    }
}

TEST_CASE ("crusher fractional downsample runs like a phase accumulator")
{
    // 2.5 holds the samples at ceil (k * 2.5): 0, 3, 5, 8, 10, ... so runs alternate 3 and 2.
    Crusher crusher;
    crusher.prepare (sampleRate, 512);
    crusher.setParams (makeParams (16.0f, 2.5f));
    crusher.trigger (makeStep (0, 1000));

    CaptureBuffer capture;
    capture.prepare (4096);
    StereoBuffer block (1);
    capture.write (block.left.data(), block.right.data(), 1);
    crusher.process (capture, block.block(), 0); // starts the grid at 0

    const std::vector<int64_t> expected { 0, 0, 0, 3, 3, 5, 5, 5, 8, 8, 10, 10, 10, 13, 13, 15 };

    for (size_t n = 0; n < expected.size(); ++n)
        CHECK (crusher.heldIndex (static_cast<int64_t> (n)) == expected[n]);

    // Through the effect: the number of distinct runs matches total / factor for awkward factors.
    const auto input = makeRamp (20000, 0.0f, 1.0f / 32768.0f);

    for (const float factor : { 1.3f, 2.5f, 7.7f, 33.33f, 63.9f })
    {
        const auto output = render (makeParams (16.0f, factor), input, 256, oneStep (20000));
        int runs = 1;
        int longestRun = 1;
        int shortestRun = 1 << 30;
        int currentRun = 1;

        for (size_t i = 1; i < output.left.size(); ++i)
        {
            if (output.left[i] != output.left[i - 1])
            {
                shortestRun = std::min (shortestRun, currentRun);
                ++runs;
                currentRun = 1;
            }
            else
            {
                longestRun = std::max (longestRun, ++currentRun);
            }
        }

        const double expectedRuns = 20000.0 / static_cast<double> (factor);
        CHECK (std::fabs (runs - expectedRuns) <= 2.0);
        CHECK (longestRun == static_cast<int> (std::ceil (factor)));
        CHECK (shortestRun == static_cast<int> (std::floor (factor)));
    }
}

TEST_CASE ("crusher clamps the downsample factor to its range")
{
    const auto input = makeNoise (8000, 21, 0.8f);
    const Script script { { stepAt (0, 8000) }, {}, {} };

    const auto one = render (makeParams (8.0f, 1.0f), input, 128, script);
    const auto sixtyFour = render (makeParams (8.0f, 64.0f), input, 128, script);

    CHECK (maxDifference (render (makeParams (8.0f, 0.25f), input, 128, script).left, one.left) == 0.0f);
    CHECK (maxDifference (render (makeParams (8.0f, -3.0f), input, 128, script).left, one.left) == 0.0f);
    CHECK (maxDifference (render (makeParams (8.0f, std::nanf ("")), input, 128, script).left, one.left) == 0.0f);
    CHECK (maxDifference (render (makeParams (8.0f, 500.0f), input, 128, script).left, sixtyFour.left) == 0.0f);
    CHECK (maxDifference (render (makeParams (8.0f, std::numeric_limits<float>::infinity()), input, 128, script).left, one.left) == 0.0f);
}

TEST_CASE ("crusher only reads the past")
{
    constexpr int total = 30000;
    constexpr int64_t divergeAt = 13777;

    const auto first = makeNoise (total, 1, 0.7f);
    auto second = first;
    const auto other = makeNoise (total, 99, 0.7f);

    for (size_t i = at (divergeAt); i < second.left.size(); ++i)
    {
        second.left[i] = other.left[i];
        second.right[i] = other.right[i];
    }

    const Script script { { stepAt (0), stepAt (6000), { 12500, makeStep (12000, 6000) }, stepAt (18000), stepAt (24000) },
                          { { 0, 9000 }, { 12500, total } },
                          { { 13700, makeParams (3.0f, 2.7f) } } };

    for (const auto& params : { makeParams (6.0f, 8.0f), makeParams (2.0f, 63.7f), makeParams (11.0f, 1.0f) })
    {
        for (const int blockSize : { 1, 64, 1000 })
        {
            const auto outputFirst = render (params, first, blockSize, script);
            const auto outputSecond = render (params, second, blockSize, script);

            CHECK (maxDifference (outputFirst.left, outputSecond.left, 0, at (divergeAt)) == 0.0f);
            CHECK (maxDifference (outputFirst.right, outputSecond.right, 0, at (divergeAt)) == 0.0f);

            // Sanity: the outputs do differ afterwards, so the check above is not vacuous.
            CHECK (maxDifference (outputFirst.left, outputSecond.left, at (divergeAt)) > 0.0f);
        }
    }

    // The held index is never in the future, even straight after a downsample change.
    Crusher crusher;
    crusher.prepare (sampleRate, 64);
    CaptureBuffer capture;
    capture.prepare (4096);
    StereoBuffer block (64);
    bool causal = true;

    for (int blockIndex = 0; blockIndex < 40; ++blockIndex)
    {
        crusher.setParams (makeParams (6.0f, 1.0f + static_cast<float> ((blockIndex * 37) % 63) + 0.37f));
        capture.write (block.left.data(), block.right.data(), 64);
        crusher.process (capture, block.block(), blockIndex * 64);

        for (int64_t now = blockIndex * 64; now < (blockIndex + 1) * 64; ++now)
        {
            const auto held = crusher.heldIndex (now);
            causal = causal && held <= now && held >= now - 2 * static_cast<int64_t> (Crusher::maxDownsample);
        }
    }

    CHECK (causal);
}

TEST_CASE ("crusher output does not depend on block size")
{
    constexpr int total = 36000;
    const auto input = makeNoise (total, 17, 0.9f);

    // Contiguous retriggers, a gap, a late trigger and a live parameter change.
    const Script script { { stepAt (0), stepAt (6000), stepAt (12000), { 20111, makeStep (18000, 6000) }, stepAt (24000), stepAt (30000) },
                          { { 0, 15000 }, { 20111, total } },
                          { { 26500, makeParams (3.5f, 17.3f) } } };

    for (const auto& params : { makeParams (6.0f, 8.0f), makeParams (1.0f, 2.37f), makeParams (12.3f, 64.0f) })
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

TEST_CASE ("crusher retrigger while running keeps the hold grid")
{
    constexpr int total = 30000;
    const auto input = makeSine (total, 97.0, sampleRate, 0.8f);
    const auto params = makeParams (5.0f, 7.3f);

    const auto once = render (params, input, 512, oneStep (total));
    const auto everyStep = render (params, input, 512, { { stepAt (0), stepAt (6000), stepAt (12000), stepAt (18000), stepAt (24000) }, {}, {} });

    // Back-to-back Crusher steps are one continuous effect: identical to a single long step.
    CHECK (maxDifference (once.left, everyStep.left) == 0.0f);
    CHECK (maxDifference (once.right, everyStep.right) == 0.0f);
}

TEST_CASE ("crusher trigger after a gap restarts the grid at the step start")
{
    const auto input = makeRamp (20000, 0.0f, 1.0f / 32768.0f);
    const float step = Crusher::quantiserStep (16.0f);
    const auto output = render (makeParams (16.0f, 8.0f), input, 512,
                                { { stepAt (0), stepAt (9005) }, { { 0, 6000 }, { 9005, 15005 } }, {} });

    // Dry outside the active ranges.
    CHECK (maxDifference (output.left, input.left, 6000, 9005) == 0.0f);

    // The new grid starts on the step's first sample, not on the old grid (which would hold 9000).
    bool restarted = true;

    for (int64_t n = 9005; n < 15005; ++n)
    {
        const int64_t source = 9005 + (n - 9005) / 8 * 8;
        restarted = restarted && output.left[at (n)] == Crusher::quantise (input.left[at (source)], step);
    }

    CHECK (restarted);
}

TEST_CASE ("crusher late trigger anchors the grid at the step start")
{
    // After a transport jump the trigger can arrive after its step began, in an earlier block.
    const auto input = makeRamp (12000, 0.0f, 1.0f / 32768.0f);
    const float step = Crusher::quantiserStep (16.0f);
    const auto output = render (makeParams (16.0f, 7.0f), input, 512,
                                { { { 2000, makeStep (1000, 6000) } }, { { 2000, 7000 } }, {} });

    bool anchored = true;

    for (int64_t n = 2000; n < 7000; ++n)
    {
        const int64_t source = 1000 + (n - 1000) / 7 * 7;
        anchored = anchored && output.left[at (n)] == Crusher::quantise (input.left[at (source)], step);
    }

    CHECK (anchored);
    CHECK (maxDifference (output.left, input.left, 0, 2000) == 0.0f);
}

TEST_CASE ("crusher downsample change mid-step continues from the last hold point")
{
    const auto input = makeRamp (4000, 0.0f, 1.0f / 32768.0f);
    const float step = Crusher::quantiserStep (16.0f);

    auto expectHolds = [&] (const StereoBuffer& output, int64_t begin, int64_t end, int64_t anchor, int64_t factor) {
        bool ok = true;

        for (int64_t n = begin; n < end; ++n)
            ok = ok && output.left[at (n)] == Crusher::quantise (input.left[at (anchor + (n - anchor) / factor * factor)], step);

        return ok;
    };

    // Shorter: at 1004 the 16-grid holds 992; the 4-grid continues from 992, so 1004 is due now.
    const auto shorter = render (makeParams (16.0f, 16.0f), input, 64,
                                 { { stepAt (0, 4000) }, {}, { { 1004, makeParams (16.0f, 4.0f) } } });
    CHECK (expectHolds (shorter, 0, 1004, 0, 16));
    CHECK (expectHolds (shorter, 1004, 4000, 992, 4));

    // Longer: at 1002 the 4-grid holds 1000, which the 16-grid keeps until 1016.
    const auto longer = render (makeParams (16.0f, 4.0f), input, 64,
                                { { stepAt (0, 4000) }, {}, { { 1002, makeParams (16.0f, 16.0f) } } });
    CHECK (expectHolds (longer, 0, 1002, 0, 4));
    CHECK (expectHolds (longer, 1002, 4000, 1000, 16));
    CHECK (longer.left[1015] == longer.left[1000]);

    // A bit-depth change applies straight away.
    const auto bitsChange = render (makeParams (16.0f, 1.0f), input, 64,
                                    { { stepAt (0, 4000) }, {}, { { 2000, makeParams (3.0f, 1.0f) } } });
    CHECK (bitsChange.left[1999] == Crusher::quantise (input.left[1999], step));
    CHECK (bitsChange.left[2000] == Crusher::quantise (input.left[2000], Crusher::quantiserStep (3.0f)));
}

TEST_CASE ("crusher stays click-free on a low sine across retriggers and gaps")
{
    constexpr int total = 48000;
    constexpr double frequency = 40.0;
    constexpr float amplitude = 0.5f;
    const auto input = makeSine (total, frequency, sampleRate, amplitude);
    const float sineSlope = static_cast<float> (6.283185307179586 * frequency / sampleRate) * amplitude;

    Script script;

    for (int64_t start = 0; start < total; start += sixteenth)
        script.triggers.push_back (stepAt (start));

    for (const auto& params : { makeParams (6.0f, 8.0f), makeParams (10.0f, 3.3f), makeParams (16.0f, 1.0f) })
    {
        const float step = Crusher::quantiserStep (params.bits);

        // A held sample can lag the sine by up to ceil (factor) samples, plus half a step of rounding each side.
        const float bound = sineSlope * (std::ceil (params.downsample) + 1.0f) + step + 1.0e-6f;

        const auto continuous = render (params, input, 333, script);
        CHECK (maxJump (continuous.left) <= bound);
        CHECK (maxJump (continuous.right) <= bound);

        // With gaps, dry -> crushed edges are no worse (the engine crossfades them anyway).
        Script gappy = script;
        gappy.active = { { 0, 6000 }, { 12000, 18000 }, { 24000, 36000 } };
        const auto withGaps = render (params, input, 333, gappy);
        CHECK (maxJump (withGaps.left) <= bound);
    }
}

TEST_CASE ("crusher stays finite for silence, DC, full-scale noise, denormals and extreme parameters")
{
    constexpr int total = 8000;
    const float denormal = std::numeric_limits<float>::denorm_min() * 1000.0f;

    const std::vector<StereoBuffer> inputs { StereoBuffer (total), makeConstant (total, 1.0f), makeConstant (total, -1.0f),
                                             makeNoise (total, 3, 1.0f), makeConstant (total, denormal) };

    const float nan = std::nanf ("");
    const float infinity = std::numeric_limits<float>::infinity();
    const std::vector<CrusherParams> paramSets { makeParams (1.0f, 1.0f), makeParams (1.0f, 64.0f), makeParams (16.0f, 64.0f),
                                                 makeParams (16.0f, 1.0f), makeParams (1.0001f, 1.0001f), makeParams (15.999f, 63.999f),
                                                 makeParams (0.0f, 0.0f), makeParams (-5.0f, -5.0f), makeParams (100.0f, 1.0e9f),
                                                 makeParams (nan, nan), makeParams (infinity, infinity), makeParams (-infinity, -infinity) };

    const Script script { { stepAt (0, 3000), stepAt (3000, 1), stepAt (3001, 2999), { 6500, makeStep (6000, 2000) } },
                          { { 0, 6000 }, { 6500, total } },
                          {} };

    for (const auto& input : inputs)
    {
        const float inputPeak = std::max (peak (input.left), peak (input.right));

        for (const auto& params : paramSets)
        {
            const auto output = render (params, input, 97, script);
            CHECK (allFinite (output.left));
            CHECK (allFinite (output.right));
            CHECK (peak (output.left) <= std::max (1.0f, inputPeak));
            CHECK (peak (output.right) <= std::max (1.0f, inputPeak));
        }
    }

    // Silence and denormals come out as exact silence.
    const auto silent = render (makeParams (6.0f, 8.0f), inputs[0], 64, script);
    const auto denormals = render (makeParams (16.0f, 1.0f), inputs[4], 64, oneStep (total));
    CHECK (peak (silent.left) == 0.0f);
    CHECK (peak (denormals.left) == 0.0f);
    CHECK (peak (denormals.right) == 0.0f);
}

TEST_CASE ("crusher processes both channels on the same grid")
{
    // makeRamp's right channel is the negated left one, and the quantiser is symmetric.
    const auto input = makeRamp (6000, 0.1f, 1.0f / 16384.0f);
    const auto output = render (makeParams (4.7f, 5.5f), input, 100, { { stepAt (0) }, {}, {} });
    bool mirrored = true;

    for (size_t i = 0; i < output.left.size(); ++i)
        mirrored = mirrored && output.right[i] == -output.left[i];

    CHECK (mirrored);
}

TEST_CASE ("crusher reset forgets the running grid")
{
    const auto input = makeNoise (3000, 4, 0.6f);

    Crusher crusher;
    crusher.prepare (sampleRate, 512);
    crusher.setParams (makeParams (7.0f, 9.5f));

    auto renderFromZero = [&] {
        CaptureBuffer capture;
        capture.prepare (8192);
        StereoBuffer output = input;
        crusher.trigger (makeStep (0, 3000));

        for (int start = 0; start < 3000; start += 500)
        {
            capture.write (input.left.data() + start, input.right.data() + start, 500);
            crusher.process (capture, output.block (start, 500), start);
        }

        return output;
    };

    const auto first = renderFromZero();
    crusher.reset();
    const auto second = renderFromZero();

    CHECK (maxDifference (first.left, second.left) == 0.0f);
    CHECK (maxDifference (first.right, second.right) == 0.0f);
}
