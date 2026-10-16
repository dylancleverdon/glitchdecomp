#include "StepSequencer.h"

#include <algorithm>
#include <cmath>

namespace splinter
{

namespace
{
    constexpr double defaultStepBeats = 0.25;
    constexpr double fallbackSampleRate = 44100.0;

    int64_t positiveModulo (int64_t value, int divisor) noexcept
    {
        const auto remainder = value % divisor;
        return remainder < 0 ? remainder + divisor : remainder;
    }

    double sanitiseStepBeats (double stepBeats) noexcept
    {
        if (! std::isfinite (stepBeats) || stepBeats <= 0.0)
            return defaultStepBeats;

        return std::clamp (stepBeats, StepSequencer::minStepBeats, StepSequencer::maxStepBeats);
    }

    EffectType sanitiseEffect (EffectType type) noexcept
    {
        const auto value = static_cast<int> (type);
        return value >= 0 && value < numEffectTypes ? type : EffectType::None;
    }
} // namespace

double StepSequencer::sanitiseBpm (double bpm) noexcept
{
    if (! std::isfinite (bpm) || bpm <= 0.0)
        return defaultBpm;

    return std::clamp (bpm, minBpm, maxBpm);
}

double StepSequencer::samplesPerBeat (double sampleRate, double bpm) noexcept
{
    return sampleRate * 60.0 / sanitiseBpm (bpm);
}

double StepSequencer::Clock::beatsAt (int64_t index) const noexcept
{
    return anchorBeats + static_cast<double> (index - anchorIndex) / samplesPerBeat;
}

void StepSequencer::prepare (double sampleRate) noexcept
{
    sampleRate_ = std::isfinite (sampleRate) && sampleRate > 0.0 ? sampleRate : fallbackSampleRate;
    reset();
}

void StepSequencer::reset() noexcept
{
    clock_ = {};
    clock_.samplesPerBeat = samplesPerBeat (sampleRate_, defaultBpm);
    mode_ = ClockMode::none;

    blockStart_ = 0;
    blockLength_ = 0;
    cursor_ = 0;
    resyncPending_ = false;

    hasStep_ = false;
    stepNumber_ = 0;
    stepIndex_ = 0;
    stepEffect_ = EffectType::None;
    stepContext_ = {};

    randomState_ = randomSeed;
}

void StepSequencer::beginBlock (const SequencerParams& params, const TransportInfo& transport,
                                int64_t blockStartIndex, int numSamples) noexcept
{
    for (size_t slot = 0; slot < pattern_.size(); ++slot)
        pattern_[slot] = sanitiseEffect (params.steps[slot]);

    const auto chance = static_cast<double> (params.chance);
    chance_ = std::isfinite (chance) ? std::clamp (chance, 0.0, 1.0) : 1.0;

    const int numSteps = std::clamp (params.numSteps, 1, maxSteps);
    const double stepBeats = sanitiseStepBeats (params.stepBeats);
    const double beatLength = samplesPerBeat (sampleRate_, transport.bpm);

    const bool followHost = transport.isPlaying && transport.hasPpqPosition
                         && std::isfinite (transport.ppqPosition)
                         && std::abs (transport.ppqPosition) <= maxPpqMagnitude;

    bool resync = ! hasStep_;

    if (followHost)
    {
        // Where the previous block's clock says this block should start, against where the host says it does.
        const double driftSamples = std::abs (transport.ppqPosition - clock_.beatsAt (blockStartIndex)) * beatLength;

        if (mode_ != ClockMode::hostSynced || driftSamples > jumpToleranceSeconds * sampleRate_)
            resync = true;

        clock_ = { blockStartIndex, transport.ppqPosition, beatLength };
        mode_ = ClockMode::hostSynced;
    }
    else if (mode_ == ClockMode::none)
    {
        clock_ = { blockStartIndex, 0.0, beatLength };
        mode_ = ClockMode::freeRunning;
    }
    else
    {
        // Free-running carries on from wherever the clock was, so stopping the host does not
        // retrigger the step. Re-anchoring only on a tempo change keeps positions closed-form.
        if (mode_ == ClockMode::hostSynced || beatLength != clock_.samplesPerBeat)
            clock_ = { blockStartIndex, clock_.beatsAt (blockStartIndex), beatLength };

        mode_ = ClockMode::freeRunning;
    }

    if (hasStep_ && stepBeats != stepBeats_)
        resync = true;

    // A new step count can move the current step to another slot: start that slot's step.
    if (hasStep_ && numSteps != numSteps_ && positiveModulo (stepNumber_, numSteps) != stepIndex_)
        resync = true;

    numSteps_ = numSteps;
    stepBeats_ = stepBeats;

    blockStart_ = blockStartIndex;
    blockLength_ = std::max (0, numSamples);
    cursor_ = 0;
    resyncPending_ = resyncPending_ || resync;
}

bool StepSequencer::nextSegment (StepSegment& segment) noexcept
{
    if (cursor_ >= blockLength_)
        return false;

    const int64_t now = blockStart_ + cursor_;
    bool startsStep = false;

    if (resyncPending_)
    {
        beginStep (stepNumberAt (now));
        resyncPending_ = false;
        startsStep = true;
    }
    else if (stepStartIndex (stepNumber_ + 1) <= now)
    {
        // Normally the boundary is exactly `now`; it is earlier if the host drifted a little forward.
        beginStep (std::max (stepNumberAt (now), stepNumber_ + 1));
        startsStep = true;
    }

    const int64_t blockEnd = blockStart_ + blockLength_;
    const int64_t nextStepStart = std::max (stepStartIndex (stepNumber_ + 1), now + 1);
    const int64_t segmentEnd = std::min (blockEnd, nextStepStart);

    segment.offset = cursor_;
    segment.length = static_cast<int> (segmentEnd - now);
    segment.stepIndex = stepIndex_;
    segment.effect = stepEffect_;
    segment.startsStep = startsStep;
    segment.context = stepContext_;

    cursor_ += segment.length;
    return true;
}

int64_t StepSequencer::stepStartIndex (int64_t stepNumber) const noexcept
{
    const double beatsFromAnchor = static_cast<double> (stepNumber) * stepBeats_ - clock_.anchorBeats;
    const double samplesFromAnchor = std::ceil (beatsFromAnchor * clock_.samplesPerBeat - boundaryToleranceSamples);
    return clock_.anchorIndex + static_cast<int64_t> (samplesFromAnchor);
}

int64_t StepSequencer::stepNumberAt (int64_t index) const noexcept
{
    auto stepNumber = static_cast<int64_t> (std::floor (clock_.beatsAt (index) / stepBeats_));

    // The floor and the boundary rounding can disagree by one right at a boundary: boundaries win.
    for (int attempt = 0; attempt < 4 && stepStartIndex (stepNumber + 1) <= index; ++attempt)
        ++stepNumber;

    for (int attempt = 0; attempt < 4 && stepStartIndex (stepNumber) > index; ++attempt)
        --stepNumber;

    return stepNumber;
}

void StepSequencer::beginStep (int64_t stepNumber) noexcept
{
    const int64_t start = stepStartIndex (stepNumber);

    hasStep_ = true;
    stepNumber_ = stepNumber;
    stepIndex_ = static_cast<int> (positiveModulo (stepNumber, numSteps_));

    stepContext_.stepStart = start;
    stepContext_.stepLength = std::max<int64_t> (1, stepStartIndex (stepNumber + 1) - start);
    stepContext_.sampleRate = sampleRate_;
    stepContext_.samplesPerBeat = clock_.samplesPerBeat;

    const auto slotEffect = pattern_[static_cast<size_t> (stepIndex_)];
    stepEffect_ = EffectType::None;

    // Every non-empty step rolls, whatever the chance, so raising the chance only ever adds hits.
    if (slotEffect != EffectType::None && nextRandom() < chance_)
        stepEffect_ = slotEffect;
}

double StepSequencer::nextRandom() noexcept
{
    // xorshift64
    randomState_ ^= randomState_ << 13;
    randomState_ ^= randomState_ >> 7;
    randomState_ ^= randomState_ << 17;

    return static_cast<double> (randomState_ >> 11) * (1.0 / 9007199254740992.0); // [0, 1) with 53 bits
}

} // namespace splinter
