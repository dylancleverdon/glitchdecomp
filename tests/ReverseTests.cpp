#include "TestFramework.h"
#include "TestSignals.h"

#include "dsp/CaptureBuffer.h"
#include "dsp/effects/Reverse.h"

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
    constexpr int64_t fade = 144;              // 3 ms at 48 kHz
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

    StepContext makeStep (int64_t start, int64_t length)
    {
        StepContext step;
        step.stepStart = start;
        step.stepLength = length;
        step.sampleRate = sampleRate;
        step.samplesPerBeat = samplesPerBeat;
        return step;
    }

    Trigger stepAt (int64_t start, int64_t length)
    {
        return { start, makeStep (start, length) };
    }

    /** A trigger that arrives after its step began, as after a transport jump. */
    Trigger lateTrigger (int64_t index, int64_t stepStart, int64_t length)
    {
        return { index, makeStep (stepStart, length) };
    }

    ReverseParams makeParams (float speed)
    {
        ReverseParams params;
        params.speed = speed;
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

    /** Drives the effect the way GlitchEngine does: each host block goes into the capture
        buffer first, then it is rendered in segments split at the trigger points. The effect
        only runs inside the `active` ranges (elsewhere the audio stays dry); empty = always. */
    StereoBuffer render (const ReverseParams& params, const StereoBuffer& input, int blockSize,
                         const std::vector<Trigger>& triggers, const std::vector<Range>& active = {},
                         int capacity = captureCapacity)
    {
        Reverse reverse;
        reverse.prepare (sampleRate, blockSize);
        reverse.setParams (params);

        CaptureBuffer capture;
        capture.prepare (capacity);

        StereoBuffer output = input;
        const auto total = static_cast<int64_t> (input.size());
        size_t nextTrigger = 0;

        for (int64_t blockStart = 0; blockStart < total; blockStart += blockSize)
        {
            const int64_t blockEnd = std::min (blockStart + blockSize, total);
            capture.write (input.left.data() + blockStart, input.right.data() + blockStart,
                           static_cast<int> (blockEnd - blockStart));

            for (int64_t segmentStart = blockStart; segmentStart < blockEnd;)
            {
                while (nextTrigger < triggers.size() && triggers[nextTrigger].index <= segmentStart)
                    reverse.trigger (triggers[nextTrigger++].step);

                int64_t segmentEnd = blockEnd;

                if (nextTrigger < triggers.size())
                    segmentEnd = std::min (segmentEnd, triggers[nextTrigger].index);

                for (const auto& range : active)
                    for (const auto edge : { range.begin, range.end })
                        if (edge > segmentStart)
                            segmentEnd = std::min (segmentEnd, edge);

                if (isActive (active, segmentStart))
                    reverse.process (capture,
                                     output.block (static_cast<int> (segmentStart), static_cast<int> (segmentEnd - segmentStart)),
                                     segmentStart);

                segmentStart = segmentEnd;
            }
        }

        return output;
    }

    StereoBuffer renderWhole (const ReverseParams& params, const StereoBuffer& input,
                              const std::vector<Trigger>& triggers, const std::vector<Range>& active = {})
    {
        return render (params, input, input.size(), triggers, active);
    }

    size_t at (int64_t index)
    {
        return static_cast<size_t> (index);
    }

    double maxDifference (const StereoBuffer& a, const StereoBuffer& b, int64_t begin, int64_t end)
    {
        double result = 0.0;

        for (auto i = at (begin); i < at (end); ++i)
        {
            result = std::max (result, static_cast<double> (std::fabs (a.left[i] - b.left[i])));
            result = std::max (result, static_cast<double> (std::fabs (a.right[i] - b.right[i])));
        }

        return result;
    }

    float stereoJump (const StereoBuffer& buffer, int64_t begin, int64_t end)
    {
        return std::max (maxJump (buffer.left, at (begin), at (end)), maxJump (buffer.right, at (begin), at (end)));
    }

    float stereoPeak (const StereoBuffer& buffer, int64_t begin, int64_t end)
    {
        return std::max (peak (buffer.left, at (begin), at (end)), peak (buffer.right, at (begin), at (end)));
    }

    StereoBuffer makeDc (int numSamples, float value)
    {
        StereoBuffer buffer (numSamples);
        std::fill (buffer.left.begin(), buffer.left.end(), value);
        std::fill (buffer.right.begin(), buffer.right.end(), value);
        return buffer;
    }

    /** `first` up to (not including) `from`, then `second`. */
    StereoBuffer splice (const StereoBuffer& first, const StereoBuffer& second, int64_t from)
    {
        StereoBuffer result = first;
        std::copy (second.left.begin() + from, second.left.end(), result.left.begin() + from);
        std::copy (second.right.begin() + from, second.right.end(), result.right.begin() + from);
        return result;
    }
} // namespace

