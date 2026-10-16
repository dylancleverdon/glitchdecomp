#include "TestFramework.h"
#include "TestSignals.h"

#include "dsp/CaptureBuffer.h"
#include "dsp/effects/TapeStop.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

using namespace splinter;
using namespace splinter::test;

namespace
{
constexpr double sampleRate = 44100.0;
constexpr int crossfadeSamples = 132; // TapeStop::crossfadeSeconds at 44.1 kHz

struct TriggerEvent
{
    int64_t at = 0; // absolute index at which trigger() is called
    StepContext step;
};

StepContext makeStep (int64_t start, int64_t length)
{
    StepContext step;
    step.stepStart = start;
    step.stepLength = length;
    step.sampleRate = sampleRate;
    step.samplesPerBeat = sampleRate * 0.5;
    return step;
}

TriggerEvent triggerAtStart (int64_t start, int64_t length)
{
    return { start, makeStep (start, length) };
}

/** Drives the effect the way the engine does: blocks of at most blockSize, split at
    trigger points, each appended to the capture buffer before it is processed. */
StereoBuffer renderTapeStop (const TapeStopParams& params, const StereoBuffer& input,
                             const std::vector<TriggerEvent>& triggers, int blockSize = 512)
{
    TapeStop effect;
    effect.prepare (sampleRate, blockSize);
    effect.setParams (params);

    CaptureBuffer capture;
    capture.prepare (input.size() + 16);

    auto output = input;
    const auto total = static_cast<int64_t> (input.size());
    size_t nextTrigger = 0;
    int64_t position = 0;

    while (position < total)
    {
        while (nextTrigger < triggers.size() && triggers[nextTrigger].at <= position)
            effect.trigger (triggers[nextTrigger++].step);

        auto length = std::min<int64_t> (blockSize, total - position);

        if (nextTrigger < triggers.size())
            length = std::min (length, triggers[nextTrigger].at - position);

        auto block = output.block (static_cast<int> (position), static_cast<int> (length));
        capture.write (block.left, block.right, block.numSamples);
        effect.process (capture, block, position);
        position += length;
    }

    return output;
}

TapeStopParams makeParams (float duration, float curve)
{
    TapeStopParams params;
    params.duration = duration;
    params.curve = curve;
    return params;
}

StereoBuffer makeConstant (int numSamples, float value)
{
    StereoBuffer buffer (numSamples);
    std::fill (buffer.left.begin(), buffer.left.end(), value);
    std::fill (buffer.right.begin(), buffer.right.end(), value);
    return buffer;
}

int countRisingZeroCrossings (const std::vector<float>& samples, size_t begin, size_t end)
{
    int count = 0;

    for (size_t i = begin + 1; i < end; ++i)
        if (samples[i - 1] < 0.0f && samples[i] >= 0.0f)
            ++count;

    return count;
}

float maxDifference (const StereoBuffer& a, const StereoBuffer& b, size_t begin = 0, size_t end = SIZE_MAX)
{
    float result = 0.0f;
    end = std::min ({ end, a.left.size(), b.left.size() });

    for (size_t i = begin; i < end; ++i)
        result = std::max ({ result, std::fabs (a.left[i] - b.left[i]), std::fabs (a.right[i] - b.right[i]) });

    return result;
}

bool identicalBefore (const StereoBuffer& a, const StereoBuffer& b, size_t end)
{
    for (size_t i = 0; i < end; ++i)
        if (a.left[i] != b.left[i] || a.right[i] != b.right[i])
            return false;

    return true;
}

/** Integral of TapeStop::speedAt from 0 to `progress`, by the trapezoid rule. */
double integrateSpeed (double progress, double curve, int numSteps = 20000)
{
    const auto stepSize = progress / numSteps;
    double sum = 0.0;

    for (int i = 0; i < numSteps; ++i)
        sum += 0.5 * stepSize * (TapeStop::speedAt (i * stepSize, curve) + TapeStop::speedAt ((i + 1) * stepSize, curve));

    return sum;
}
} // namespace

