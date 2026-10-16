#include "TestFramework.h"
#include "TestSignals.h"

#include "dsp/CaptureBuffer.h"
#include "dsp/effects/Stutter.h"

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
    constexpr int64_t quarter = 24000;
    constexpr int64_t thirtySecond = 3000;     // slice for the default repeatBeats = 0.125
    constexpr int64_t crossfade = 144;         // 3 ms at 48 kHz
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

    StepContext makeStep (int64_t start, int64_t length, double beatLength = samplesPerBeat)
    {
        StepContext step;
        step.stepStart = start;
        step.stepLength = length;
        step.sampleRate = sampleRate;
        step.samplesPerBeat = beatLength;
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

    StutterParams makeParams (float repeatBeats, float glide = 0.0f, float decay = 0.0f)
    {
        StutterParams params;
        params.repeatBeats = repeatBeats;
        params.pitchGlideSemitones = glide;
        params.decay = decay;
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
    StereoBuffer render (const StutterParams& params, const StereoBuffer& input, int blockSize,
                         const std::vector<Trigger>& triggers, const std::vector<Range>& active = {})
    {
        Stutter stutter;
        stutter.prepare (sampleRate, blockSize);
        stutter.setParams (params);

        CaptureBuffer capture;
        capture.prepare (captureCapacity);

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
                    stutter.trigger (triggers[nextTrigger++].step);

                int64_t segmentEnd = blockEnd;

                if (nextTrigger < triggers.size())
                    segmentEnd = std::min (segmentEnd, triggers[nextTrigger].index);

                for (const auto& range : active)
                    for (const auto edge : { range.begin, range.end })
                        if (edge > segmentStart)
                            segmentEnd = std::min (segmentEnd, edge);

                if (isActive (active, segmentStart))
                    stutter.process (capture,
                                     output.block (static_cast<int> (segmentStart), static_cast<int> (segmentEnd - segmentStart)),
                                     segmentStart);

                segmentStart = segmentEnd;
            }
        }

        return output;
    }

    StereoBuffer renderWhole (const StutterParams& params, const StereoBuffer& input,
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

    struct RateMeasurement
    {
        double rate = 0.0;
        int64_t centre = 0; // absolute index the rate was measured around
    };

    /** Playback rate of a ramp read back by the effect, measured over the first window in
        [from, to) that contains no loop point or crossfade. The rate is 0 if there is none. */
    RateMeasurement measureRampRate (const std::vector<float>& output, int64_t from, int64_t to, double rampStep)
    {
        constexpr int64_t window = 32;

        for (int64_t n = from; n < to; ++n)
        {
            const double average = (static_cast<double> (output[at (n + window)]) - output[at (n)]) / window;

            if (average <= 0.0)
                continue;

            bool clean = true;

            for (int64_t i = n; i < n + window && clean; ++i)
            {
                const double difference = static_cast<double> (output[at (i + 1)]) - output[at (i)];
                clean = difference > 0.9 * average && difference < 1.1 * average;
            }

            if (clean)
                return { average / rampStep, n + window / 2 };
        }

        return {};
    }
} // namespace

TEST_CASE ("stutter passes dry audio until triggered, then plays the live input on the first pass")
{
    const auto input = makeNoise (40000, 3);
    const int64_t start = 12000;
    const auto output = renderWhole (makeParams (0.125f), input, { stepAt (start, quarter) });

    CHECK (maxDifference (output, input, 0, start) == 0.0);
    CHECK (maxDifference (output, input, start, start + thirtySecond - crossfade) == 0.0);

    // From the end of the first pass on, it is no longer the live input.
    CHECK (maxDifference (output, input, start + thirtySecond, start + 2 * thirtySecond) > 0.1);
}