TEST_CASE ("reverse passes dry audio until it is triggered")
{
    const auto input = makeNoise (20000, 21);
    const auto output = renderWhole (makeParams (1.0f), input, { stepAt (12000, sixteenth) });

    CHECK (maxDifference (output, input, 0, 12000) == 0.0);
    CHECK (maxDifference (output, input, 12000 + fade, 12000 + 2 * fade) > 0.1);
}

TEST_CASE ("reverse plays the audio before the step backwards")
{
    constexpr float rampStep = 1.0f / 65536.0f;
    const auto input = makeRamp (40000, 0.0f, rampStep);
    const int64_t start = 30000;
    const auto output = renderWhole (makeParams (1.0f), input, { stepAt (start, sixteenth) });

    // Away from the edge fades, output at step offset t is exactly input at start - 1 - t.
    double worst = 0.0;

    for (int64_t offset = fade; offset < sixteenth - fade; ++offset)
    {
        const auto played = at (start + offset);
        const auto source = at (start - 1 - offset);
        worst = std::max (worst, static_cast<double> (std::fabs (output.left[played] - input.left[source])));
        worst = std::max (worst, static_cast<double> (std::fabs (output.right[played] - input.right[source])));
    }

    CHECK (worst == 0.0);

    // So the ramp runs downhill while the input runs uphill.
    for (int64_t offset = fade; offset < sixteenth - fade - 1; ++offset)
        CHECK (output.left[at (start + offset + 1)] < output.left[at (start + offset)]);
}

TEST_CASE ("reverse speed 0.5 and 2 scale the backwards read rate")
{
    constexpr double rampStep = 1.0 / 65536.0;
    const auto input = makeRamp (40000, 0.0f, static_cast<float> (rampStep));
    const int64_t start = 30000;

    for (const float speed : { 0.5f, 2.0f, 1.25f })
    {
        const auto output = renderWhole (makeParams (speed), input, { stepAt (start, sixteenth) });
        double worst = 0.0;

        for (int64_t offset = fade; offset < sixteenth - fade; ++offset)
        {
            const double expected = (static_cast<double> (start - 1) - static_cast<double> (offset) * speed) * rampStep;
            worst = std::max (worst, std::fabs (output.left[at (start + offset)] - expected));
            worst = std::max (worst, std::fabs (output.right[at (start + offset)] + expected));
        }

        CHECK (worst < 2.0e-6);

        // Slope over the middle of the step, in input samples per output sample.
        const auto from = at (start + 1000);
        const auto to = at (start + 5000);
        const double slope = (static_cast<double> (output.left[to]) - output.left[from]) / static_cast<double> (to - from) / rampStep;
        CHECK_NEAR (slope, -speed, 1.0e-3);
    }
}

TEST_CASE ("reverse fades in and out at the step edges")
{
    // With a DC input the output is the fade envelope itself.
    const auto input = makeDc (40000, 1.0f);
    const int64_t start = 30000;
    const auto output = renderWhole (makeParams (1.0f), input, { stepAt (start, sixteenth) });

    CHECK (output.left[at (start)] == 0.0f);
    CHECK_NEAR (output.left[at (start + fade / 2)], 0.5, 0.02);
    CHECK (output.left[at (start + fade)] == 1.0f);
    CHECK (output.left[at (start + sixteenth - fade)] == 1.0f);
    CHECK_NEAR (output.left[at (start + sixteenth - fade / 2)], 0.5, 0.02);
    CHECK (output.left[at (start + sixteenth - 1)] < 0.001f);

    // After the step it stays silent (the engine may keep rendering it while fading out).
    CHECK (stereoPeak (output, start + sixteenth, 40000) == 0.0f);
    CHECK (stereoPeak (output, start, 40000) <= 1.0f);

    for (int64_t i = start + 1; i <= start + fade; ++i)
        CHECK (output.left[at (i)] >= output.left[at (i - 1)]);

    for (int64_t i = start + sixteenth - fade + 1; i <= start + sixteenth; ++i)
        CHECK (output.left[at (i)] <= output.left[at (i - 1)]);
}