TEST_CASE ("tape stop speed curve falls from 1 to 0 and bends towards exponential")
{
    const double curves[] = { 0.0, 0.25, 0.5, 0.75, 1.0 };

    for (auto curve : curves)
    {
        CHECK_NEAR (TapeStop::speedAt (0.0, curve), 1.0, 1e-12);
        CHECK_NEAR (TapeStop::speedAt (1.0, curve), 0.0, 1e-12);
        CHECK_NEAR (TapeStop::speedAt (-0.5, curve), 1.0, 1e-12); // full speed before the stop
        CHECK_NEAR (TapeStop::speedAt (2.0, curve), 0.0, 1e-12);  // stopped afterwards

        auto previous = 1.0;

        for (int i = 1; i <= 1000; ++i)
        {
            const auto speed = TapeStop::speedAt (i / 1000.0, curve);
            CHECK (speed <= previous);
            CHECK (speed >= 0.0);
            previous = speed;
        }
    }

    // curve 0 is a linear ramp
    for (int i = 0; i <= 10; ++i)
        CHECK_NEAR (TapeStop::speedAt (i / 10.0, 0.0), 1.0 - i / 10.0, 1e-12);

    for (int i = 1; i < 20; ++i)
    {
        const auto progress = i / 20.0;

        // a higher curve slows down sooner
        for (int c = 0; c < 10; ++c)
            CHECK (TapeStop::speedAt (progress, (c + 1) / 10.0) < TapeStop::speedAt (progress, c / 10.0));

        // and it moves smoothly between linear and exponential
        for (int c = 0; c < 100; ++c)
            CHECK (std::fabs (TapeStop::speedAt (progress, (c + 1) / 100.0) - TapeStop::speedAt (progress, c / 100.0)) < 0.02);

        CHECK_NEAR (TapeStop::speedAt (progress, 1.0e-9), 1.0 - progress, 1e-6);
    }

    // curve 1 is strongly exponential: a quick dive to a low speed
    CHECK (TapeStop::speedAt (0.2, 1.0) < 0.4);
    CHECK (TapeStop::speedAt (0.5, 1.0) < 0.1);

    // out-of-range curves are clamped
    CHECK_NEAR (TapeStop::speedAt (0.3, -1.0), TapeStop::speedAt (0.3, 0.0), 1e-12);
    CHECK_NEAR (TapeStop::speedAt (0.3, 7.0), TapeStop::speedAt (0.3, 1.0), 1e-12);
    CHECK (std::isfinite (TapeStop::speedAt (0.3, std::nan (""))));
}

TEST_CASE ("tape stop distance is the integral of the speed curve")
{
    const double curves[] = { 0.0, 1.0e-7, 0.1, 0.5, 0.9, 1.0 };

    for (auto curve : curves)
    {
        for (int i = 1; i <= 20; ++i)
        {
            const auto progress = i / 20.0;
            CHECK_NEAR (TapeStop::distanceAt (progress, curve), integrateSpeed (progress, curve), 1e-8);
        }

        CHECK_NEAR (TapeStop::distanceAt (0.0, curve), 0.0, 1e-15);
        CHECK_NEAR (TapeStop::distanceAt (3.0, curve), TapeStop::distanceAt (1.0, curve), 1e-15);
        CHECK_NEAR (TapeStop::distanceAt (-0.25, curve), -0.25, 1e-15);
    }

    // linear ramp: p - p^2 / 2, so a full stop covers half its length
    CHECK_NEAR (TapeStop::distanceAt (1.0, 0.0), 0.5, 1e-12);
    CHECK_NEAR (TapeStop::distanceAt (0.5, 0.0), 0.375, 1e-12);

    // the exponential curve covers less ground
    CHECK (TapeStop::distanceAt (1.0, 1.0) < 0.25);
}

