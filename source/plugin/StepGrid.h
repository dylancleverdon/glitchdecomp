#pragma once

#include "dsp/EffectParams.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <functional>

namespace splinter
{

class SplinterAudioProcessor;

/** The 16-step pattern editor.

    Each cell shows its step's effect. Left click cycles to the next effect, right click
    picks one from a menu, and shift-drag paints the effect of the cell where the drag
    started across the cells it passes. Every edit goes through the step parameters with
    a change gesture, so hosts record automation and undo; the grid polls the parameters
    and the processor's playhead at 30 Hz and repaints only the cells that changed. */
class StepGrid final : public juce::Component,
                       public juce::SettableTooltipClient,
                       private juce::Timer
{
public:
    explicit StepGrid (SplinterAudioProcessor& processor);
    ~StepGrid() override;

    /** Called on the message thread when the playing step or the sounding effect changes. */
    std::function<void (int step, EffectType soundingEffect)> onPlayheadChanged;

    void paint (juce::Graphics& g) override;
    void resized() override;

    void mouseDown (const juce::MouseEvent& event) override;
    void mouseDrag (const juce::MouseEvent& event) override;
    void mouseUp (const juce::MouseEvent& event) override;
    void mouseMove (const juce::MouseEvent& event) override;
    void mouseExit (const juce::MouseEvent& event) override;

private:
    struct CellState
    {
        EffectType effect = EffectType::None;
        bool inPattern = true; // false for steps beyond the 'steps' parameter
        bool isPlayhead = false;
        bool isSounding = false; // playhead step whose effect fired
        bool isHovered = false;

        bool operator== (const CellState& other) const noexcept;
        bool operator!= (const CellState& other) const noexcept { return ! operator== (other); }
    };

    void timerCallback() override;
    void refresh();
    CellState readCellState (int step, int playheadStep, EffectType soundingEffect, int numSteps) const;

    EffectType stepEffect (int step) const;
    void setStepEffect (int step, EffectType effect);
    void setAllSteps (const std::array<EffectType, maxSteps>& pattern);

    void showStepMenu (int step);
    void paintThrough (int step);
    void finishPainting();

    juce::Rectangle<float> cellBounds (int step) const;
    juce::Rectangle<float> numberBounds (int step) const;
    int stepAt (float x) const;
    void paintCell (juce::Graphics& g, int step, const CellState& state) const;

    SplinterAudioProcessor& processor_;
    std::array<juce::AudioParameterChoice*, maxSteps> stepParameters_ {};
    juce::AudioParameterInt* numStepsParameter_ = nullptr;

    std::array<CellState, maxSteps> shownCells_ {};
    int shownPlayhead_ = -2; // forces the first onPlayheadChanged call
    EffectType shownSoundingEffect_ = EffectType::None;
    int hoveredStep_ = -1;

    bool isPainting_ = false;
    EffectType paintEffect_ = EffectType::None;
    int lastPaintedStep_ = -1;
    std::array<bool, maxSteps> paintGestureOpen_ {};

    int numOpenMenus_ = 0;

    juce::Rectangle<float> cellArea_; // the cells, without the step numbers below them

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StepGrid)
};

} // namespace splinter