TEST_CASE ("reverse edges do not click on a low sine")
{
    const auto input = makeSine (40000, 110.0, sampleRate);
    const float naturalSlope = maxJump (input.left);
    const int64_t start = 20000;

    for (const float speed : { 0.5f, 1.0f, 2.0f })
    {
        const auto output = renderWhole (makeParams (speed), input, { stepAt (start, sixteenth) });
        const auto bound = speed * naturalSlope + 0.006f;

        CHECK (stereoJump (output, start, start + sixteenth + 1000) < bound);
    }
}

TEST_CASE ("reverse reads before the stored audio are silence")
{
    const auto input = makeNoise (20000, 22);

    // Speed 2 from a step at 1000 runs past index 0 after 500 samples.
    {
        const auto output = renderWhole (makeParams (2.0f), input, { stepAt (1000, sixteenth) });
        CHECK (allFinite (output.left) && allFinite (output.right));
        CHECK (stereoPeak (output, 1000 + 500, 1000 + sixteenth) == 0.0f);
        CHECK (stereoPeak (output, 1000 + fade, 1000 + 400) > 0.1f);
    }

    // A small capture buffer: the head falls off the oldest stored sample about 2730 samples in.
    {
        const auto output = render (makeParams (2.0f), input, 1, { stepAt (12000, sixteenth) }, {}, 8192);
        CHECK (allFinite (output.left) && allFinite (output.right));
        CHECK (stereoPeak (output, 12000 + 2800, 12000 + sixteenth) == 0.0f);
        CHECK (stereoPeak (output, 12000 + fade, 12000 + 2000) > 0.1f);
    }
}

TEST_CASE ("reverse never reads ahead of the live input")
{
    const auto inputA = makeNoise (40000, 23);
    const auto inputB = makeNoise (40000, 24);
    const std::vector<Trigger> triggers { stepAt (12000, sixteenth), stepAt (18000, sixteenth), lateTrigger (26000, 25000, sixteenth) };

    for (const float speed : { 0.5f, 0.77f, 1.0f, 2.0f })
    {
        const auto params = makeParams (speed);
        const auto outputA = renderWhole (params, inputA, triggers);

        for (const int64_t divergeAt : { 12000, 12001, 12002, 12100, 15000, 18001, 26001, 30000 })
        {
            const auto outputB = renderWhole (params, splice (inputA, inputB, divergeAt), triggers);
            CHECK (maxDifference (outputA, outputB, 0, divergeAt) == 0.0);
        }
    }

    // Stronger: the whole step only depends on audio up to its first sample (which the
    // cubic may touch as look-ahead for a position just before the step).
    for (const float speed : { 0.5f, 1.0f, 2.0f })
    {
        const auto params = makeParams (speed);
        const auto outputA = renderWhole (params, inputA, { stepAt (12000, sixteenth) });
        const auto outputB = renderWhole (params, splice (inputA, inputB, 12001), { stepAt (12000, sixteenth) });
        CHECK (maxDifference (outputA, outputB, 0, 40000) == 0.0);
    }
}

