#include "TestFramework.h"
#include "TestSignals.h"

#include "dsp/CaptureBuffer.h"
#include "dsp/effects/Stretch.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

using namespace splinter;
using namespace splinter::test;

namespace
{
constexpr double sampleRate = 44100.0;
constexpr double twoPi = 6.283185307179586;
constexpr int crossfadeSamples = 132; // Stretch::crossfadeSeconds at 44.1 kHz

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
StereoBuffer renderStretch (const StretchParams& params, const StereoBuffer& input,
                            const std::vector<TriggerEvent>& triggers, int blockSize = 512)
{
    Stretch effect;
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

StretchParams makeParams (float factor, float grainMs)
{
    StretchParams params;
    params.factor = factor;
    params.grainMs = grainMs;
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

/** Power of one frequency in samples[begin, end) (Goertzel), optionally Hann windowed. */
double tonePower (const std::vector<float>& samples, size_t begin, size_t end, double frequency, bool windowed = false)
{
    const auto coefficient = 2.0 * std::cos (twoPi * frequency / sampleRate);
    const auto length = static_cast<double> (end - begin);
    double previous = 0.0, beforePrevious = 0.0;

    for (size_t i = begin; i < end; ++i)
    {
        const auto window = windowed ? 0.5 - 0.5 * std::cos (twoPi * static_cast<double> (i - begin) / length) : 1.0;
        const auto current = window * samples[i] + coefficient * previous - beforePrevious;
        beforePrevious = previous;
        previous = current;
    }

    return previous * previous + beforePrevious * beforePrevious - coefficient * previous * beforePrevious;
}

double bandPower (const std::vector<float>& samples, size_t begin, size_t end, double lowHz, double highHz)
{
    double sum = 0.0;

    for (auto frequency = lowHz; frequency <= highHz; frequency += 1.0)
        sum += tonePower (samples, begin, end, frequency);

    return sum;
}

double strongestFrequency (const std::vector<float>& samples, size_t begin, size_t end, double lowHz, double highHz)
{
    double best = lowHz, bestPower = -1.0;

    for (auto frequency = lowHz; frequency <= highHz; frequency += 0.5)
    {
        const auto power = tonePower (samples, begin, end, frequency, true);

        if (power > bestPower)
        {
            bestPower = power;
            best = frequency;
        }
    }

    return best;
}

/** Hop the effect uses for a grain size on a step (grains are capped at half the step). */
int64_t expectedHop (double grainMs, int64_t stepLength)
{
    const auto grainSamples = std::min (grainMs * 0.001 * sampleRate, 0.5 * static_cast<double> (stepLength));
    return std::max<int64_t> (1, static_cast<int64_t> (0.5 * grainSamples));
}
} // namespace

TEST_CASE ("stretch at factor 1 reproduces the input exactly")
{
    const auto input = makeNoise (30000, 4);

    for (auto grainMs : { 10.0f, 80.0f, 250.0f })
    {
        const auto output = renderStretch (makeParams (1.0f, grainMs), input,
                                           { triggerAtStart (1000, 12000), triggerAtStart (13000, 12000) });
        CHECK (maxDifference (output, input) == 0.0f);
    }
}

TEST_CASE ("stretch plays the dry input for the first half grain")
{
    // Grains that would have started before the step read the live input, so the step
    // opens on the dry signal (no fade-in dip) for any factor.
    constexpr int stepStart = 3000;
    const auto input = makeNoise (20000, 6);

    for (auto factor : { 1.5f, 4.0f, 16.0f })
    {
        for (auto grainMs : { 10.0f, 80.0f })
        {
            const auto output = renderStretch (makeParams (factor, grainMs), input, { triggerAtStart (stepStart, 15000) });
            const auto hop = static_cast<size_t> (expectedHop (grainMs, 15000));

            CHECK (identicalBefore (output, input, stepStart + hop));
            CHECK (maxDifference (output, input, stepStart + hop, stepStart + 15000) > 0.1f);
        }
    }
}

TEST_CASE ("stretch is causal")
{
    constexpr int length = 30000;
    const auto inputA = makeNoise (length, 51);
    const auto later = makeNoise (length, 52);

    struct Scenario
    {
        std::vector<TriggerEvent> triggers;
        int changeAt;
        bool changeInFirstHop; // the first grain plays the live input there, so the outputs must differ
    };

    const Scenario scenarios[] = {
        { { triggerAtStart (1000, 20000) }, 1001, true },
        { { triggerAtStart (1000, 20000) }, 4000, false },
        { { triggerAtStart (1000, 20000) }, 15000, false },
        { { triggerAtStart (1000, 8000), triggerAtStart (9000, 8000) }, 9050, true },
        { { triggerAtStart (1000, 12000), { 7000, makeStep (4000, 12000) } }, 7030, false }, // transport jump
    };

    for (auto factor : { 1.001f, 1.5f, 4.0f, 16.0f })
    {
        for (auto grainMs : { 10.0f, 80.0f, 250.0f })
        {
            for (const auto& scenario : scenarios)
            {
                auto inputB = inputA;

                for (int i = scenario.changeAt; i < length; ++i)
                {
                    inputB.left[static_cast<size_t> (i)] = later.left[static_cast<size_t> (i)];
                    inputB.right[static_cast<size_t> (i)] = later.right[static_cast<size_t> (i)];
                }

                const auto params = makeParams (factor, grainMs);
                const auto outputA = renderStretch (params, inputA, scenario.triggers, 64);
                const auto outputB = renderStretch (params, inputB, scenario.triggers, 64);

                CHECK (identicalBefore (outputA, outputB, static_cast<size_t> (scenario.changeAt)));

                if (scenario.changeInFirstHop || factor < 1.01f)
                    CHECK (maxDifference (outputA, outputB, static_cast<size_t> (scenario.changeAt)) > 0.01f);
            }
        }
    }
}

TEST_CASE ("stretch keeps the pitch of a sine")
{
    constexpr int stepStart = 2000;
    constexpr int stepLength = 44100;

    struct Case
    {
        double toneHz;
        float factor;
        float grainMs;
    };

    for (const auto& test : { Case { 440.0, 4.0f, 80.0f }, Case { 1000.0, 2.5f, 40.0f }, Case { 300.0, 16.0f, 120.0f } })
    {
        const auto input = makeSine (stepStart + stepLength, test.toneHz, sampleRate);
        const auto output = renderStretch (makeParams (test.factor, test.grainMs), input, { triggerAtStart (stepStart, stepLength) });

        // Grains are not phase aligned, so the sine drifts by up to half a cycle per hop:
        // the strongest component lies within 1 / (2 * hop) of the tone. Resampling by the
        // factor instead would land at toneHz / factor.
        const auto hopSeconds = static_cast<double> (expectedHop (test.grainMs, stepLength)) / sampleRate;
        const auto tolerance = 1.0 / (2.0 * hopSeconds) + 2.0;
        const auto begin = static_cast<size_t> (stepStart + 2 * expectedHop (test.grainMs, stepLength));
        const auto end = static_cast<size_t> (stepStart + stepLength);

        const auto strongest = strongestFrequency (output.left, begin, end, test.toneHz * 0.5, test.toneHz * 1.5);
        CHECK_NEAR (strongest, test.toneHz, tolerance);

        const auto seconds = static_cast<double> (end - begin) / sampleRate;
        const auto crossingsPerSecond = countRisingZeroCrossings (output.left, begin, end) / seconds;
        CHECK_NEAR (crossingsPerSecond, test.toneHz, tolerance);

        // and the level holds up: not cancelled into silence
        CHECK (rms (output.left, begin, end) > 0.15);
    }
}

TEST_CASE ("stretch time-stretches a two-tone input")
{
    // First half of the step 300 Hz, second half 900 Hz. At factor 4 the read centre only
    // reaches a quarter of the way in, so the whole step is the 300 Hz half, stretched.
    constexpr int stepStart = 2000;
    constexpr int stepLength = 44100;
    constexpr int switchAt = stepStart + stepLength / 2;

    StereoBuffer input (stepStart + stepLength + 1000);

    for (int i = 0; i < input.size(); ++i)
    {
        const auto frequency = i < switchAt ? 300.0 : 900.0;
        const auto value = 0.5f * static_cast<float> (std::sin (twoPi * frequency * i / sampleRate));
        input.left[static_cast<size_t> (i)] = value;
        input.right[static_cast<size_t> (i)] = value;
    }

    const auto begin = static_cast<size_t> (stepStart);
    const auto end = static_cast<size_t> (stepStart + stepLength);

    const auto dry = renderStretch (makeParams (1.0f, 80.0f), input, { triggerAtStart (stepStart, stepLength) });
    const auto dryRatio = bandPower (dry.left, begin, end, 280.0, 320.0) / bandPower (dry.left, begin, end, 880.0, 920.0);
    CHECK (dryRatio > 0.5 && dryRatio < 2.0);

    for (auto grainMs : { 20.0f, 80.0f, 250.0f })
    {
        const auto stretched = renderStretch (makeParams (4.0f, grainMs), input, { triggerAtStart (stepStart, stepLength) });
        const auto low = bandPower (stretched.left, begin, end, 280.0, 320.0);
        const auto high = bandPower (stretched.left, begin, end, 880.0, 920.0);
        CHECK (low > 100.0 * high);

        // the last quarter of the step is still 300 Hz: 75 rising crossings in 0.25 s, not 225
        CHECK_NEAR (countRisingZeroCrossings (stretched.left, end - 11025, end), 75, 10);
        CHECK (rms (stretched.left, end - 11025, end) > 0.15);
    }
}

TEST_CASE ("stretch grains start at the read centre and read forward at normal speed")
{
    // A ramp input turns each output value into the source position it came from.
    constexpr int stepStart = 2000;
    constexpr int stepLength = 44100;
    constexpr float rampScale = 1.0f / 65536.0f;
    const auto input = makeRamp (stepStart + stepLength + 1000, 0.0f, rampScale);

    for (auto factor : { 2.0f, 4.0f, 16.0f })
    {
        constexpr float grainMs = 80.0f;
        const auto hop = expectedHop (grainMs, stepLength);
        const auto grainLength = 2 * hop;
        const auto grainSamples = static_cast<double> (grainLength);
        const auto output = renderStretch (makeParams (factor, grainMs), input, { triggerAtStart (stepStart, stepLength) });

        const auto sourcePos = [&] (int64_t elapsed) { return stepStart + static_cast<double> (elapsed) / factor; };

        // Where the newer grain's window is zero, the output is purely the older grain:
        // it started one hop earlier at the read centre of that time, and has read on by one hop.
        for (int64_t grain = 1; grain * hop < stepLength; ++grain)
        {
            const auto index = static_cast<size_t> (stepStart + grain * hop);
            const auto olderStart = static_cast<int64_t> (std::floor (sourcePos ((grain - 1) * hop)));
            CHECK (output.left[index] == input.left[static_cast<size_t> (olderStart + hop)]);
        }

        // Everywhere else the output mixes grains that started within the last grain length.
        for (int64_t elapsed = grainLength; elapsed < stepLength; ++elapsed)
        {
            const auto position = output.left[static_cast<size_t> (stepStart + elapsed)] / rampScale;
            CHECK (position >= sourcePos (elapsed - grainLength) - 1.0 && position <= sourcePos (elapsed) + grainSamples);
        }

        // So by the end of the step it has only covered 1 / factor of it (plus a grain).
        const auto endPosition = static_cast<double> (output.left[static_cast<size_t> (stepStart + stepLength - 1)] / rampScale) - stepStart;
        CHECK (endPosition < stepLength / factor + grainSamples);
        CHECK (endPosition > stepLength / factor - grainSamples);
    }
}

TEST_CASE ("stretch caps grains at half the step")
{
    // A 250 ms grain on a 100 ms step would leave the whole step dry; capped, it stretches.
    constexpr int stepStart = 2000;
    constexpr int stepLength = 4410;
    const auto input = makeNoise (stepStart + stepLength + 1000, 8);
    const auto output = renderStretch (makeParams (4.0f, 250.0f), input, { triggerAtStart (stepStart, stepLength) });

    std::vector<float> difference;

    for (size_t i = stepStart + stepLength / 2; i < stepStart + stepLength; ++i)
        difference.push_back (output.left[i] - input.left[i]);

    CHECK (rms (difference) > 0.5 * rms (input.left, stepStart, stepStart + stepLength));

    // and the grain is exactly half the step: the newer window is zero a quarter step in
    const auto hop = expectedHop (250.0, stepLength);
    CHECK (hop == stepLength / 4);
    CHECK (output.left[static_cast<size_t> (stepStart + 2 * hop)] == input.left[static_cast<size_t> (stepStart + hop / 4 + hop)]);
}

TEST_CASE ("stretch does not click inside a step or across retriggers")
{
    // A 30 Hz sine moves at most ~0.0021 per sample; a hard cut between grains or steps
    // would jump by up to 1. Window and crossfade slopes allow ~0.012 at most.
    constexpr int stepStart = 4410;
    constexpr int stepLength = 22050;
    const auto input = makeSine (stepStart + 4 * stepLength, 30.0, sampleRate);

    const std::vector<TriggerEvent> triggers = {
        triggerAtStart (stepStart, stepLength),
        triggerAtStart (stepStart + stepLength, stepLength),
        { stepStart + 2 * stepLength, makeStep (stepStart + stepLength + 7000, stepLength) }, // transport jump
    };

    for (auto factor : { 2.0f, 4.0f, 16.0f })
    {
        for (auto grainMs : { 10.0f, 80.0f, 250.0f })
        {
            const auto output = renderStretch (makeParams (factor, grainMs), input, triggers);
            CHECK (maxJump (output.left) < 0.016f);
            CHECK (maxJump (output.right) < 0.016f);
        }
    }

    // On noise, consecutive steps fade over the crossfade instead of cutting.
    const auto noise = makeNoise (stepStart + 3 * stepLength, 12);
    const auto plain = renderStretch (makeParams (4.0f, 80.0f), noise, { triggerAtStart (stepStart, stepLength) });
    const auto retriggered = renderStretch (makeParams (4.0f, 80.0f), noise,
                                            { triggerAtStart (stepStart, stepLength), triggerAtStart (stepStart + stepLength, stepLength) });
    const auto boundary = static_cast<size_t> (stepStart + stepLength);
    CHECK (std::fabs (retriggered.left[boundary] - plain.left[boundary]) < 0.01f);
}

TEST_CASE ("stretch output is independent of block size")
{
    constexpr int length = 40000;
    auto input = makeNoise (length, 15, 0.3f);
    const auto tone = makeSine (length, 330.0, sampleRate, 0.4f);

    for (size_t i = 0; i < input.left.size(); ++i)
    {
        input.left[i] += tone.left[i];
        input.right[i] -= tone.right[i];
    }

    const std::vector<TriggerEvent> triggers = {
        triggerAtStart (1000, 9000),
        triggerAtStart (10000, 9000),     // consecutive step
        { 15000, makeStep (13000, 9000) }, // transport jump while sounding
        triggerAtStart (22000, 4001),
        triggerAtStart (26001, 1),         // degenerate one-sample step
        triggerAtStart (26002, 9000),
    };

    for (auto factor : { 1.0f, 3.3f, 16.0f })
    {
        for (auto grainMs : { 10.0f, 77.7f })
        {
            const auto params = makeParams (factor, grainMs);
            const auto reference = renderStretch (params, input, triggers, length);

            for (int blockSize : { 1, 7, 64, 512 })
                CHECK (maxDifference (renderStretch (params, input, triggers, blockSize), reference) == 0.0f);
        }
    }
}

TEST_CASE ("stretch picks up mid-step when the step started in the past")
{
    constexpr int length = 30000;
    const auto input = makeNoise (length, 23);
    const auto params = makeParams (4.0f, 60.0f);

    const auto jumped = renderStretch (params, input, { { 9000, makeStep (5000, 20000) } });
    const auto reference = renderStretch (params, input, { triggerAtStart (5000, 20000) });

    CHECK (identicalBefore (jumped, input, 9000));
    CHECK (maxDifference (jumped, reference, 9000) == 0.0f);
}

TEST_CASE ("stretch crossfades when retriggered while sounding")
{
    constexpr int stepStart = 2000;
    constexpr int retriggerAt = 14000;
    constexpr int length = 30000;
    const auto input = makeNoise (length, 27);
    const auto params = makeParams (4.0f, 80.0f);

    const auto oldOnly = renderStretch (params, input, { triggerAtStart (stepStart, 12000) });
    const auto newOnly = renderStretch (params, input, { triggerAtStart (retriggerAt, 12000) });
    const auto retriggered = renderStretch (params, input, { triggerAtStart (stepStart, 12000), triggerAtStart (retriggerAt, 12000) });

    CHECK (identicalBefore (retriggered, oldOnly, retriggerAt));
    CHECK (maxDifference (retriggered, newOnly, retriggerAt + crossfadeSamples) == 0.0f);
    CHECK (maxDifference (oldOnly, newOnly, retriggerAt, retriggerAt + crossfadeSamples) > 0.1f);

    for (size_t i = retriggerAt; i < retriggerAt + crossfadeSamples; ++i)
    {
        const auto low = std::min (oldOnly.left[i], newOnly.left[i]) - 1.0e-6f;
        const auto high = std::max (oldOnly.left[i], newOnly.left[i]) + 1.0e-6f;
        CHECK (retriggered.left[i] >= low && retriggered.left[i] <= high);
    }

    // The first sample after the retrigger is still almost all old stretch.
    CHECK (std::fabs (retriggered.left[retriggerAt] - oldOnly.left[retriggerAt])
           <= 0.01f * std::fabs (newOnly.left[retriggerAt] - oldOnly.left[retriggerAt]) + 1.0e-6f);
    CHECK (std::fabs (retriggered.right[retriggerAt] - oldOnly.right[retriggerAt])
           <= 0.01f * std::fabs (newOnly.right[retriggerAt] - oldOnly.right[retriggerAt]) + 1.0e-6f);

    // Not crossfaded when another source played in between: the engine fades then.
    Stretch effect;
    effect.prepare (sampleRate, 500);
    effect.setParams (params);

    CaptureBuffer capture;
    capture.prepare (length);

    auto afterGap = input;

    for (int position = 0; position < length; position += 500)
    {
        if (position == stepStart)
            effect.trigger (makeStep (stepStart, 6000));

        if (position == retriggerAt)
            effect.trigger (makeStep (retriggerAt, 12000));

        auto block = afterGap.block (position, 500);
        capture.write (block.left, block.right, 500);

        if (position < stepStart + 6000 || position >= retriggerAt)
            effect.process (capture, block, position);
    }

    CHECK (maxDifference (afterGap, newOnly, retriggerAt) == 0.0f);
}

TEST_CASE ("stretch reads its parameters at trigger time")
{
    constexpr int length = 20000;
    const auto input = makeNoise (length, 33);

    Stretch effect;
    effect.prepare (sampleRate, 512);
    effect.setParams (makeParams (4.0f, 50.0f));

    CaptureBuffer capture;
    capture.prepare (length);

    auto output = input;
    effect.trigger (makeStep (0, 10000));

    for (int position = 0; position < length; position += 500)
    {
        auto block = output.block (position, 500);
        capture.write (block.left, block.right, 500);
        effect.process (capture, block, position);

        if (position == 3000)
            effect.setParams (makeParams (1.0f, 50.0f)); // heard from the next step

        if (position + 500 == 10000)
            effect.trigger (makeStep (10000, 10000));
    }

    const auto reference = renderStretch (makeParams (4.0f, 50.0f), input, { triggerAtStart (0, 10000) });
    CHECK (maxDifference (output, reference, 0, 10000) == 0.0f);

    // factor 1 on the second step: the input again once the crossfade is over
    CHECK (maxDifference (output, input, 10000 + crossfadeSamples, length) == 0.0f);
}

TEST_CASE ("stretch stays finite for extreme parameters and inputs")
{
    constexpr int length = 4000;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();

    std::vector<StereoBuffer> inputs;
    inputs.push_back (makeConstant (length, 0.0f));
    inputs.push_back (makeConstant (length, 1.0f));
    inputs.push_back (makeConstant (length, -1.0f));
    inputs.push_back (makeNoise (length, 79, 1.0f));
    inputs.push_back (makeConstant (length, 1.0e-40f)); // denormal

    const float factors[] = { nan, -1.0f, 0.0f, 0.5f, 1.0f, 16.0f, 1.0e9f, inf, -inf };
    const float grains[] = { nan, -5.0f, 0.0f, 1.0f, 10.0f, 250.0f, 1.0e6f, inf, -inf };
    const int64_t stepLengths[] = { 1, 3, 100, 44100, 0, -5 };

    for (const auto& input : inputs)
    {
        const auto inputPeak = std::max (peak (input.left), peak (input.right));

        for (auto stepLength : stepLengths)
        {
            std::vector<TriggerEvent> triggers;
            const auto spacing = std::max<int64_t> (1, std::min<int64_t> (stepLength, 1500));

            for (int64_t start = 100; start < length; start += spacing)
                triggers.push_back ({ start, makeStep (start, stepLength) });

            for (auto factor : factors)
            {
                for (auto grainMs : grains)
                {
                    const auto output = renderStretch (makeParams (factor, grainMs), input, triggers);
                    CHECK (allFinite (output.left) && allFinite (output.right));

                    // windows sum to 1, so the output never exceeds the input
                    CHECK (std::max (peak (output.left), peak (output.right)) <= inputPeak * 1.0001f);
                }
            }
        }
    }
}

TEST_CASE ("stretch leaves the input alone until triggered")
{
    const auto input = makeNoise (3000, 43);
    const auto output = renderStretch (makeParams (4.0f, 80.0f), input, {});
    CHECK (maxDifference (output, input) == 0.0f);

    // reset() forgets the stretch
    Stretch effect;
    effect.prepare (sampleRate, 512);
    effect.setParams (makeParams (4.0f, 10.0f));

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