TEST_CASE ("tape stop head travels the integral of the speed curve")
{
    // Left is a ramp, so the output value tells the read position. Right is DC, so it
    // tells the gain. Cubic Hermite interpolation reproduces a ramp exactly.
    constexpr int stepStart = 3000;
    constexpr int stepLength = 24000;
    constexpr float duration = 0.8f;
    constexpr double stopLength = static_cast<double> (duration) * stepLength;
    constexpr float rampScale = 1.0f / 32768.0f;

    StereoBuffer input (stepStart + stepLength + 2000);

    for (int i = 0; i < input.size(); ++i)
    {
        input.left[static_cast<size_t> (i)] = static_cast<float> (i) * rampScale;
        input.right[static_cast<size_t> (i)] = 0.5f;
    }

    const float curves[] = { 0.0f, 0.5f, 1.0f };

    for (auto curve : curves)
    {
        const auto output = renderTapeStop (makeParams (duration, curve), input, { triggerAtStart (stepStart, stepLength) });

        // Integrate the speed sample by sample, independently of TapeStop::distanceAt.
        double travelled = 0.0;
        double lastMeasured = 0.0;
        double lastExpected = 0.0;
        int checkedSamples = 0;

        for (int elapsed = 0; elapsed < static_cast<int> (stopLength); ++elapsed)
        {
            if (elapsed > 0)
            {
                constexpr int substeps = 8;

                for (int s = 0; s < substeps; ++s)
                {
                    const auto from = (elapsed - 1 + s / static_cast<double> (substeps)) / stopLength;
                    const auto to = (elapsed - 1 + (s + 1) / static_cast<double> (substeps)) / stopLength;
                    travelled += 0.5 * (TapeStop::speedAt (from, curve) + TapeStop::speedAt (to, curve)) / substeps;
                }
            }

            const auto index = static_cast<size_t> (stepStart + elapsed);
            const auto gain = output.right[index] / 0.5f;

            // the gain is exact (DC times 0.5), so even a tiny one still tells the position
            if (gain < 1.0e-6f)
                continue;

            const auto measured = static_cast<double> (output.left[index] / gain) / rampScale;
            const auto expected = stepStart - 1 + travelled;
            CHECK_NEAR (measured, expected, 0.05);

            if (curve == 0.0f)
            {
                const auto progress = elapsed / stopLength;
                CHECK_NEAR (measured, stepStart - 1 + stopLength * (progress - 0.5 * progress * progress), 0.05);
            }

            lastMeasured = measured;
            lastExpected = expected;
            ++checkedSamples;
        }

        CHECK (checkedSamples > static_cast<int> (0.99 * stopLength));
        CHECK_NEAR (lastMeasured, lastExpected, 0.05);

        // Total distance: the head stops where the integral of the whole curve says.
        const auto totalDistance = stopLength * integrateSpeed (1.0, curve);
        CHECK_NEAR (lastMeasured, stepStart - 1 + totalDistance, 0.05);

        if (curve == 0.0f)
            CHECK_NEAR (totalDistance, 0.5 * stopLength, 1e-6);
    }
}

TEST_CASE ("tape stop starts at the live input")
{
    auto input = makeNoise (8000, 3);
    const auto output = renderTapeStop (makeParams (1.0f, 0.5f), input, { triggerAtStart (2000, 4000) });

    // Before the step the dry input is untouched; the stop begins one sample behind it.
    CHECK (identicalBefore (output, input, 2000));
    CHECK (output.left[2000] == input.left[1999]);
    CHECK (output.right[2000] == input.right[1999]);
    CHECK_NEAR (output.left[2001], input.left[2000], 0.01);
    CHECK_NEAR (output.right[2001], input.right[2000], 0.01);
}

