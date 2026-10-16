#include "GlitchEngine.h"

#include <algorithm>
#include <cmath>

namespace splinter
{

namespace
{
    constexpr double pi = 3.14159265358979323846;
    constexpr double fallbackSampleRate = 44100.0;
    constexpr double minSampleRate = 1000.0;
    constexpr double maxSampleRate = 768000.0;
    constexpr float minGainDb = -24.0f;
    constexpr float maxGainDb = 12.0f;

    size_t indexOf (EffectType type) noexcept
    {
        return static_cast<size_t> (type);
    }

    float decibelsToGain (float decibels) noexcept
    {
        return static_cast<float> (std::pow (10.0, static_cast<double> (decibels) / 20.0));
    }

    /** Keeps a misbehaving effect from pushing NaN or infinity into the host. */
    float finiteOrSilent (float sample) noexcept
    {
        return std::isfinite (sample) ? sample : 0.0f;
    }
} // namespace

//==============================================================================
void GlitchEngine::LinearSmoother::setTarget (float target) noexcept
{
    if (target == target_)
        return;

    target_ = target;
    remaining_ = rampLength_;
    increment_ = (target_ - current_) / static_cast<float> (rampLength_);
}

void GlitchEngine::LinearSmoother::snapTo (float value) noexcept
{
    current_ = value;
    target_ = value;
    increment_ = 0.0f;
    remaining_ = 0;
}

float GlitchEngine::LinearSmoother::next() noexcept
{
    if (remaining_ > 0)
    {
        current_ += increment_;

        if (--remaining_ == 0)
            current_ = target_;
    }

    return current_;
}

//==============================================================================
GlitchEngine::GlitchEngine()
{
    effects_[indexOf (EffectType::Stutter)] = &stutter_;
    effects_[indexOf (EffectType::Reverse)] = &reverse_;
    effects_[indexOf (EffectType::TapeStop)] = &tapeStop_;
    effects_[indexOf (EffectType::Stretch)] = &stretch_;
    effects_[indexOf (EffectType::Crusher)] = &crusher_;
    effects_[indexOf (EffectType::Gate)] = &gate_;
}

GlitchEngine::~GlitchEngine() = default;

void GlitchEngine::prepare (double sampleRate, int maxBlockSize)
{
    sampleRate_ = std::isfinite (sampleRate) && sampleRate > 0.0 ? std::clamp (sampleRate, minSampleRate, maxSampleRate)
                                                                 : fallbackSampleRate;

    // Larger host blocks are processed in chunks, so this only bounds the scratch memory.
    maxBlockSize_ = std::clamp (maxBlockSize, 1, maxChunkSize);

    capture_.prepare (static_cast<int> (std::ceil (captureSeconds * sampleRate_)) + maxBlockSize_);
    sequencer_.prepare (sampleRate_);

    for (auto* effect : effects_)
        if (effect != nullptr)
            effect->prepare (sampleRate_, maxBlockSize_);

    scratch_.assign (static_cast<size_t> (numEffectTypes) * 2 * static_cast<size_t> (maxBlockSize_), 0.0f);

    fadeLength_ = std::max (1, static_cast<int> (std::lround (crossfadeSeconds * sampleRate_)));
    fadeCurve_.resize (static_cast<size_t> (fadeLength_));

    for (int i = 0; i < fadeLength_; ++i)
    {
        // Raised cosine, sampled strictly between 0 and 1: the fade has no corner at either end.
        const double position = static_cast<double> (i + 1) / static_cast<double> (fadeLength_ + 1);
        fadeCurve_[static_cast<size_t> (i)] = static_cast<float> (0.5 - 0.5 * std::cos (pi * position));
    }

    const int smoothingLength = std::max (1, static_cast<int> (std::lround (smoothingSeconds * sampleRate_)));
    mixSmoother_.setRampLength (smoothingLength);
    gainSmoother_.setRampLength (smoothingLength);

    reset();
}

void GlitchEngine::reset() noexcept
{
    capture_.reset();
    sequencer_.reset();

    for (auto* effect : effects_)
        if (effect != nullptr)
            effect->reset();

    layers_ = {};
    numLayers_ = 1;
    snapSmoothers_ = true;

    currentStep_.store (-1, std::memory_order_relaxed);
    currentEffect_.store (static_cast<int> (EffectType::None), std::memory_order_relaxed);
}

void GlitchEngine::setParams (const EngineParams& params) noexcept
{
    params_ = params;
}

void GlitchEngine::process (StereoBlock block, const TransportInfo& transport) noexcept
{
    if (block.numSamples <= 0 || maxBlockSize_ <= 0)
        return;

    applyParams();

    const double beatsPerSample = 1.0 / StepSequencer::samplesPerBeat (sampleRate_, transport.bpm);

    for (int offset = 0; offset < block.numSamples;)
    {
        const int length = std::min (maxBlockSize_, block.numSamples - offset);

        // An oversized block runs as consecutive chunks, each told where the transport is at its start.
        auto chunkTransport = transport;
        chunkTransport.ppqPosition += static_cast<double> (offset) * beatsPerSample;

        processChunk (block.subBlock (offset, length), chunkTransport);
        offset += length;
    }

    currentStep_.store (sequencer_.currentStep(), std::memory_order_relaxed);
    currentEffect_.store (static_cast<int> (sequencer_.currentEffect()), std::memory_order_relaxed);
}

//==============================================================================
void GlitchEngine::applyParams() noexcept
{
    const float mix = std::isfinite (params_.mix) ? std::clamp (params_.mix, 0.0f, 1.0f) : 1.0f;
    const float gainDb = std::isfinite (params_.outputGainDb) ? std::clamp (params_.outputGainDb, minGainDb, maxGainDb) : 0.0f;
    const float gain = decibelsToGain (gainDb);

    if (snapSmoothers_)
    {
        mixSmoother_.snapTo (mix);
        gainSmoother_.snapTo (gain);
        snapSmoothers_ = false;
    }
    else
    {
        mixSmoother_.setTarget (mix);
        gainSmoother_.setTarget (gain);
    }

    stutter_.setParams (params_.stutter);
    reverse_.setParams (params_.reverse);
    tapeStop_.setParams (params_.tapeStop);
    stretch_.setParams (params_.stretch);
    crusher_.setParams (params_.crusher);
    gate_.setParams (params_.gate);
}

void GlitchEngine::processChunk (StereoBlock block, const TransportInfo& transport) noexcept
{
    const int64_t blockStartIndex = capture_.writeIndex();
    capture_.write (block.left, block.right, block.numSamples);

    sequencer_.beginBlock (params_.sequencer, transport, blockStartIndex, block.numSamples);

    StepSegment segment;

    while (sequencer_.nextSegment (segment))
    {
        if (segment.startsStep)
            startStep (segment);

        renderSegment (block.subBlock (segment.offset, segment.length), blockStartIndex + segment.offset);
    }
}

void GlitchEngine::startStep (const StepSegment& segment) noexcept
{
    if (auto* effect = effectFor (segment.effect))
        effect->trigger (segment.context);

    // The same effect carrying on into the next step crossfades internally after its retrigger.
    if (segment.effect != layers_[static_cast<size_t> (numLayers_ - 1)].source)
        pushLayer (segment.effect);
}

void GlitchEngine::pushLayer (EffectType source) noexcept
{
    // Full only if the source changed several times within one crossfade: the oldest
    // layer, by now nearly silent, makes room.
    if (numLayers_ == maxLayers)
    {
        std::move (layers_.begin() + 1, layers_.end(), layers_.begin());
        --numLayers_;
    }

    layers_[static_cast<size_t> (numLayers_++)] = { source, 0 };
}

void GlitchEngine::renderSegment (StereoBlock block, int64_t startIndex) noexcept
{
    for (int done = 0; done < block.numSamples;)
    {
        // Spans end where a fade completes, so a faded-out effect stops being processed
        // on exactly the same sample whatever the block size.
        int spanLength = block.numSamples - done;

        for (int layer = 1; layer < numLayers_; ++layer)
            spanLength = std::min (spanLength, fadeLength_ - layers_[static_cast<size_t> (layer)].fadeElapsed);

        renderSpan (block.subBlock (done, spanLength), startIndex + done);

        for (int layer = 1; layer < numLayers_; ++layer)
            layers_[static_cast<size_t> (layer)].fadeElapsed += spanLength;

        retireFadedLayers();
        done += spanLength;
    }
}

void GlitchEngine::renderSpan (StereoBlock block, int64_t startIndex) noexcept
{
    const int numSamples = block.numSamples;
    std::array<StereoBlock, maxLayers> layerWet {};
    unsigned renderedSources = 0;

    for (int layer = 0; layer < numLayers_; ++layer)
    {
        const auto source = layers_[static_cast<size_t> (layer)].source;
        auto* effect = effectFor (source);

        if (effect == nullptr)
            continue;

        layerWet[static_cast<size_t> (layer)] = scratchFor (source, numSamples);
        const auto sourceBit = 1u << indexOf (source);

        // An effect can sit in two layers (fading out and retriggered): render it once.
        if ((renderedSources & sourceBit) != 0)
            continue;

        renderedSources |= sourceBit;

        const auto& wet = layerWet[static_cast<size_t> (layer)];
        std::copy (block.left, block.left + numSamples, wet.left);
        std::copy (block.right, block.right + numSamples, wet.right);
        effect->process (capture_, wet, startIndex);
    }

    for (int i = 0; i < numSamples; ++i)
    {
        float remaining = 1.0f; // weight not yet taken by the layers above
        float active = 0.0f;    // total weight of the effect layers
        float wetLeft = 0.0f;
        float wetRight = 0.0f;

        for (int layer = numLayers_ - 1; layer >= 0; --layer)
        {
            const auto& current = layers_[static_cast<size_t> (layer)];
            const float fadeIn = layer == 0 ? 1.0f : fadeCurve_[static_cast<size_t> (current.fadeElapsed + i)];
            const float weight = remaining * fadeIn;
            remaining -= weight;

            if (current.source != EffectType::None)
            {
                const auto& wet = layerWet[static_cast<size_t> (layer)];
                active += weight;
                wetLeft += weight * finiteOrSilent (wet.left[i]);
                wetRight += weight * finiteOrSilent (wet.right[i]);
            }
        }

        const float mix = mixSmoother_.next();
        const float gain = gainSmoother_.next();
        const float dryAmount = 1.0f - mix * active;

        block.left[i] = (block.left[i] * dryAmount + mix * wetLeft) * gain;
        block.right[i] = (block.right[i] * dryAmount + mix * wetRight) * gain;
    }
}

void GlitchEngine::retireFadedLayers() noexcept
{
    for (int layer = numLayers_ - 1; layer > 0; --layer)
    {
        if (layers_[static_cast<size_t> (layer)].fadeElapsed >= fadeLength_)
        {
            // Fully faded in, so everything below it is silent now.
            std::move (layers_.begin() + layer, layers_.begin() + numLayers_, layers_.begin());
            numLayers_ -= layer;
            return;
        }
    }
}

GlitchEffect* GlitchEngine::effectFor (EffectType type) const noexcept
{
    const auto index = static_cast<int> (type);
    return index > 0 && index < numEffectTypes ? effects_[static_cast<size_t> (index)] : nullptr;
}

StereoBlock GlitchEngine::scratchFor (EffectType type, int numSamples) noexcept
{
    const auto channelLength = static_cast<size_t> (maxBlockSize_);
    float* left = scratch_.data() + indexOf (type) * 2 * channelLength;
    return { left, left + channelLength, numSamples };
}

} // namespace splinter
