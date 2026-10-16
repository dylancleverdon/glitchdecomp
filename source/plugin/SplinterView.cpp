#include "SplinterView.h"

#include "Parameters.h"
#include "PluginProcessor.h"
#include "Theme.h"

#include <initializer_list>

namespace splinter
{

namespace
{
    constexpr int outerMargin = 16;
    constexpr int headerHeight = 54;
    constexpr int patternHeight = 150;
    constexpr int patternTitleHeight = 26;
    constexpr int rowGap = 10;
    constexpr int panelGap = 10;
    constexpr int statusWidth = 260;
    constexpr float patternCornerSize = 10.0f;

    /** Shares a row between panels in proportion to their layout weights. */
    void layoutRow (juce::Rectangle<int> row, std::initializer_list<ControlPanel*> panels)
    {
        float totalWeight = 0.0f;

        for (auto* panel : panels)
            totalWeight += panel->getLayoutWeight();

        const int numGaps = static_cast<int> (panels.size()) - 1;
        const auto spaceForPanels = static_cast<float> (row.getWidth() - numGaps * panelGap);
        int panelsLeft = static_cast<int> (panels.size());

        for (auto* panel : panels)
        {
            const int width = --panelsLeft == 0 ? row.getWidth()
                                                : juce::roundToInt (spaceForPanels * panel->getLayoutWeight() / totalWeight);
            panel->setBounds (row.removeFromLeft (width));
            row.removeFromLeft (panelGap);
        }
    }
}

//==============================================================================
SplinterView::SplinterView (SplinterAudioProcessor& processor)
    : stepGrid_ (processor),
      globalPanel_ ("Global", Theme::globalAccent),
      stutterPanel_ ("Stutter", Theme::effectColour (EffectType::Stutter), EffectType::Stutter),
      reversePanel_ ("Reverse", Theme::effectColour (EffectType::Reverse), EffectType::Reverse),
      tapeStopPanel_ ("Tape Stop", Theme::effectColour (EffectType::TapeStop), EffectType::TapeStop),
      stretchPanel_ ("Stretch", Theme::effectColour (EffectType::Stretch), EffectType::Stretch),
      crusherPanel_ ("Crusher", Theme::effectColour (EffectType::Crusher), EffectType::Crusher),
      gatePanel_ ("Gate", Theme::effectColour (EffectType::Gate), EffectType::Gate),
      effectPanels_ { &stutterPanel_, &reversePanel_, &tapeStopPanel_, &stretchPanel_, &crusherPanel_, &gatePanel_ }
{
    auto& state = processor.getParameterState();

    globalPanel_.addKnob (state, ParamIDs::mix, "Mix", "Dry/wet balance while a step's effect plays");
    globalPanel_.addKnob (state, ParamIDs::output, "Output", "Output level", 0.0f);
    globalPanel_.addChoice (state, ParamIDs::stepRate, "Step Rate", "Length of one step");
    globalPanel_.addKnob (state, ParamIDs::steps, "Steps", "Pattern length: the sequence loops after this many steps");
    globalPanel_.addKnob (state, ParamIDs::chance, "Chance", "Probability that a step's effect fires");

    stutterPanel_.addChoice (state, ParamIDs::stutterRepeat, "Repeat", "Length of the repeated slice");
    stutterPanel_.addKnob (state, ParamIDs::stutterPitch, "Pitch", "Pitch reached by the end of the step", 0.0f);
    stutterPanel_.addKnob (state, ParamIDs::stutterDecay, "Decay", "How quickly the repeats fade away");

    reversePanel_.addKnob (state, ParamIDs::reverseSpeed, "Speed", "Playback speed of the reversed audio", 1.0f);

    tapeStopPanel_.addKnob (state, ParamIDs::tapeStopTime, "Time", "Share of the step it takes to come to a halt");
    tapeStopPanel_.addKnob (state, ParamIDs::tapeStopCurve, "Curve", "Shape of the slow-down, from linear to exponential");

    stretchPanel_.addKnob (state, ParamIDs::stretchFactor, "Factor", "How many times slower the audio plays, at the same pitch");
    stretchPanel_.addKnob (state, ParamIDs::stretchGrain, "Grain", "Grain size: short is grittier, long is smoother");

    crusherPanel_.addKnob (state, ParamIDs::crusherBits, "Bits", "Bit depth");
    crusherPanel_.addKnob (state, ParamIDs::crusherDownsample, "Downsample", "Holds every sample for this many samples");

    gatePanel_.addChoice (state, ParamIDs::gateDivision, "Division", "Length of one gate cycle");
    gatePanel_.addKnob (state, ParamIDs::gateDuty, "Duty", "Share of each cycle the gate is open");

    addAndMakeVisible (stepGrid_);
    addAndMakeVisible (globalPanel_);

    for (auto* panel : effectPanels_)
        addAndMakeVisible (*panel);

    stepGrid_.onPlayheadChanged = [this] (int step, EffectType soundingEffect) { showPlayhead (step, soundingEffect); };

    setSize (logicalWidth, logicalHeight);
}

SplinterView::~SplinterView()
{
    stepGrid_.onPlayheadChanged = nullptr;
}

void SplinterView::showPlayhead (int step, EffectType soundingEffect)
{
    playheadStep_ = step;
    soundingEffect_ = soundingEffect;

    for (auto* panel : effectPanels_)
        panel->setLit (panel->getEffect() == soundingEffect);

    repaint (statusArea_);
}

//==============================================================================
void SplinterView::resized()
{
    auto area = getLocalBounds().reduced (outerMargin, 0);

    headerArea_ = area.removeFromTop (headerHeight);
    statusArea_ = headerArea_.withTrimmedLeft (headerArea_.getWidth() - statusWidth).reduced (0, 14);

    patternArea_ = area.removeFromTop (patternHeight);
    stepGrid_.setBounds (patternArea_.withTrimmedTop (patternTitleHeight).reduced (6, 0).withTrimmedBottom (4));

    area.removeFromTop (rowGap + 2);
    area.removeFromBottom (outerMargin);

    const int rowHeight = (area.getHeight() - rowGap) / 2;
    layoutRow (area.removeFromTop (rowHeight), { &globalPanel_, &stutterPanel_, &reversePanel_ });
    area.removeFromTop (rowGap);
    layoutRow (area, { &tapeStopPanel_, &stretchPanel_, &crusherPanel_, &gatePanel_ });
}

void SplinterView::paint (juce::Graphics& g)
{
    g.fillAll (Theme::background);

    const auto glowBottom = static_cast<float> (patternArea_.getBottom());
    g.setGradientFill (juce::ColourGradient (Theme::backgroundRaised, 0.0f, 0.0f, Theme::background, 0.0f, glowBottom, false));
    g.fillRect (getLocalBounds().withHeight (patternArea_.getBottom()));

    paintHeader (g);
    paintStatus (g);

    const auto pattern = patternArea_.toFloat();
    g.setColour (Theme::backgroundRaised);
    g.fillRoundedRectangle (pattern, patternCornerSize);
    g.setColour (Theme::panelOutline);
    g.drawRoundedRectangle (pattern.reduced (0.5f), patternCornerSize, 1.0f);

    const auto titleRow = patternArea_.withHeight (patternTitleHeight).reduced (14, 0).withTrimmedTop (4);

    g.setColour (Theme::textDim);
    g.setFont (Theme::font (12.0f, true).withExtraKerningFactor (0.12f));
    g.drawText ("PATTERN", titleRow, juce::Justification::centredLeft, false);

    g.setColour (Theme::textFaint);
    g.setFont (Theme::font (12.0f));
    g.drawText ("click: next effect    right-click: choose    shift-drag: paint", titleRow, juce::Justification::centredRight, true);
}

void SplinterView::paintHeader (juce::Graphics& g) const
{
    auto area = headerArea_.toFloat();

    // The mark: six splinters, one in each effect's colour.
    const auto mark = area.removeFromLeft (40.0f).withSizeKeepingCentre (38.0f, 24.0f);
    constexpr float shardOffsets[] { 5.0f, 0.0f, 8.0f, 2.0f, 10.0f, 4.0f };

    for (int shard = 0; shard < 6; ++shard)
    {
        const float x = mark.getX() + static_cast<float> (shard) * 6.0f;
        const float top = mark.getY() + shardOffsets[shard];
        const float bottom = mark.getBottom() - shardOffsets[5 - shard] * 0.5f;

        juce::Path path;
        path.startNewSubPath (x + 4.0f, top);
        path.lineTo (x + 7.0f, top);
        path.lineTo (x + 3.0f, bottom);
        path.lineTo (x, bottom);
        path.closeSubPath();

        g.setColour (Theme::effectColour (static_cast<EffectType> (shard + 1)));
        g.fillPath (path);
    }

    area.removeFromLeft (10.0f);

    const juce::String wordmark ("SPLINTER");
    const auto wordmarkFont = Theme::font (23.0f, true).withExtraKerningFactor (0.16f);
    const float wordmarkWidth = juce::GlyphArrangement::getStringWidth (wordmarkFont, wordmark) + 4.0f;

    g.setColour (Theme::text);
    g.setFont (wordmarkFont);
    g.drawText (wordmark, area.removeFromLeft (wordmarkWidth), juce::Justification::centredLeft, false);

    area.removeFromLeft (12.0f);
    g.setColour (Theme::textDim);
    g.setFont (Theme::font (13.0f));
    g.drawText ("tempo-synced glitch sequencer", area.withTrimmedRight (static_cast<float> (statusWidth)),
                juce::Justification::centredLeft, true);
}

void SplinterView::paintStatus (juce::Graphics& g) const
{
    auto area = statusArea_.toFloat();
    auto pill = area.removeFromRight (150.0f);
    const bool isSounding = soundingEffect_ != EffectType::None;
    const auto colour = Theme::effectColour (soundingEffect_);
    const float cornerSize = pill.getHeight() * 0.5f;

    g.setColour (isSounding ? colour : Theme::cellOff);
    g.fillRoundedRectangle (pill, cornerSize);

    if (! isSounding)
    {
        g.setColour (Theme::panelOutline);
        g.drawRoundedRectangle (pill.reduced (0.5f), cornerSize, 1.0f);
    }

    const auto ink = isSounding ? Theme::ink : Theme::textDim;
    auto pillContent = pill.reduced (14.0f, 0.0f);
    Theme::drawEffectGlyph (g, soundingEffect_, pillContent.removeFromLeft (18.0f).withSizeKeepingCentre (18.0f, 10.0f), ink);

    const juce::String effectText = isSounding ? juce::String (effectTypeName (soundingEffect_)).toUpperCase()
                                               : juce::String (playheadStep_ < 0 ? "IDLE" : "DRY");
    g.setColour (ink);
    g.setFont (Theme::font (12.5f, true).withExtraKerningFactor (0.1f));
    g.drawText (effectText, pillContent, juce::Justification::centred, false);

    area.removeFromRight (12.0f);
    g.setColour (Theme::textDim);
    g.setFont (Theme::font (13.0f));
    g.drawText (playheadStep_ < 0 ? juce::String ("STEP  -") : "STEP  " + juce::String (playheadStep_ + 1),
                area, juce::Justification::centredRight, false);
}

} // namespace splinter