TEST_CASE ("stutter loops the slice from the step start until the step ends")
{
    const auto input = makeNoise (40000, 4);
    const int64_t start = 12000;
    const auto output = renderWhole (makeParams (0.125f), input, { stepAt (start, quarter) });

    for (int64_t repeat = 1; repeat < quarter / thirtySecond; ++repeat)
    {
        double worst = 0.0;

        for (int64_t phase = 0; phase < thirtySecond - crossfade; ++phase)
        {
            const auto played = at (start + repeat * thirtySecond + phase);
            const auto original = at (start + phase);
            worst = std::max (worst, static_cast<double> (std::fabs (output.left[played] - input.left[original])));
            worst = std::max (worst, static_cast<double> (std::fabs (output.right[played] - input.right[original])));
        }

        CHECK (worst == 0.0);
    }

    // At the end of each pass the output has crossfaded into the audio just before the
    // slice, which flows into the slice start after the wrap.
    const auto lastOfPass = at (start + 2 * thirtySecond - 1);
    CHECK_NEAR (output.left[lastOfPass], input.left[at (start - 1)], 1.0e-3);
}

TEST_CASE ("stutter slice length follows repeatBeats and is clamped between 32 samples and the step")
{
    const auto input = makeNoise (30000, 5);
    const int64_t start = 12000;

    // 1/64 note = 1500 samples.
    {
        const auto output = renderWhole (makeParams (0.0625f), input, { stepAt (start, sixteenth) });
        CHECK (maxDifference (output, input, start, start + 1500 - crossfade) == 0.0);

        double worst = 0.0;
        for (int64_t phase = 0; phase < 1500 - crossfade; ++phase)
            worst = std::max (worst, std::fabs (static_cast<double> (output.left[at (start + 1500 + phase)])
                                                - input.left[at (start + phase)]));
        CHECK (worst == 0.0);
    }

    // A tiny repeat length still loops 32 samples (with an 8 sample crossfade).
    {
        const auto output = renderWhole (makeParams (1.0e-6f), input, { stepAt (start, sixteenth) });

        for (int64_t repeat = 1; repeat < 10; ++repeat)
        {
            double worst = 0.0;
            for (int64_t phase = 0; phase < 32 - 8; ++phase)
                worst = std::max (worst, std::fabs (static_cast<double> (output.left[at (start + repeat * 32 + phase)])
                                                    - input.left[at (start + phase)]));
            CHECK (worst == 0.0);
        }
    }

    // A repeat longer than the step is the whole step: live input throughout.
    {
        const auto output = renderWhole (makeParams (2.0f), input, { stepAt (start, sixteenth) });
        CHECK (maxDifference (output, input, start, start + sixteenth - crossfade) == 0.0);
    }
}

TEST_CASE ("stutter loop points are crossfaded so a low sine does not click")
{
    const auto input = makeSine (60000, 110.0, sampleRate);
    const int64_t start = 12000;
    const float naturalSlope = maxJump (input.left);

    for (const float repeatBeats : { 0.5f, 0.25f, 0.125f, 0.0625f, 0.03125f })
    {
        const auto slice = static_cast<int64_t> (repeatBeats * samplesPerBeat);
        const auto output = renderWhole (makeParams (repeatBeats), input, { stepAt (start, quarter) });

        // Without the crossfade the wrap would jump straight from the slice end to its start
        // (for some lengths the sine happens to line up, so only check where it does not).
        const float hardCut = std::fabs (input.left[at (start + slice)] - input.left[at (start)]);

        if (repeatBeats == 0.125f)
            CHECK (hardCut > 0.3f);

        CHECK (stereoJump (output, start - 100, start + quarter) < naturalSlope + 0.015f);
    }
}

TEST_CASE ("stutter crossfades never raise the level of a DC input")
{
    const auto input = makeDc (60000, 0.9f);
    const int64_t start = 12000;

    for (const float glide : { -24.0f, -5.0f, 0.0f, 12.0f, 24.0f })
    {
        const auto output = renderWhole (makeParams (0.0625f, glide), input,
                                         { stepAt (start, quarter), stepAt (start + quarter, sixteenth) });

        CHECK (peak (output.left, at (start), at (start + quarter + sixteenth)) <= 0.9f + 1.0e-5f);
        CHECK (maxDifference (output, input, start, start + quarter + sixteenth) < 1.0e-5);
    }
}

