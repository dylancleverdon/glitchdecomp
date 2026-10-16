#include "DemoLoop.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace splinter::render
{

namespace
{
    constexpr double twoPi = 6.283185307179586;

    /** Deterministic white noise in [-1, 1]. */
    class NoiseSource
    {
    public:
        float next() noexcept
        {
            state_ ^= state_ << 13;
            state_ ^= state_ >> 17;
            state_ ^= state_ << 5;
            return static_cast<float> (state_) / 2147483648.0f - 1.0f;
        }

    private:
        uint32_t state_ = 0x2545F491u;
    };

    struct Stereo
    {
        std::vector<float>& left;
        std::vector<float>& right;
        double sampleRate;

        size_t size() const noexcept { return left.size(); }

        void add (size_t index, double leftValue, double rightValue) noexcept
        {
            left[index] += static_cast<float> (leftValue);
            right[index] += static_cast<float> (rightValue);
        }
    };

    /** Short linear fade-in so no voice starts with a click. */
    double attack (double time, double attackSeconds) noexcept
    {
        return std::min (1.0, time / attackSeconds);
    }

    void addKick (Stereo& out, size_t start)
    {
        const auto length = static_cast<size_t> (0.4 * out.sampleRate);
        double phase = 0.0;

        for (size_t i = 0; i < length && start + i < out.size(); ++i)
        {
            const double time = static_cast<double> (i) / out.sampleRate;
            const double frequency = 48.0 + 110.0 * std::exp (-time / 0.03);
            phase += twoPi * frequency / out.sampleRate;

            const double value = 0.8 * attack (time, 0.001) * std::exp (-time / 0.15) * std::sin (phase);
            out.add (start + i, value, value);
        }
    }

    void addSnare (Stereo& out, size_t start, double velocity, NoiseSource& noise)
    {
        const auto length = static_cast<size_t> (0.25 * out.sampleRate);
        float previousNoise = 0.0f;

        for (size_t i = 0; i < length && start + i < out.size(); ++i)
        {
            const double time = static_cast<double> (i) / out.sampleRate;
            const float white = noise.next();
            const double bright = static_cast<double> (white - 0.6f * previousNoise); // tilts the noise upwards
            previousNoise = white;

            const double body = 0.25 * std::exp (-time / 0.04) * std::sin (twoPi * 185.0 * time);
            const double rattle = 0.3 * std::exp (-time / 0.07) * bright;
            const double value = velocity * attack (time, 0.001) * (body + rattle);
            out.add (start + i, value, value);
        }
    }

    void addHat (Stereo& out, size_t start, double velocity, NoiseSource& noise)
    {
        const auto length = static_cast<size_t> (0.06 * out.sampleRate);
        float previousNoise = 0.0f;

        for (size_t i = 0; i < length && start + i < out.size(); ++i)
        {
            const double time = static_cast<double> (i) / out.sampleRate;
            const float white = noise.next();
            const double hiss = 0.5 * static_cast<double> (white - previousNoise); // first difference: a crude high-pass
            previousNoise = white;

            const double value = velocity * attack (time, 0.0005) * std::exp (-time / 0.015) * hiss;
            out.add (start + i, 0.7 * value, value); // slightly to the right
        }
    }

    void addBassNote (Stereo& out, size_t start, size_t length, int midiNote)
    {
        const double frequency = 440.0 * std::pow (2.0, (midiNote - 69) / 12.0);
        const double releaseSeconds = 0.012;
        const double noteSeconds = static_cast<double> (length) / out.sampleRate;

        for (size_t i = 0; i < length && start + i < out.size(); ++i)
        {
            const double time = static_cast<double> (i) / out.sampleRate;
            const double phase = twoPi * frequency * time;

            // A band-limited saw: the first eight harmonics.
            double saw = 0.0;

            for (int harmonic = 1; harmonic <= 8; ++harmonic)
                saw += std::sin (harmonic * phase) / harmonic;

            const double release = std::clamp ((noteSeconds - time) / releaseSeconds, 0.0, 1.0);
            const double value = 0.2 * attack (time, 0.004) * release * std::exp (-time / 0.3) * saw;
            out.add (start + i, value, value);
        }
    }

    // Two bars of sixteenths, MIDI note numbers around A1; 0 is a rest.
    constexpr int bassLine[32] { 33, 0, 33, 45, 0, 33, 43, 0, 33, 0, 40, 0, 43, 45, 0, 36,
                                 33, 0, 33, 45, 0, 33, 43, 0, 38, 0, 38, 0, 40, 0, 43, 40 };
} // namespace

std::vector<std::vector<float>> makeDemoLoop (double sampleRate, double bpm, double seconds)
{
    const auto numSamples = static_cast<size_t> (std::max (0.0, std::round (seconds * sampleRate)));
    std::vector<std::vector<float>> channels (2, std::vector<float> (numSamples, 0.0f));

    if (numSamples == 0 || ! (sampleRate > 0.0) || ! (bpm > 0.0))
        return channels;

    Stereo out { channels[0], channels[1], sampleRate };
    NoiseSource noise;
    const double sixteenth = sampleRate * 60.0 / bpm / 4.0;

    for (int64_t step = 0;; ++step)
    {
        const auto start = static_cast<size_t> (std::llround (static_cast<double> (step) * sixteenth));

        if (start >= numSamples)
            break;

        const auto inBar = static_cast<int> (step % 16);

        if (inBar % 4 == 0)
            addKick (out, start);

        if (inBar == 4 || inBar == 12)
            addSnare (out, start, 1.0, noise);
        else if (step % 32 == 31)
            addSnare (out, start, 0.35, noise); // ghost note at the end of every second bar

        addHat (out, start, inBar % 4 == 2 ? 0.5 : (inBar % 2 == 1 ? 0.25 : 0.18), noise);

        if (const int note = bassLine[step % 32]; note != 0)
            addBassNote (out, start, static_cast<size_t> (0.85 * sixteenth), note);
    }

    // Safety: keep the peak at -1 dBFS whatever the tempo does to overlapping voices.
    constexpr float maxPeak = 0.891f;
    float loudest = 0.0f;

    for (const auto& channel : channels)
        for (const float sample : channel)
            loudest = std::max (loudest, std::fabs (sample));

    if (loudest > maxPeak)
        for (auto& channel : channels)
            for (auto& sample : channel)
                sample *= maxPeak / loudest;

    return channels;
}

} // namespace splinter::render
