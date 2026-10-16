#include "Stutter.h"

#include <algorithm>
#include <cmath>

namespace splinter
{

namespace
{
    constexpr double pi = 3.14159265358979323846;
    constexpr double ln2 = 0.69314718055994530942;
    constexpr double ln10 = 2.30258509299404568402;

    // Repeats far past the step end keep fading, but never into denormal territory.
    constexpr double gainFloorDb = -120.0;

    // Keeps every position exactly representable in a double, whatever the engine sends.
    constexpr double maxStepLength = 1099511627776.0; // 2^40 samples

    double finiteOr (double value, double fallback) noexcept
    {
        return std::isfinite (value) ? value : fallback;
    }

    /** 0 -> 1 with zero slope at both ends. The outgoing side uses 1 minus this, so the
        weights always sum to one and a crossfade can never raise the level. */
    double raisedCosine (double x) noexcept
    {
        return 0.5 - 0.5 * std::cos (pi * std::clamp (x, 0.0, 1.0));
    }

    /** Interpolated read that never looks at samples after newestAllowed. The cubic needs
        two samples of look-ahead, so right behind the write head it falls back to linear. */
    float readCausal (const CaptureBuffer& capture, int channel, double position, int64_t newestAllowed) noexcept
    {
        if (! std::isfinite (position))
            return 0.0f;

        const auto whole = std::floor (position);
        const auto base = static_cast<int64_t> (whole);

        if (base >= newestAllowed)
            return capture.sample (channel, newestAllowed);

        if (whole == position)
            return capture.sample (channel, base);

        if (base + 2 <= newestAllowed)
            return capture.sampleInterpolated (channel, position);

        const auto fraction = static_cast<float> (position - whole);
        const float current = capture.sample (channel, base);
        return current + fraction * (capture.sample (channel, base + 1) - current);
    }
} // namespace

double Stutter::Voice::glideIntegral (double elapsed) const noexcept
{
    // expm1 keeps tiny glides accurate, where 2^x - 1 would cancel catastrophically.
    const double exponent = octavesPerSample * ln2 * elapsed;
    return exponent == 0.0 ? elapsed : elapsed * std::expm1 (exponent) / exponent;
}

double Stutter::Voice::readDistance (double elapsed) const noexcept
{
    const double withinStep = std::min (elapsed, stepLength);
    double distance = 0.0;

    if (octavesPerSample > 0.0)
        distance = withinStep <= sliceLength ? withinStep
                                             : sliceLength + glideIntegral (withinStep) - glideIntegral (sliceLength);
    else
        distance = glideIntegral (withinStep);

    // Past the step end (while the engine fades this voice out) the rate stops gliding.
    if (elapsed > stepLength)
        distance += std::exp2 (octavesPerSample * stepLength) * (elapsed - stepLength);

    return distance;
}

float Stutter::Voice::repeatGain (double repeat) const noexcept
{
    return static_cast<float> (std::exp (std::max (logGainPerRepeat * repeat, gainFloorDb / 20.0 * ln10)));
}

void Stutter::prepare (double sampleRate, int /*maxBlockSize*/)
{
    sampleRate_ = sampleRate > 0.0 && std::isfinite (sampleRate) ? sampleRate : 44100.0;
    crossfadeLength_ = std::max (1, static_cast<int> (std::lround (crossfadeSeconds * sampleRate_)));
    reset();
}

void Stutter::reset() noexcept
{
    numVoices_ = 0;
    hasPendingVoice_ = false;
    processedEnd_ = std::numeric_limits<int64_t>::min();
}

void Stutter::trigger (const StepContext& step) noexcept
{
    Voice voice;
    voice.stepStart = step.stepStart;
    voice.stepLength = std::clamp (static_cast<double> (step.stepLength), 1.0, maxStepLength);

    const double requestedSlice = finiteOr (static_cast<double> (params_.repeatBeats) * step.samplesPerBeat, 0.0);
    voice.sliceLength = std::min (std::max (requestedSlice, minSliceSamples), voice.stepLength);
    voice.loopCrossfade = std::min (static_cast<double> (crossfadeLength_), 0.25 * voice.sliceLength);

    const double glide = std::clamp (finiteOr (params_.pitchGlideSemitones, 0.0), -maxGlideSemitones, maxGlideSemitones);
    voice.octavesPerSample = glide / (12.0 * voice.stepLength);

    // Spread the decay so the last repeat that starts inside the step is decayRangeDb down.
    const double decay = std::clamp (finiteOr (params_.decay, 0.0), 0.0, 1.0);
    const double passesInStep = voice.readDistance (voice.stepLength) / voice.sliceLength;
    const double lastRepeat = std::max (1.0, std::ceil (passesInStep - 1.0e-6) - 1.0);
    voice.logGainPerRepeat = -decay * decayRangeDb / 20.0 * ln10 / lastRepeat;

    pendingVoice_ = voice;
    hasPendingVoice_ = true;
}

void Stutter::startPendingVoice (int64_t firstIndex) noexcept
{
    // Only crossfade from the old voices if they were sounding right up to this sample; after
    // a gap the engine was playing another source and does that crossfade itself.
    if (firstIndex != processedEnd_)
        numVoices_ = 0;

    // With every slot busy the oldest voice, by now mostly faded out, is dropped.
    numVoices_ = std::min (numVoices_ + 1, maxVoices);

    for (int voice = numVoices_ - 1; voice > 0; --voice)
        voices_[voice] = voices_[voice - 1];

    voices_[0] = pendingVoice_;
    voices_[0].fadeInStart = firstIndex;
    hasPendingVoice_ = false;
}

Stutter::Frame Stutter::renderVoice (const Voice& voice, const CaptureBuffer& capture, int64_t now) noexcept
{
    if (now < voice.stepStart)
        return { capture.sample (0, now), capture.sample (1, now) };

    const double distance = voice.readDistance (static_cast<double> (now - voice.stepStart));
    double repeat = std::floor (distance / voice.sliceLength);
    double phase = distance - repeat * voice.sliceLength;

    // The division can round across a pass boundary; keep phase in [0, sliceLength).
    if (phase >= voice.sliceLength)
    {
        phase -= voice.sliceLength;
        repeat += 1.0;
    }
    else if (phase < 0.0)
    {
        phase += voice.sliceLength;
        repeat -= 1.0;
    }

    const double position = static_cast<double> (voice.stepStart) + phase;
    const float gain = voice.repeatGain (repeat);
    Frame frame { gain * readCausal (capture, 0, position, now), gain * readCausal (capture, 1, position, now) };

    const double crossfadeStart = voice.sliceLength - voice.loopCrossfade;

    if (voice.loopCrossfade > 0.0 && phase > crossfadeStart)
    {
        // Blend into the audio just before the slice, which continues seamlessly into the
        // slice start once the phase wraps.
        const auto fadeIn = static_cast<float> (raisedCosine ((phase - crossfadeStart) / voice.loopCrossfade));
        const float leadInGain = fadeIn * voice.repeatGain (repeat + 1.0);
        const double leadInPosition = position - voice.sliceLength;

        frame.left = (1.0f - fadeIn) * frame.left + leadInGain * readCausal (capture, 0, leadInPosition, now);
        frame.right = (1.0f - fadeIn) * frame.right + leadInGain * readCausal (capture, 1, leadInPosition, now);
    }

    return frame;
}

void Stutter::process (const CaptureBuffer& capture, StereoBlock block, int64_t blockStartIndex) noexcept
{
    if (hasPendingVoice_)
        startPendingVoice (blockStartIndex);

    if (numVoices_ > 0)
    {
        for (int i = 0; i < block.numSamples; ++i)
        {
            const int64_t now = blockStartIndex + i;
            Frame mix;
            float remaining = 1.0f; // weight left over for the older voices

            // Each voice crossfades in over the mix of the voices before it.
            for (int voice = 0; voice < numVoices_; ++voice)
            {
                const bool oldest = voice == numVoices_ - 1;
                const auto sinceFadeIn = static_cast<double> (now - voices_[voice].fadeInStart);
                const auto fadeIn = oldest ? 1.0f
                                           : static_cast<float> (raisedCosine (sinceFadeIn / static_cast<double> (crossfadeLength_)));
                const auto weight = remaining * fadeIn;
                const auto frame = renderVoice (voices_[voice], capture, now);

                mix.left += weight * frame.left;
                mix.right += weight * frame.right;
                remaining -= weight;

                if (remaining <= 0.0f)
                {
                    numVoices_ = voice + 1; // everything older has faded out
                    break;
                }
            }

            block.left[i] = mix.left;
            block.right[i] = mix.right;
        }
    }

    processedEnd_ = blockStartIndex + block.numSamples;
}

} // namespace splinter
