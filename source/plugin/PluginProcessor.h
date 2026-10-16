#pragma once

#include "Parameters.h"
#include "dsp/GlitchEngine.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <vector>

namespace splinter
{

/** The plugin: owns the parameters and the GlitchEngine and feeds it host audio and transport. */
class SplinterAudioProcessor final : public juce::AudioProcessor
{
public:
    SplinterAudioProcessor();
    ~SplinterAudioProcessor() override;

    //==============================================================================
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void reset() override;

    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages) override;
    using juce::AudioProcessor::processBlock;

    //==============================================================================
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    //==============================================================================
    juce::AudioProcessorValueTreeState& getParameterState() noexcept { return parameters_; }

    /** Sequencer step at the end of the last processed block, or -1 before any audio. Any thread. */
    int getCurrentStep() const noexcept { return engine_.currentStep(); }

    /** Effect sounding at the end of the last processed block. Any thread. */
    EffectType getCurrentEffect() const noexcept { return engine_.currentEffect(); }

    /** Written into the saved state; bump it when the state format changes. */
    static constexpr int stateVersion = 1;

private:
    TransportInfo readTransport() noexcept;
    void processChunk (float* left, float* right, int numSamples, const TransportInfo& transport) noexcept;

    juce::AudioProcessorValueTreeState parameters_;
    ParameterReader parameterReader_;
    GlitchEngine engine_;

    std::vector<float> monoScratch_; // stands in for the right channel of a mono bus
    int preparedBlockSize_ = 0;      // 0 until prepareToPlay(); the audio passes through untouched before that
    double preparedSampleRate_ = 44100.0;
    double lastHostBpm_ = 120.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SplinterAudioProcessor)
};

} // namespace splinter
