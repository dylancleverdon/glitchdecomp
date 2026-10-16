#include "PluginProcessor.h"

#include "PluginEditor.h"

#include <algorithm>
#include <cmath>
#include <optional>

namespace splinter
{

namespace
{
    // The range the DSP core works in; the per-chunk transport below must use the same tempo.
    constexpr double minBpm = 20.0;
    constexpr double maxBpm = 999.0;
    constexpr double fallbackSampleRate = 44100.0;

    const juce::Identifier stateType { "SplinterParameters" };
    const juce::Identifier stateVersionAttribute { "stateVersion" };

    // These match the child layout AudioProcessorValueTreeState uses for its state.
    const juce::Identifier parameterType { "PARAM" };
    const juce::Identifier parameterIdProperty { "id" };
    const juce::Identifier parameterValueProperty { "value" };

    const juce::XmlElement* findParameterElement (const juce::XmlElement& stateXml, const juce::String& parameterID)
    {
        for (const auto* child : stateXml.getChildWithTagNameIterator (parameterType.toString()))
            if (child->getStringAttribute (parameterIdProperty) == parameterID)
                return child;

        return nullptr;
    }

    /** The stored value of a parameter element, if it is a plain finite number. */
    std::optional<double> readStoredValue (const juce::XmlElement& element)
    {
        const auto text = element.getStringAttribute (parameterValueProperty).trim();

        if (text.isEmpty() || ! text.containsOnly ("0123456789.-+eE"))
            return std::nullopt;

        const auto value = text.getDoubleValue();
        return std::isfinite (value) ? std::optional<double> (value) : std::nullopt;
    }
}

//==============================================================================
SplinterAudioProcessor::SplinterAudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      parameters_ (*this, nullptr, stateType, createParameterLayout()),
      parameterReader_ (parameters_)
{
}

SplinterAudioProcessor::~SplinterAudioProcessor() = default;

//==============================================================================
void SplinterAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    preparedSampleRate_ = std::isfinite (sampleRate) && sampleRate > 0.0 ? sampleRate : fallbackSampleRate;
    preparedBlockSize_ = std::max (1, samplesPerBlock);

    monoScratch_.assign (static_cast<size_t> (preparedBlockSize_), 0.0f);

    engine_.prepare (preparedSampleRate_, preparedBlockSize_);
    engine_.reset();
    engine_.setParams (parameterReader_.read());
}

void SplinterAudioProcessor::releaseResources()
{
    engine_.reset();
}

void SplinterAudioProcessor::reset()
{
    engine_.reset();
}

bool SplinterAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto input = layouts.getMainInputChannelSet();
    const auto output = layouts.getMainOutputChannelSet();

    if (input != output)
        return false;

    return output == juce::AudioChannelSet::mono() || output == juce::AudioChannelSet::stereo();
}

//==============================================================================
TransportInfo SplinterAudioProcessor::readTransport() noexcept
{
    TransportInfo transport;

    if (auto* hostPlayHead = getPlayHead())
    {
        if (const auto position = hostPlayHead->getPosition())
        {
            // Keep the last tempo the host reported if a block arrives without one.
            if (const auto bpm = position->getBpm(); bpm.hasValue() && std::isfinite (*bpm) && *bpm > 0.0)
                lastHostBpm_ = std::clamp (*bpm, minBpm, maxBpm);

            if (const auto ppq = position->getPpqPosition(); ppq.hasValue() && std::isfinite (*ppq))
            {
                transport.hasPpqPosition = true;
                transport.ppqPosition = *ppq;
            }

            transport.isPlaying = position->getIsPlaying();
        }
    }

    transport.bpm = lastHostBpm_;
    return transport;
}

void SplinterAudioProcessor::processChunk (float* left, float* right, int numSamples, const TransportInfo& transport) noexcept
{
    if (right == nullptr)
    {
        // Mono: the engine always works in stereo, so give it a copy of the channel as its right side.
        std::copy (left, left + numSamples, monoScratch_.begin());
        engine_.process ({ left, monoScratch_.data(), numSamples }, transport);
        return;
    }

    engine_.process ({ left, right, numSamples }, transport);
}

void SplinterAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int numSamples = buffer.getNumSamples();
    const int numInputChannels = getTotalNumInputChannels();
    const int numOutputChannels = getTotalNumOutputChannels();

    for (int channel = numInputChannels; channel < numOutputChannels; ++channel)
        buffer.clear (channel, 0, numSamples);

    const int numChannels = std::min (buffer.getNumChannels(), numInputChannels);

    if (preparedBlockSize_ <= 0 || numSamples <= 0 || numChannels <= 0)
        return;

    engine_.setParams (parameterReader_.read());

    const auto transport = readTransport();
    const double beatsPerSample = transport.bpm / (60.0 * preparedSampleRate_);

    float* left = buffer.getWritePointer (0);
    float* right = numChannels >= 2 ? buffer.getWritePointer (1) : nullptr;

    // Hosts may send more than the block size they announced: the mono scratch only
    // holds preparedBlockSize_ samples, so run oversized blocks as consecutive chunks.
    for (int offset = 0; offset < numSamples; offset += preparedBlockSize_)
    {
        const int length = std::min (preparedBlockSize_, numSamples - offset);

        auto chunkTransport = transport;
        chunkTransport.ppqPosition += static_cast<double> (offset) * beatsPerSample;

        processChunk (left + offset, right != nullptr ? right + offset : nullptr, length, chunkTransport);
    }
}

//==============================================================================
juce::AudioProcessorEditor* SplinterAudioProcessor::createEditor()
{
    return new SplinterEditor (*this);
}

//==============================================================================
void SplinterAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    const auto state = parameters_.copyState();

    if (auto xml = state.createXml())
    {
        xml->setAttribute (stateVersionAttribute, stateVersion);
        copyXmlToBinary (*xml, destData);
    }
}

void SplinterAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (data == nullptr || sizeInBytes <= 0)
        return;

    const auto xml = getXmlFromBinary (data, sizeInBytes);

    if (xml == nullptr || ! xml->hasTagName (stateType))
        return;

    // stateVersion 1 is the only format so far, so there is nothing to migrate. Parameters
    // are only ever appended, so a state saved by a newer version loads too.

    // Rebuild the state from the known parameters only: unknown IDs are dropped, values
    // are clamped to their range, and missing or unreadable values take their defaults.
    juce::ValueTree restored (stateType);

    for (auto* parameter : getParameters())
    {
        auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (parameter);

        if (ranged == nullptr)
            continue;

        const auto& range = ranged->getNormalisableRange();
        auto value = ranged->convertFrom0to1 (ranged->getDefaultValue());

        if (const auto* element = findParameterElement (*xml, ranged->paramID))
            if (const auto stored = readStoredValue (*element))
                value = range.snapToLegalValue (static_cast<float> (juce::jlimit<double> (range.start, range.end, *stored)));

        restored.appendChild (juce::ValueTree (parameterType, { { parameterIdProperty, ranged->paramID },
                                                                { parameterValueProperty, value } }),
                              nullptr);
    }

    parameters_.replaceState (restored);
}

} // namespace splinter

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new splinter::SplinterAudioProcessor();
}