TEST_CASE ("stutter pitch glide follows 2^(semitones * progress / 12)")
{
    constexpr double rampStep = 1.0 / 40000.0;
    const auto input = makeRamp (40000, -0.5f, static_cast<float> (rampStep));
    const int64_t start = 12000;

    for (const float glide : { 12.0f, -12.0f, 24.0f, -24.0f, 7.0f })
    {
        const auto output = renderWhole (makeParams (0.125f, glide), input, { stepAt (start, quarter) });

        for (const double progress : { 0.3, 0.5, 0.7, 0.85 })
        {
            const auto from = start + static_cast<int64_t> (progress * quarter);
            const auto measured = measureRampRate (output.left, from, from + 4000, rampStep);
            const double measuredProgress = static_cast<double> (measured.centre - start) / quarter;
            const double expected = std::exp2 (glide * measuredProgress / 12.0);

            CHECK (measured.rate > 0.0);
            CHECK_NEAR (measured.rate, expected, 0.01 * expected);
        }
    }

    // A falling glide already bends the first pass.
    {
        const auto output = renderWhole (makeParams (0.125f, -12.0f), input, { stepAt (start, quarter) });
        const auto measured = measureRampRate (output.left, start + 1200, start + 1300, rampStep);
        CHECK_NEAR (measured.rate, std::exp2 (-static_cast<double> (measured.centre - start) / quarter), 0.005);
    }
}

TEST_CASE ("stutter rising glide holds the first pass at the live input")
{
    const auto input = makeNoise (40000, 6);
    const int64_t start = 12000;

    for (const float glide : { 0.5f, 12.0f, 24.0f })
    {
        const auto output = renderWhole (makeParams (0.125f, glide), input, { stepAt (start, quarter) });
        CHECK (maxDifference (output, input, start, start + thirtySecond - crossfade) == 0.0);
    }
}

TEST_CASE ("stutter never reads ahead of the live input")
{
    const auto inputA = makeNoise (36000, 7);
    const auto inputB = makeNoise (36000, 8);
    const int64_t start = 12000;

    // Contiguous retrigger at 18000, then a late trigger (transport jump) at 26000.
    const std::vector<Trigger> triggers { stepAt (start, sixteenth), stepAt (start + sixteenth, sixteenth),
                                          lateTrigger (26000, 24500, sixteenth) };

    for (const float glide : { -24.0f, -12.0f, -0.5f, 0.0f, 7.0f, 24.0f })
    {
        for (const float repeatBeats : { 0.125f, 0.001f })
        {
            for (const float decay : { 0.0f, 1.0f })
            {
                const auto params = makeParams (repeatBeats, glide, decay);
                const auto outputA = renderWhole (params, inputA, triggers);

                for (const int64_t divergeAt : { start + 1, start + 2, start + 40, start + 2900, start + 3001,
                                                  start + sixteenth + 1, int64_t { 26001 } })
                {
                    const auto mixed = splice (inputA, inputB, divergeAt);
                    const auto outputB = renderWhole (params, mixed, triggers);
                    CHECK (maxDifference (outputA, outputB, 0, divergeAt) == 0.0);
                }
            }
        }
    }
}