TEST_CASE ("tape stop pitch falls with the speed")
{
    constexpr int stepStart = 1000;
    constexpr int stepLength = 44100;
    constexpr double toneHz = 2000.0;
    constexpr int window = 2205; // 50 ms

    const auto input = makeSine (stepStart + stepLength, toneHz, sampleRate);
    const float curves[] = { 0.0f, 1.0f };

    for (auto curve : curves)
    {
        const auto output = renderTapeStop (makeParams (1.0f, curve), input, { triggerAtStart (stepStart, stepLength) });
        int previousCrossings = std::numeric_limits<int>::max();

        for (const auto centre : { 0.1, 0.3, 0.5, 0.7 })
        {
            const auto begin = static_cast<size_t> (stepStart + centre * stepLength - window / 2);
            const auto crossings = countRisingZeroCrossings (output.left, begin, begin + window);
            const auto expected = toneHz * TapeStop::speedAt (centre, curve) * window / sampleRate;

            CHECK_NEAR (crossings, expected, 2.0 + 0.05 * expected);
            CHECK (crossings <= previousCrossings);
            previousCrossings = crossings;
        }
    }
}

TEST_CASE ("tape stop fades out at the end of the stop and is silent until the step ends")
{
    constexpr int stepStart = 2000;
    constexpr int stepLength = 20000;
    constexpr int stopLength = 10000; // duration 0.5
    constexpr int fadeStart = static_cast<int> (stopLength * (1.0 - TapeStop::fadeFraction));

    // DC shows the gain directly: full level, then a smooth fade that ends at the stop.
    const auto dc = makeConstant (stepStart + stepLength + 3000, 0.5f);
    const auto dcOutput = renderTapeStop (makeParams (0.5f, 0.5f), dc, { triggerAtStart (stepStart, stepLength) });

    for (int i = stepStart; i <= stepStart + fadeStart; ++i)
        CHECK_NEAR (dcOutput.left[static_cast<size_t> (i)], 0.5, 1e-6);

    for (int i = stepStart + fadeStart + 1; i < stepStart + stopLength; ++i)
        CHECK (dcOutput.left[static_cast<size_t> (i)] <= dcOutput.left[static_cast<size_t> (i - 1)]);

    CHECK (dcOutput.left[static_cast<size_t> (stepStart + stopLength - 1)] < 1.0e-5f);
    CHECK (maxJump (dcOutput.left, stepStart, stepStart + stepLength) < 0.5f * 3.1416f / (2.0f * (stopLength - fadeStart)) * 1.01f);

    // Silence (exact zeros) from the stop to the end of the step, and until the next trigger.
    const auto noise = makeNoise (stepStart + stepLength + 3000, 9);
    const auto noiseOutput = renderTapeStop (makeParams (0.5f, 0.5f), noise, { triggerAtStart (stepStart, stepLength) });

    CHECK (rms (noiseOutput.left, stepStart, stepStart + fadeStart) > 0.2);
    CHECK (peak (noiseOutput.left, stepStart + stopLength, noiseOutput.left.size()) == 0.0f);
    CHECK (peak (noiseOutput.right, stepStart + stopLength, noiseOutput.right.size()) == 0.0f);

    // A full-length stop is silent only at the very end of its step.
    const auto fullOutput = renderTapeStop (makeParams (1.0f, 0.0f), noise, { triggerAtStart (stepStart, stepLength) });
    CHECK (peak (fullOutput.left, stepStart + stepLength - 50, stepStart + stepLength) < 1.0e-3f);
    CHECK (peak (fullOutput.left, stepStart + stepLength, fullOutput.left.size()) == 0.0f);
}

