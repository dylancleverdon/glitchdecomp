#pragma once

// Shared helpers for DSP tests: test signals and signal checks.

#include "dsp/AudioBlock.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace splinter::test
{

/** Owns two channels and hands out StereoBlock views. */
struct StereoBuffer
{
    std::vector<float> left, right;

    explicit StereoBuffer (int numSamples = 0) : left (static_cast<size_t> (numSamples), 0.0f), right (static_cast<size_t> (numSamples), 0.0f) {}

    int size() const { return static_cast<int> (left.size()); }
    StereoBlock block() { return { left.data(), right.data(), size() }; }
    StereoBlock block (int offset, int length) { return { left.data() + offset, right.data() + offset, length }; }
};

inline StereoBuffer makeSine (int numSamples, double frequency, double sampleRate, float amplitude = 0.5f)
{
    StereoBuffer buffer (numSamples);
    constexpr double twoPi = 6.283185307179586;

    for (int i = 0; i < numSamples; ++i)
    {
        const auto value = amplitude * static_cast<float> (std::sin (twoPi * frequency * i / sampleRate));
        buffer.left[static_cast<size_t> (i)] = value;
        buffer.right[static_cast<size_t> (i)] = value;
    }

    return buffer;
}

/** Deterministic white noise in [-amplitude, amplitude]. */
inline StereoBuffer makeNoise (int numSamples, uint32_t seed = 1, float amplitude = 0.5f)
{
    StereoBuffer buffer (numSamples);
    uint32_t state = seed != 0 ? seed : 1;

    auto next = [&state] {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return static_cast<float> (state) / 4294967295.0f * 2.0f - 1.0f;
    };

    for (int i = 0; i < numSamples; ++i)
    {
        buffer.left[static_cast<size_t> (i)] = amplitude * next();
        buffer.right[static_cast<size_t> (i)] = amplitude * next();
    }

    return buffer;
}

/** A linear ramp, handy for checking which input sample ended up where. */
inline StereoBuffer makeRamp (int numSamples, float start = 0.0f, float step = 1.0f / 65536.0f)
{
    StereoBuffer buffer (numSamples);

    for (int i = 0; i < numSamples; ++i)
    {
        buffer.left[static_cast<size_t> (i)] = start + step * static_cast<float> (i);
        buffer.right[static_cast<size_t> (i)] = -(start + step * static_cast<float> (i));
    }

    return buffer;
}

inline bool allFinite (const std::vector<float>& samples)
{
    for (auto s : samples)
        if (! std::isfinite (s))
            return false;

    return true;
}

inline float peak (const std::vector<float>& samples, size_t begin = 0, size_t end = SIZE_MAX)
{
    float result = 0.0f;
    end = std::min (end, samples.size());

    for (size_t i = begin; i < end; ++i)
        result = std::max (result, std::fabs (samples[i]));

    return result;
}

inline double rms (const std::vector<float>& samples, size_t begin = 0, size_t end = SIZE_MAX)
{
    end = std::min (end, samples.size());

    if (end <= begin)
        return 0.0;

    double sum = 0.0;

    for (size_t i = begin; i < end; ++i)
        sum += static_cast<double> (samples[i]) * samples[i];

    return std::sqrt (sum / static_cast<double> (end - begin));
}

/** Largest absolute difference between neighbouring samples: a cheap click detector. */
inline float maxJump (const std::vector<float>& samples, size_t begin = 0, size_t end = SIZE_MAX)
{
    float result = 0.0f;
    end = std::min (end, samples.size());

    for (size_t i = begin + 1; i < end; ++i)
        result = std::max (result, std::fabs (samples[i] - samples[i - 1]));

    return result;
}

} // namespace splinter::test