TEST_CASE ("stutter decay fades the repeats so the last one is near silent")
{
    const auto input = makeNoise (40000, 9);
    const int64_t start = 12000;
    constexpr int numPasses = static_cast<int> (quarter / thirtySecond);

    auto passLevels = [&] (float decay) {
        const auto output = renderWhole (makeParams (0.125f, 0.0f, decay), input, { stepAt (start, quarter) });
        std::vector<double> levels;

        for (int pass = 0; pass < numPasses; ++pass)
        {
            const auto begin = at (start + pass * thirtySecond);
            levels.push_back (rms (output.left, begin, begin + at (thirtySecond - crossfade)));
        }

        for (auto& level : levels)
            level /= rms (input.left, at (start), at (start + thirtySecond - crossfade));

        return levels;
    };

    const auto none = passLevels (0.0f);
    for (const auto level : none)
        CHECK_NEAR (level, 1.0, 1.0e-6);

    const auto full = passLevels (1.0f);
    CHECK_NEAR (full[0], 1.0, 1.0e-6);
    CHECK (full.back() < 0.002);
    CHECK (full[1] > 0.2); // the fade is spread across the step, not instant

    const auto half = passLevels (0.5f);
    CHECK (half.back() > 0.02 && half.back() < 0.05); // about -30 dB

    for (size_t pass = 1; pass < full.size(); ++pass)
    {
        CHECK (full[pass] < full[pass - 1]);
        CHECK (half[pass] < half[pass - 1]);
        CHECK (half[pass] > full[pass]);
    }

    // Default 1/16 step with 1/32 repeats: the only repeat is the last one.
    {
        const auto output = renderWhole (makeParams (0.125f, 0.0f, 1.0f), input, { stepAt (start, sixteenth) });
        const auto repeatBegin = at (start + thirtySecond);
        CHECK (rms (output.left, repeatBegin, repeatBegin + at (thirtySecond - crossfade))
               < 0.002 * rms (input.left, at (start), at (start + thirtySecond)));
    }

    // With a rising glide there are more, shorter passes, and the step still ends near silent.
    {
        const auto output = renderWhole (makeParams (0.125f, 12.0f, 1.0f), input, { stepAt (start, quarter) });
        const auto tailBegin = at (start + quarter - 2400);
        CHECK (rms (output.left, tailBegin, tailBegin + 2400) < 0.005 * rms (input.left, at (start), at (start + thirtySecond)));
    }
}

TEST_CASE ("stutter retrigger crossfades from the old voice without a click")
{
    const auto input = makeSine (40000, 110.0, sampleRate);
    const float naturalSlope = maxJump (input.left);
    const int64_t first = 12000;
    const int64_t second = first + sixteenth;

    for (const float glide : { 0.0f, -7.0f })
    {
        const auto params = makeParams (0.125f, glide, 0.3f);
        const auto output = renderWhole (params, input,
                                         { stepAt (first, sixteenth), stepAt (second, sixteenth), stepAt (second + sixteenth, sixteenth) });

        CHECK (stereoJump (output, first - 100, second + 2 * sixteenth) < naturalSlope + 0.015f);

        // Once the crossfade is over only the new voice is left.
        const auto fresh = renderWhole (params, input, { stepAt (second, sixteenth) }, { { second, second + sixteenth } });
        CHECK (maxDifference (output, fresh, second + crossfade, second + sixteenth) == 0.0);
        CHECK (maxDifference (output, fresh, second, second + crossfade) > 0.01);
    }
}

TEST_CASE ("stutter does not crossfade from a voice that stopped sounding")
{
    const auto input = makeNoise (40000, 10);
    const auto params = makeParams (0.125f);

    // Stutter, then another source for a while (effect not processed), then stutter again.
    const auto output = renderWhole (params, input, { stepAt (6000, sixteenth), stepAt (24000, sixteenth) },
                                     { { 6000, 12000 + crossfade }, { 24000, 30000 } });
    const auto fresh = renderWhole (params, input, { stepAt (24000, sixteenth) }, { { 24000, 30000 } });

    CHECK (maxDifference (output, fresh, 24000, 30000) == 0.0);
    CHECK (maxDifference (output, input, 24000, 24000 + thirtySecond - crossfade) == 0.0);
}

