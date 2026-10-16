#include "StepGrid.h"

#include "Parameters.h"
#include "PluginProcessor.h"
#include "Theme.h"

#include <algorithm>

namespace splinter
{

namespace
{
    constexpr int refreshRateHz = 30;
    constexpr int stepsPerGroup = 4; // one beat of 1/16 steps
    constexpr float glowMargin = 8.0f;
    constexpr float numberStripHeight = 20.0f;
    constexpr float cellGap = 6.0f;
    constexpr float groupGap = 16.0f;
    constexpr float cornerSize = 6.0f;
    constexpr float outOfPatternAlpha = 0.28f;

    // Menu item IDs after the effect items (1 ... numEffectTypes).
    constexpr int fillAllItemId = 100;
    constexpr int clearAllItemId = 101;
    constexpr int defaultPatternItemId = 102;

    EffectType nextEffect (EffectType type) noexcept
    {
        return static_cast<EffectType> ((static_cast<int> (type) + 1) % numEffectTypes);
    }
}

bool StepGrid::CellState::operator== (const CellState& other) const noexcept
{
    return effect == other.effect && inPattern == other.inPattern && isPlayhead == other.isPlayhead
        && isSounding == other.isSounding && isHovered == other.isHovered;
}

//==============================================================================
StepGrid::StepGrid (SplinterAudioProcessor& processor)
    : processor_ (processor)
{
    auto& state = processor_.getParameterState();

    for (size_t step = 0; step < stepParameters_.size(); ++step)
    {
        stepParameters_[step] = dynamic_cast<juce::AudioParameterChoice*> (state.getParameter (ParamIDs::step (static_cast<int> (step))));
        jassert (stepParameters_[step] != nullptr);
    }

    numStepsParameter_ = dynamic_cast<juce::AudioParameterInt*> (state.getParameter (ParamIDs::steps));
    jassert (numStepsParameter_ != nullptr);

    setTitle ("Step pattern");
    setTooltip ("Click: next effect.   Right-click: choose an effect.   Shift-drag: paint an effect across steps.");

    refresh();
    startTimerHz (refreshRateHz);
}

StepGrid::~StepGrid()
{
    stopTimer();
    finishPainting();

    // The menu uses this component's look and feel, which may go away with the editor.
    if (numOpenMenus_ > 0)
        juce::PopupMenu::dismissAllActiveMenus();
}

//==============================================================================
void StepGrid::timerCallback()
{
    refresh();
}

void StepGrid::refresh()
{
    const int playhead = processor_.getCurrentStep();
    const auto soundingEffect = processor_.getCurrentEffect();
    const int numSteps = numStepsParameter_ != nullptr ? numStepsParameter_->get() : maxSteps;

    for (int step = 0; step < maxSteps; ++step)
    {
        const auto state = readCellState (step, playhead, soundingEffect, numSteps);
        auto& shown = shownCells_[static_cast<size_t> (step)];

        if (state != shown)
        {
            shown = state;
            repaint (cellBounds (step).expanded (glowMargin).getUnion (numberBounds (step)).getSmallestIntegerContainer());
        }
    }

    // Only marked as shown once someone has been told, so a callback set later still hears the current state.
    if ((playhead != shownPlayhead_ || soundingEffect != shownSoundingEffect_) && onPlayheadChanged != nullptr)
    {
        shownPlayhead_ = playhead;
        shownSoundingEffect_ = soundingEffect;
        onPlayheadChanged (playhead, soundingEffect);
    }
}

StepGrid::CellState StepGrid::readCellState (int step, int playheadStep, EffectType soundingEffect, int numSteps) const
{
    CellState state;
    state.effect = stepEffect (step);
    state.inPattern = step < numSteps;
    state.isPlayhead = step == playheadStep;
    state.isSounding = state.isPlayhead && soundingEffect != EffectType::None && soundingEffect == state.effect;
    state.isHovered = step == hoveredStep_;
    return state;
}

//==============================================================================
EffectType StepGrid::stepEffect (int step) const
{
    if (step < 0 || step >= maxSteps)
        return EffectType::None;

    const auto* parameter = stepParameters_[static_cast<size_t> (step)];
    return parameter != nullptr ? static_cast<EffectType> (juce::jlimit (0, numEffectTypes - 1, parameter->getIndex()))
                                : EffectType::None;
}

void StepGrid::setStepEffect (int step, EffectType effect)
{
    // No gesture for a no-op, so hosts do not record an empty undo step.
    if (step < 0 || step >= maxSteps || stepEffect (step) == effect)
        return;

    if (auto* parameter = stepParameters_[static_cast<size_t> (step)])
    {
        parameter->beginChangeGesture();
        parameter->setValueNotifyingHost (parameter->convertTo0to1 (static_cast<float> (effect)));
        parameter->endChangeGesture();
    }

    refresh();
}

void StepGrid::setAllSteps (const std::array<EffectType, maxSteps>& pattern)
{
    for (int step = 0; step < maxSteps; ++step)
        setStepEffect (step, pattern[static_cast<size_t> (step)]);
}

//==============================================================================
void StepGrid::showStepMenu (int step)
{
    const auto current = stepEffect (step);

    juce::PopupMenu menu;
    menu.addSectionHeader ("Step " + juce::String (step + 1));

    for (int type = 0; type < numEffectTypes; ++type)
    {
        const auto effect = static_cast<EffectType> (type);
        const auto colour = effect == EffectType::None ? Theme::textDim : Theme::effectColour (effect);
        menu.addColouredItem (type + 1, effectTypeName (effect), colour, true, effect == current);
    }

    menu.addSeparator();
    menu.addItem (fillAllItemId, "Fill all steps with " + juce::String (effectTypeName (current)));
    menu.addItem (clearAllItemId, "Clear all steps");
    menu.addItem (defaultPatternItemId, "Restore default pattern");
    menu.setLookAndFeel (&getLookAndFeel());

    const auto options = juce::PopupMenu::Options()
                             .withTargetComponent (this)
                             .withTargetScreenArea (localAreaToGlobal (cellBounds (step).getSmallestIntegerContainer()))
                             .withMinimumWidth (180);

    ++numOpenMenus_;

    menu.showMenuAsync (options, [safeThis = juce::Component::SafePointer<StepGrid> (this), step, current] (int result)
    {
        if (safeThis == nullptr)
            return;

        --safeThis->numOpenMenus_;

        if (result >= 1 && result <= numEffectTypes)
        {
            safeThis->setStepEffect (step, static_cast<EffectType> (result - 1));
        }
        else if (result == fillAllItemId || result == clearAllItemId || result == defaultPatternItemId)
        {
            auto pattern = defaultPattern;

            if (result != defaultPatternItemId)
                pattern.fill (result == fillAllItemId ? current : EffectType::None);

            safeThis->setAllSteps (pattern);
        }
    });
}

void StepGrid::paintThrough (int step)
{
    // A fast drag can skip cells between two mouse events, so paint everything in between.
    const int first = std::min (step, lastPaintedStep_);
    const int last = std::max (step, lastPaintedStep_);

    for (int cell = first; cell <= last; ++cell)
    {
        if (stepEffect (cell) == paintEffect_)
            continue;

        auto* parameter = stepParameters_[static_cast<size_t> (cell)];

        if (parameter == nullptr)
            continue;

        auto& gestureOpen = paintGestureOpen_[static_cast<size_t> (cell)];

        if (! gestureOpen)
        {
            parameter->beginChangeGesture();
            gestureOpen = true;
        }

        parameter->setValueNotifyingHost (parameter->convertTo0to1 (static_cast<float> (paintEffect_)));
    }

    lastPaintedStep_ = step;
    refresh();
}

void StepGrid::finishPainting()
{
    for (size_t step = 0; step < paintGestureOpen_.size(); ++step)
    {
        if (paintGestureOpen_[step] && stepParameters_[step] != nullptr)
            stepParameters_[step]->endChangeGesture();

        paintGestureOpen_[step] = false;
    }

    isPainting_ = false;
    lastPaintedStep_ = -1;
}

//==============================================================================
void StepGrid::mouseDown (const juce::MouseEvent& event)
{
    finishPainting(); // in case a previous drag never got its mouseUp

    const int step = stepAt (event.position.x);

    if (event.mods.isPopupMenu())
    {
        showStepMenu (step);
        return;
    }

    if (! event.mods.isLeftButtonDown())
        return;

    if (event.mods.isShiftDown())
    {
        isPainting_ = true;
        paintEffect_ = stepEffect (step);
        lastPaintedStep_ = step;
        return;
    }

    setStepEffect (step, nextEffect (stepEffect (step)));
}

void StepGrid::mouseDrag (const juce::MouseEvent& event)
{
    if (isPainting_)
        paintThrough (stepAt (event.position.x));
}

void StepGrid::mouseUp (const juce::MouseEvent&)
{
    if (isPainting_)
        finishPainting();
}

void StepGrid::mouseMove (const juce::MouseEvent& event)
{
    const int step = stepAt (event.position.x);

    if (step != hoveredStep_)
    {
        hoveredStep_ = step;
        refresh();
    }
}

void StepGrid::mouseExit (const juce::MouseEvent&)
{
    hoveredStep_ = -1;
    refresh();
}

//==============================================================================
void StepGrid::resized()
{
    cellArea_ = getLocalBounds().toFloat().reduced (glowMargin).withTrimmedBottom (numberStripHeight);
}

juce::Rectangle<float> StepGrid::cellBounds (int step) const
{
    constexpr int numGroups = maxSteps / stepsPerGroup;
    constexpr int numCellGaps = maxSteps - numGroups;
    const float cellWidth = (cellArea_.getWidth() - (numGroups - 1) * groupGap - numCellGaps * cellGap) / maxSteps;

    const int groupsBefore = step / stepsPerGroup;
    const int cellGapsBefore = step - groupsBefore;
    const float x = cellArea_.getX() + static_cast<float> (step) * cellWidth
                  + static_cast<float> (cellGapsBefore) * cellGap + static_cast<float> (groupsBefore) * groupGap;

    return { x, cellArea_.getY(), cellWidth, cellArea_.getHeight() };
}

juce::Rectangle<float> StepGrid::numberBounds (int step) const
{
    const auto cell = cellBounds (step);
    return { cell.getX(), cell.getBottom() + 2.0f, cell.getWidth(), numberStripHeight - 2.0f };
}

int StepGrid::stepAt (float x) const
{
    for (int step = 0; step < maxSteps - 1; ++step)
    {
        const float gapAfter = (step + 1) % stepsPerGroup == 0 ? groupGap : cellGap;

        if (x < cellBounds (step).getRight() + gapAfter * 0.5f)
            return step;
    }

    return maxSteps - 1;
}

//==============================================================================
void StepGrid::paint (juce::Graphics& g)
{
    for (int step = 0; step < maxSteps; ++step)
        paintCell (g, step, shownCells_[static_cast<size_t> (step)]);
}

void StepGrid::paintCell (juce::Graphics& g, int step, const CellState& state) const
{
    const auto bounds = cellBounds (step);
    const auto colour = Theme::effectColour (state.effect);
    const bool isOff = state.effect == EffectType::None;
    const float alpha = state.inPattern ? 1.0f : outOfPatternAlpha;

    if (state.isSounding)
    {
        for (int ring = 3; ring >= 1; --ring)
        {
            const float spread = static_cast<float> (ring) * 2.2f;
            g.setColour (colour.withAlpha (0.11f * alpha));
            g.fillRoundedRectangle (bounds.expanded (spread), cornerSize + spread);
        }
    }

    auto fill = isOff ? Theme::cellOff : colour.withMultipliedBrightness (state.isSounding ? 1.0f : 0.8f);

    if (state.isHovered)
        fill = fill.brighter (0.12f);

    fill = fill.withMultipliedAlpha (alpha);
    g.setGradientFill (juce::ColourGradient (fill.brighter (0.08f), bounds.getX(), bounds.getY(),
                                             fill.darker (0.15f), bounds.getX(), bounds.getBottom(), false));
    g.fillRoundedRectangle (bounds, cornerSize);

    if (isOff)
    {
        g.setColour (Theme::panelOutline.withMultipliedAlpha (alpha));
        g.drawRoundedRectangle (bounds.reduced (0.5f), cornerSize, 1.0f);
    }

    if (state.isPlayhead)
    {
        g.setColour (Theme::text.withMultipliedAlpha (state.inPattern ? 0.95f : 0.4f));
        g.drawRoundedRectangle (bounds.expanded (1.5f), cornerSize + 1.5f, 2.0f);
    }

    auto content = bounds.reduced (5.0f, 10.0f);
    const auto labelArea = content.removeFromBottom (16.0f);
    const auto glyphArea = content.withSizeKeepingCentre (std::min (content.getWidth() - 6.0f, 28.0f),
                                                          std::min (content.getHeight(), 18.0f));
    const auto ink = (isOff ? Theme::textFaint : Theme::ink).withMultipliedAlpha (alpha);

    Theme::drawEffectGlyph (g, state.effect, glyphArea, ink);

    g.setColour (ink);
    g.setFont (Theme::font (11.5f, true));
    g.drawFittedText (Theme::effectShortLabel (state.effect), labelArea.getSmallestIntegerContainer(),
                      juce::Justification::centred, 1, 0.7f);

    auto numberArea = numberBounds (step);
    const auto numberColour = state.isPlayhead ? Theme::text : (state.inPattern ? Theme::textDim : Theme::textFaint.withMultipliedAlpha (0.6f));
    g.setColour (numberColour);
    g.setFont (Theme::font (12.0f, state.isPlayhead));
    g.drawText (juce::String (step + 1), numberArea.withTrimmedBottom (4.0f), juce::Justification::centred, false);

    if (state.isPlayhead)
    {
        g.setColour (isOff || ! state.isSounding ? Theme::text.withAlpha (0.6f) : colour);
        g.fillRoundedRectangle (numberArea.removeFromBottom (3.0f).reduced (bounds.getWidth() * 0.25f, 0.0f), 1.5f);
    }
}

} // namespace splinter