TEST_CASE ("tape stop is causal")
{
    constexpr int length = 20000;
    const auto inputA = makeNoise (length, 21);
    const auto later = makeNoise (length, 22);

    struct Scenario
    {
        float duration;
        float curve;
        std::vector<TriggerEvent> triggers;
        int changeAt;
    };

    const Scenario scenarios[] = {
        { 1.0f, 0.0f, { triggerAtStart (1000, 8000) }, 3500 },
        { 1.0f, 1.0f, { triggerAtStart (1000, 8000) }, 1001 },
        { 0.05f, 0.5f, { triggerAtStart (1000, 8000) }, 1200 },
        { 0.7f, 0.3f, { triggerAtStart (1000, 4000), triggerAtStart (5000, 4000) }, 5050 },
        { 1.0f, 0.5f, { triggerAtStart (1000, 6000), { 4000, makeStep (2500, 6000) } }, 4060 }, // transport jump
    };

    for (const auto& scenario : scenarios)
    {
        auto inputB = inputA;

        for (int i = scenario.changeAt; i < length; ++i)
        {
            inputB.left[static_cast<size_t> (i)] = later.left[static_cast<size_t> (i)];
            inputB.right[static_cast<size_t> (i)] = later.right[static_cast<size_t> (i)];
        }

        const auto params = makeParams (scenario.duration, scenario.curve);
        const auto outputA = renderTapeStop (params, inputA, scenario.triggers, 64);
        const auto outputB = renderTapeStop (params, inputB, scenario.triggers, 64);

        CHECK (identicalBefore (outputA, outputB, static_cast<size_t> (scenario.changeAt)));

        if (scenario.duration == 1.0f)
            CHECK (maxDifference (outputA, outputB, static_cast<size_t> (scenario.changeAt)) > 0.01f);
    }
}

TEST_CASE ("tape stop does not click at the stop end or across retriggers")
{
    // A 30 Hz sine moves at most ~0.0021 per sample; a hard cut would jump by up to 0.5.
    constexpr int stepStart = 4410;
    constexpr int stepLength = 11025;
    const auto input = makeSine (stepStart + 4 * stepLength, 30.0, sampleRate);
    const auto inputJump = maxJump (input.left);

    const std::vector<TriggerEvent> triggers = {
        triggerAtStart (stepStart, stepLength),
        triggerAtStart (stepStart + stepLength, stepLength),
        triggerAtStart (stepStart + 2 * stepLength, stepLength),
    };

    for (auto duration : { 1.0f, 0.5f, 0.2f })
    {
        for (auto curve : { 0.0f, 0.5f, 1.0f })
        {
            const auto output = renderTapeStop (makeParams (duration, curve), input, triggers);
            CHECK (maxJump (output.left) < 0.012f);
            CHECK (maxJump (output.right) < 0.012f);
            CHECK (maxJump (output.left) > inputJump * 0.5f); // it did play something
        }
    }

    // A retrigger while the old stop is still sounding (a transport jump) is crossfaded too.
    const std::vector<TriggerEvent> jump = {
        triggerAtStart (stepStart, 3 * stepLength),
        { stepStart + 7000, makeStep (stepStart + 2000, 3 * stepLength) },
    };

    // both voices are sounding here, so the fade itself may move by up to ~0.0118 per sample
    const auto jumpOutput = renderTapeStop (makeParams (1.0f, 0.5f), input, jump);
    CHECK (maxJump (jumpOutput.left) < 0.016f);
}

TEST_CASE ("tape stop output is independent of block size")
{
    constexpr int length = 24000;
    auto input = makeNoise (length, 5, 0.3f);
    const auto tone = makeSine (length, 220.0, sampleRate, 0.4f);

    for (size_t i = 0; i < input.left.size(); ++i)
    {
        input.left[i] += tone.left[i];
        input.right[i] -= tone.right[i];
    }

    const std::vector<TriggerEvent> triggers = {
        triggerAtStart (1000, 6000),
        triggerAtStart (7000, 6000),                 // consecutive step
        { 10000, makeStep (8500, 6000) },            // transport jump while sounding
        triggerAtStart (14500, 3001),
        triggerAtStart (17501, 1),                   // degenerate one-sample step
        triggerAtStart (17502, 5000),
    };

    for (auto curve : { 0.0f, 0.6f, 1.0f })
    {
        const auto params = makeParams (0.7f, curve);
        const auto reference = renderTapeStop (params, input, triggers, length);

        for (int blockSize : { 1, 7, 64, 512 })
            CHECK (maxDifference (renderTapeStop (params, input, triggers, blockSize), reference) == 0.0f);
    }
}