TEST_CASE ("stutter started mid-step matches the voice that started on time")
{
    const auto input = makeNoise (40000, 11);
    const int64_t start = 12000;

    for (const float glide : { 0.0f, 7.0f, -7.0f })
    {
        const auto params = makeParams (0.125f, glide, 0.5f);
        const auto onTime = renderWhole (params, input, { stepAt (start, quarter) }, { { start, 40000 } });

        for (const int64_t arrival : { start + 1000, start + 4500, start + 20000 })
        {
            const auto late = renderWhole (params, input, { lateTrigger (arrival, start, quarter) }, { { arrival, 40000 } });
            CHECK (maxDifference (late, onTime, arrival, 40000) == 0.0);
        }
    }
}

TEST_CASE ("stutter late trigger while sounding crossfades without a click")
{
    const auto input = makeSine (40000, 110.0, sampleRate);
    const float naturalSlope = maxJump (input.left);
    const auto params = makeParams (0.125f);

    // A transport jump at 15000 lands 1500 samples into a step that "started" at 13500.
    const auto output = renderWhole (params, input, { stepAt (12000, sixteenth), lateTrigger (15000, 13500, sixteenth) });
    CHECK (stereoJump (output, 11000, 22000) < naturalSlope + 0.015f);

    const auto fresh = renderWhole (params, input, { lateTrigger (15000, 13500, sixteenth) }, { { 15000, 40000 } });
    CHECK (maxDifference (output, fresh, 15000 + crossfade, 40000) == 0.0);
}

TEST_CASE ("stutter retrigger inside a crossfade stays smooth")
{
    const auto input = makeSine (40000, 110.0, sampleRate);
    const float naturalSlope = maxJump (input.left);
    const auto params = makeParams (0.125f, 0.0f, 0.2f);

    // Two transport jumps within one crossfade length: three voices overlap.
    const std::vector<Trigger> triggers { stepAt (12000, sixteenth), lateTrigger (15000, 13500, sixteenth),
                                          lateTrigger (15050, 12100, sixteenth) };
    const auto output = renderWhole (params, input, triggers);

    // Two crossfades overlap, so allow for both slopes.
    CHECK (stereoJump (output, 11000, 22000) < naturalSlope + 0.025f);

    const auto fresh = renderWhole (params, input, { lateTrigger (15050, 12100, sixteenth) }, { { 15050, 40000 } });
    CHECK (maxDifference (output, fresh, 15050 + crossfade, 40000) == 0.0);
}

TEST_CASE ("stutter output does not depend on the block size")
{
    const auto input = makeNoise (48000, 12);
    const std::vector<Trigger> triggers { stepAt (6000, sixteenth), stepAt (12000, sixteenth), lateTrigger (20000, 19000, sixteenth),
                                          stepAt (30000, 2 * sixteenth) };

    for (const auto& params : { makeParams (0.0625f, 7.0f, 0.6f), makeParams (0.125f, -12.0f, 1.0f), makeParams (0.03125f, 24.0f, 0.0f) })
    {
        for (const auto& active : { std::vector<Range> {}, std::vector<Range> { { 6000, 18000 }, { 20000, 26000 }, { 30000, 48000 } } })
        {
            const auto reference = renderWhole (params, input, triggers, active);

            for (const int blockSize : { 1, 7, 64, 512 })
            {
                const auto output = render (params, input, blockSize, triggers, active);
                CHECK (maxDifference (output, reference, 0, 48000) <= 1.0e-6);
            }
        }
    }
}

TEST_CASE ("stutter keeps looping smoothly past the step end")
{
    // The engine may keep rendering the old effect while it fades it out.
    const auto input = makeSine (40000, 110.0, sampleRate);
    const float naturalSlope = maxJump (input.left);

    for (const float glide : { 0.0f, -12.0f, 12.0f })
    {
        const auto output = renderWhole (makeParams (0.125f, glide), input, { stepAt (12000, sixteenth) });
        CHECK (allFinite (output.left) && allFinite (output.right));
        CHECK (stereoJump (output, 12000, 30000) < 2.0f * naturalSlope + 0.03f);
        CHECK (peak (output.left, 12000, 30000) <= 0.5f + 1.0e-4f);
    }
}