TEST_CASE ("reverse retrigger on the next step fades out and back in without a click")
{
    const auto input = makeSine (40000, 110.0, sampleRate);
    const float naturalSlope = maxJump (input.left);

    for (const float speed : { 0.5f, 1.0f, 2.0f })
    {
        const auto output = renderWhole (makeParams (speed), input,
                                         { stepAt (12000, sixteenth), stepAt (18000, sixteenth), stepAt (24000, sixteenth) });

        CHECK (stereoJump (output, 12000, 31000) < speed * naturalSlope + 0.006f);
        CHECK (std::fabs (output.left[at (18000)]) < 1.0e-6f);
        CHECK (std::fabs (output.left[at (24000)]) < 1.0e-6f);

        // The second step plays the audio just before it, not a continuation of the first.
        const auto fresh = renderWhole (makeParams (speed), input, { stepAt (18000, sixteenth) }, { { 18000, 24000 } });
        CHECK (maxDifference (output, fresh, 18000, 24000) < 1.0e-6);
    }
}

TEST_CASE ("reverse late trigger while sounding crossfades from the old voice")
{
    const auto input = makeSine (40000, 110.0, sampleRate);
    const float naturalSlope = maxJump (input.left);
    const auto params = makeParams (1.0f);

    // A transport jump at 15000 lands 1000 samples into a step that "started" at 14000.
    const auto output = renderWhole (params, input, { stepAt (12000, sixteenth), lateTrigger (15000, 14000, sixteenth) });
    CHECK (stereoJump (output, 12000, 21000) < naturalSlope + 0.012f);

    // The old voice is still heard during the crossfade, then only the new one is left.
    const auto fresh = renderWhole (params, input, { lateTrigger (15000, 14000, sixteenth) }, { { 15000, 40000 } });
    CHECK (maxDifference (output, fresh, 15000, 15000 + fade / 2) > 0.05);
    CHECK (maxDifference (output, fresh, 15000 + fade, 40000) == 0.0);

    // The late voice reads from where it would be had it started on time.
    const auto onTime = renderWhole (params, input, { stepAt (14000, sixteenth) }, { { 14000, 40000 } });
    CHECK (maxDifference (fresh, onTime, 15000 + fade, 20000) == 0.0);
}

TEST_CASE ("reverse retrigger inside a crossfade stays smooth")
{
    const auto input = makeSine (40000, 110.0, sampleRate);
    const float naturalSlope = maxJump (input.left);
    const auto params = makeParams (1.0f);

    // Two transport jumps within one fade length: three voices overlap, and the oldest is
    // still loud when the second jump arrives.
    const std::vector<Trigger> triggers { stepAt (12000, sixteenth), lateTrigger (15000, 14000, sixteenth),
                                          lateTrigger (15030, 12500, sixteenth) };
    const auto output = renderWhole (params, input, triggers);

    // Two crossfades overlap, so allow for both slopes.
    CHECK (stereoJump (output, 12000, 21000) < naturalSlope + 0.025f);

    const auto fresh = renderWhole (params, input, { lateTrigger (15030, 12500, sixteenth) }, { { 15030, 40000 } });
    CHECK (maxDifference (output, fresh, 15030 + fade, 40000) == 0.0);
}

TEST_CASE ("reverse does not crossfade from a voice that stopped sounding")
{
    const auto input = makeNoise (40000, 25);
    const auto params = makeParams (1.0f);

    // A long reverse step that the engine leaves part-way (a transport jump to another
    // source), then a late reverse trigger while the old step would still be running.
    const auto output = renderWhole (params, input, { stepAt (16000, 4 * sixteenth), lateTrigger (21000, 20000, sixteenth) },
                                     { { 16000, 17000 }, { 21000, 40000 } });
    const auto fresh = renderWhole (params, input, { lateTrigger (21000, 20000, sixteenth) }, { { 21000, 40000 } });

    CHECK (maxDifference (output, fresh, 21000, 40000) == 0.0);
    CHECK (output.left[at (21000)] == 0.0f); // fades in from silence at the trigger
}

TEST_CASE ("reverse output does not depend on the block size")
{
    const auto input = makeNoise (48000, 26);
    const std::vector<Trigger> triggers { stepAt (6000, sixteenth), stepAt (12000, sixteenth), lateTrigger (15000, 14000, sixteenth),
                                          stepAt (24000, 2 * sixteenth), lateTrigger (40000, 39990, 100) };

    for (const float speed : { 0.5f, 1.37f, 2.0f })
    {
        for (const auto& active : { std::vector<Range> {}, std::vector<Range> { { 6000, 13000 }, { 15000, 20000 }, { 24000, 48000 } } })
        {
            const auto reference = renderWhole (makeParams (speed), input, triggers, active);

            for (const int blockSize : { 1, 7, 64, 512 })
            {
                const auto output = render (makeParams (speed), input, blockSize, triggers, active);
                CHECK (maxDifference (output, reference, 0, 48000) <= 1.0e-6);
            }
        }
    }
}