TEST_CASE ("tape stop picks up mid-stop when the step started in the past")
{
    constexpr int length = 16000;
    const auto input = makeNoise (length, 13);
    const auto params = makeParams (0.9f, 0.4f);

    // The host jumped into the middle of a step: trigger() arrives at 6000 for a step that began at 4000.
    const auto jumped = renderTapeStop (params, input, { { 6000, makeStep (4000, 8000) } });
    const auto reference = renderTapeStop (params, input, { triggerAtStart (4000, 8000) });

    CHECK (identicalBefore (jumped, input, 6000));
    CHECK (maxDifference (jumped, reference, 6000) == 0.0f);
    CHECK (rms (jumped.left, 6000, 8000) > 0.1);
}

TEST_CASE ("tape stop crossfades when retriggered while sounding")
{
    constexpr int stepStart = 2000;
    constexpr int retriggerAt = 7000;
    constexpr int length = 20000;
    const auto input = makeNoise (length, 17);
    const auto params = makeParams (1.0f, 0.3f);

    // The old stop is still sounding at 7000 when a jump retriggers it at a new step start.
    const auto oldOnly = renderTapeStop (params, input, { triggerAtStart (stepStart, 12000) });
    const auto newOnly = renderTapeStop (params, input, { triggerAtStart (retriggerAt, 8000) });
    const auto retriggered = renderTapeStop (params, input, { triggerAtStart (stepStart, 12000), triggerAtStart (retriggerAt, 8000) });

    CHECK (identicalBefore (retriggered, oldOnly, retriggerAt));
    CHECK (maxDifference (retriggered, newOnly, retriggerAt + crossfadeSamples) == 0.0f);

    for (size_t i = retriggerAt; i < retriggerAt + crossfadeSamples; ++i)
    {
        const auto low = std::min (oldOnly.left[i], newOnly.left[i]) - 1.0e-6f;
        const auto high = std::max (oldOnly.left[i], newOnly.left[i]) + 1.0e-6f;
        CHECK (retriggered.left[i] >= low && retriggered.left[i] <= high);
    }

    // The first sample after the retrigger is still almost all old stop.
    CHECK (std::fabs (retriggered.left[retriggerAt] - oldOnly.left[retriggerAt])
           <= 0.01f * std::fabs (newOnly.left[retriggerAt] - oldOnly.left[retriggerAt]) + 1.0e-6f);

    // Not crossfaded when another source played in between: the engine fades then.
    {
        TapeStop effect;
        effect.prepare (sampleRate, 500);
        effect.setParams (params);

        CaptureBuffer capture;
        capture.prepare (length);

        auto afterGap = input;

        for (int position = 0; position < length; position += 500)
        {
            if (position == stepStart)
                effect.trigger (makeStep (stepStart, 3000));

            if (position == retriggerAt)
                effect.trigger (makeStep (retriggerAt, 8000));

            auto block = afterGap.block (position, 500);
            capture.write (block.left, block.right, 500);

            if (position < stepStart + 3000 || position >= retriggerAt)
                effect.process (capture, block, position);
        }

        CHECK (maxDifference (afterGap, newOnly, retriggerAt) == 0.0f);
    }

    // Consecutive steps: the old stop has gone silent, so the new one fades in from zero.
    const auto consecutive = renderTapeStop (params, input, { triggerAtStart (stepStart, 5000), triggerAtStart (stepStart + 5000, 5000) });
    CHECK (std::fabs (consecutive.left[stepStart + 5000]) < 0.01f);
    CHECK (maxDifference (consecutive, renderTapeStop (params, input, { triggerAtStart (stepStart + 5000, 5000) }),
                          stepStart + 5000 + crossfadeSamples)
           == 0.0f);
}

