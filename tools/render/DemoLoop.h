#pragma once

#include <vector>

namespace splinter::render
{

/** A synthetic loop in time with the tempo, so the effect can be heard without an input file:
    a kick on every beat, a noise-burst snare on 2 and 4, 16th-note hats and a two-bar bass line.
    Bar 1 starts at sample 0. Deterministic, and the peak stays at or below -1 dBFS.
    Returns two channels of round (seconds * sampleRate) samples. */
std::vector<std::vector<float>> makeDemoLoop (double sampleRate, double bpm, double seconds);

} // namespace splinter::render