TEST_CASE ("stutter output stays finite and bounded for any input and parameters")
{
    constexpr int length = 30000;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();

    std::vector<StereoBuffer> inputs { StereoBuffer (length), makeDc (length, 1.0f), makeDc (length, -1.0f),
                                       makeNoise (length, 13, 1.0f), makeDc (length, 1.0e-40f) };

    const std::vector<StutterParams> paramSets {
        makeParams (0.125f),
        makeParams (0.03125f, 24.0f, 1.0f),
        makeParams (0.5f, -24.0f, 1.0f),
        makeParams (1.0e-9f, 24.0f, 0.5f),
        makeParams (64.0f, -24.0f, 0.0f),
        makeParams (-1.0f, 1000.0f, 7.0f),
        makeParams (nan, nan, nan),
        makeParams (inf, -inf, -inf),
        makeParams (0.125f, -1000.0f, -3.0f),
    };

    const std::vector<std::vector<Trigger>> schedules {
        { stepAt (5000, sixteenth), stepAt (11000, sixteenth) },
        { stepAt (0, 1), stepAt (1, 7), stepAt (8, 31), stepAt (39, 32), stepAt (71, 33), stepAt (104, 3) },
        { { 5000, makeStep (5000, sixteenth, 0.0) }, { 12000, makeStep (12000, sixteenth, std::nan ("")) },
          { 20000, makeStep (20000, sixteenth, 1.0e12) } },
        { lateTrigger (9000, -4000, quarter), lateTrigger (15000, 14000, 0) },
    };

    for (const auto& input : inputs)
    {
        const float inputPeak = peak (input.left);

        for (const auto& params : paramSets)
        {
            for (const auto& triggers : schedules)
            {
                const auto output = render (params, input, 256, triggers);

                CHECK (allFinite (output.left) && allFinite (output.right));
                CHECK (peak (output.left) <= 1.5f * inputPeak);
                CHECK (peak (output.right) <= 1.5f * inputPeak);
            }
        }
    }
}

TEST_CASE ("stutter handles a four second step")
{
    const int64_t longStep = 4 * 48000;
    const auto input = makeNoise (static_cast<int> (longStep + 20000), 14);

    for (const float glide : { -24.0f, 0.0f, 24.0f })
    {
        const auto output = render (makeParams (0.5f, glide, 1.0f), input, 512, { stepAt (10000, longStep) });
        CHECK (allFinite (output.left) && allFinite (output.right));
        CHECK (peak (output.left) <= 1.5f * peak (input.left));

        if (glide >= 0.0f)
            CHECK (maxDifference (output, input, 10000, 10000 + 12000 - crossfade) == 0.0);
    }
}

TEST_CASE ("stutter reset drops the voice and returns to dry")
{
    const auto input = makeNoise (20000, 15);
    const auto ramp = makeRamp (20000);

    Stutter stutter;
    stutter.prepare (sampleRate, 10000);
    stutter.setParams (makeParams (0.125f));

    CaptureBuffer capture;
    capture.prepare (captureCapacity);

    capture.write (input.left.data(), input.right.data(), 10000);
    stutter.trigger (makeStep (0, sixteenth));
    auto first = input;
    stutter.process (capture, first.block (0, 10000), 0);
    CHECK (maxDifference (first, input, thirtySecond, 2 * thirtySecond - crossfade) > 0.1);

    stutter.reset();
    capture.reset();

    capture.write (ramp.left.data(), ramp.right.data(), 512);
    auto second = ramp;
    stutter.process (capture, second.block (0, 512), 0);
    CHECK (maxDifference (second, ramp, 0, 512) == 0.0);
}