TEST_CASE ("tape stop reads its parameters at trigger time")
{
    constexpr int length = 12000;
    const auto input = makeNoise (length, 31);

    TapeStop effect;
    effect.prepare (sampleRate, 512);
    effect.setParams (makeParams (1.0f, 0.0f));

    CaptureBuffer capture;
    capture.prepare (length);

    auto output = input;
    effect.trigger (makeStep (1000, 4000));

    for (int position = 0; position < length; position += 500)
    {
        auto block = output.block (position, 500);
        capture.write (block.left, block.right, 500);
        effect.process (capture, block, position);

        if (position == 2000)
            effect.setParams (makeParams (0.05f, 1.0f)); // mid-step change: heard from the next step

        if (position + 500 == 5000)
            effect.trigger (makeStep (5000, 4000));
    }

    const auto reference = renderTapeStop (makeParams (1.0f, 0.0f), input, { triggerAtStart (1000, 4000) });
    CHECK (maxDifference (output, reference, 1000, 5000) == 0.0f);

    // The second step uses the short stop: silent from 5000 + 0.05 * 4000 (0.05f is a hair over 0.05).
    CHECK (peak (output.left, 5201, 9000) == 0.0f);
    CHECK (rms (output.left, 5000, 5150) > 0.02);
}

TEST_CASE ("tape stop stays finite for extreme parameters and inputs")
{
    constexpr int length = 6000;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();

    std::vector<StereoBuffer> inputs;
    inputs.push_back (makeConstant (length, 0.0f));
    inputs.push_back (makeConstant (length, 1.0f));
    inputs.push_back (makeConstant (length, -1.0f));
    inputs.push_back (makeNoise (length, 77, 1.0f));
    inputs.push_back (makeConstant (length, 1.0e-40f)); // denormal

    const float durations[] = { nan, -1.0f, 0.0f, 0.05f, 0.5f, 1.0f, 100.0f, inf, -inf };
    const float curves[] = { nan, -1.0f, 0.0f, 0.5f, 1.0f, 50.0f, inf };
    const int64_t stepLengths[] = { 1, 2, 100, 44100, 0, -5 };

    for (const auto& input : inputs)
    {
        const auto inputPeak = std::max (peak (input.left), peak (input.right));

        for (auto stepLength : stepLengths)
        {
            std::vector<TriggerEvent> triggers;
            const auto spacing = std::max<int64_t> (1, std::min<int64_t> (stepLength, 2000));

            for (int64_t start = 100; start < length; start += spacing)
                triggers.push_back ({ start, makeStep (start, stepLength) });

            for (auto duration : durations)
            {
                for (auto curve : curves)
                {
                    const auto output = renderTapeStop (makeParams (duration, curve), input, triggers);
                    CHECK (allFinite (output.left) && allFinite (output.right));
                    CHECK (std::max (peak (output.left), peak (output.right)) <= 1.26f * inputPeak); // cubic Hermite overshoot
                }
            }
        }
    }
}

TEST_CASE ("tape stop leaves the input alone until triggered")
{
    const auto input = makeNoise (3000, 41);
    const auto output = renderTapeStop (makeParams (1.0f, 0.5f), input, {});
    CHECK (maxDifference (output, input) == 0.0f);

    // reset() forgets the stop
    TapeStop effect;
    effect.prepare (sampleRate, 512);

    CaptureBuffer capture;
    capture.prepare (4096);

    auto buffer = input;
    effect.trigger (makeStep (0, 1000));
    capture.write (buffer.left.data(), buffer.right.data(), 1000);
    effect.process (capture, buffer.block (0, 1000), 0);
    CHECK (maxDifference (buffer, input, 0, 1000) > 0.0f);

    effect.reset();
    capture.write (buffer.left.data() + 1000, buffer.right.data() + 1000, 1000);
    effect.process (capture, buffer.block (1000, 1000), 1000);
    CHECK (maxDifference (buffer, input, 1000, 2000) == 0.0f);
}
