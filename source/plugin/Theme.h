#pragma once

#include "dsp/EffectParams.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace splinter::Theme
{

// Base palette: dark slate surfaces with light, slightly cool text.
inline const juce::Colour background { 0xff121418 };
inline const juce::Colour backgroundRaised { 0xff181b20 };
inline const juce::Colour panel { 0xff1b1e24 };
inline const juce::Colour panelOutline { 0xff2a2e36 };
inline const juce::Colour cellOff { 0xff23262d };
inline const juce::Colour track { 0xff2c3038 };
inline const juce::Colour knobBody { 0xff262a31 };
inline const juce::Colour text { 0xffe4e7ec };
inline const juce::Colour textDim { 0xff8b919c };
inline const juce::Colour textFaint { 0xff5a606b };
inline const juce::Colour ink { 0xff111317 }; // text drawn on top of a bright effect colour
inline const juce::Colour globalAccent { 0xffc8d0dc };

/** The colour that identifies an effect everywhere in the UI. */
juce::Colour effectColour (EffectType type) noexcept;

/** Short label shown inside a step cell. */
const char* effectShortLabel (EffectType type) noexcept;

/** A small line drawing that identifies the effect (repeats, arrow, slowing curve, ...). */
void drawEffectGlyph (juce::Graphics& g, EffectType type, juce::Rectangle<float> area, juce::Colour colour);

juce::Font font (float height, bool bold = false);

} // namespace splinter::Theme