TEST_CASE ("reverse clamps the speed to the documented range")
{
    const auto input = makeNoise (30000, 27);
    const std::vector<Trigger> triggers { stepAt (20000, sixteenth) };

    const auto slowest = renderWhole (makeParams (0.5f), input, triggers);
    const auto normal = renderWhole (makeParams (1.0f), input, triggers);
    const auto fastest = renderWhole (makeParams (2.0f), input, triggers);

    CHECK (maxDifference (renderWhole (makeParams (0.0f), input, triggers), slowest, 0, 30000) == 0.0);
    CHECK (maxDifference (renderWhole (makeParams (-3.0f), input, triggers), slowest, 0, 30000) == 0.0);
    CHECK (maxDifference (renderWhole (makeParams (100.0f), input, triggers), fastest, 0, 30000) == 0.0);
    CHECK (maxDifference (renderWhole (makeParams (std::numeric_limits<float>::infinity()), input, triggers), normal, 0, 30000) == 0.0);
    CHECK (maxDifference (renderWhole (makeParams (std::numeric_limits<float>::quiet_NaN()), input, triggers), normal, 0, 30000) == 0.0);
}

TEST_CASE ("reverse output stays finite and bounded for any input and step")
{
    constexpr int length = 30000;
    std::vector<StereoBuffer> inputs { StereoBuffer (length), makeDc (length, 1.0f), makeDc (length, -1.0f),
                                       makeNoise (length, 28, 1.0f), makeDc (length, 1.0e-40f) };

    const std::vector<std::vector<Trigger>> schedules {
        { stepAt (5000, sixteenth), stepAt (11000, sixteenth) },
        { stepAt (0, 1), stepAt (1, 2), stepAt (3, 3), stepAt (6, 100), stepAt (106, 289), lateTrigger (500, 450, 0) },
        { lateTrigger (9000, -4000, 24000), lateTrigger (15000, 14000, -5) },
        { stepAt (1000, 4 * 48000) },
    };

    for (const auto& input : inputs)
    {
        const float inputPeak = peak (input.left);

        for (const float speed : { 0.5f, 1.0f, 2.0f })
        {
            for (const auto& triggers : schedules)
            {
                const auto output = render (makeParams (speed), input, 256, triggers);

                CHECK (allFinite (output.left) && allFinite (output.right));
                CHECK (stereoPeak (output, 0, length) <= 1.5f * inputPeak);
            }
        }
    }

    // DC never comes out louder than it went in (fades and crossfades sum to at most one).
    const auto dc = makeDc (length, 0.8f);
    const auto output = renderWhole (makeParams (1.0f), dc, { stepAt (6000, sixteenth), lateTrigger (9000, 8000, sixteenth),
                                                              lateTrigger (9050, 8500, sixteenth) });
    CHECK (stereoPeak (output, 0, length) <= 0.8f + 1.0e-6f);
}

TEST_CASE ("reverse reset drops the voice and returns to dry")
{
    const auto input = makeNoise (20000, 29);
    const auto ramp = makeRamp (20000);

    Reverse reverse;
    reverse.prepare (sampleRate, 10000);
    reverse.setParams (makeParams (1.0f));

    CaptureBuffer capture;
    capture.prepare (captureCapacity);

    capture.write (input.left.data(), input.right.data(), 10000);
    reverse.trigger (makeStep (4000, sixteenth));
    auto first = input;
    reverse.process (capture, first.block (0, 10000), 0);
    CHECK (maxDifference (first, input, 4000 + fade, 9000) > 0.1);

    reverse.reset();
    capture.reset();

    capture.write (ramp.left.data(), ramp.right.data(), 512);
    auto second = ramp;
    reverse.process (capture, second.block (0, 512), 0);
    CHECK (maxDifference (second, ramp, 0, 512) == 0.0);
}
