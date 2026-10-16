# Splinter: architecture and behaviour spec

Splinter is a tempo-synced glitch effect. A 16-step sequencer runs in time with the
host. Each step either passes audio through or applies one of six effects to it, and
several effects stretch or bend the audio across the length of the step.

The code is in two layers:

| Layer | Path | Depends on | Purpose |
|---|---|---|---|
| DSP core | `source/dsp/` | C++17 standard library only | All audio processing. Unit tested, renderable offline. |
| Plugin | `source/plugin/` | JUCE 8 + DSP core | Parameters, host transport, state, editor UI. |
| Tests | `tests/` | DSP core | `splinter_tests`, a self-contained test runner (`TestFramework.h`). |
| Render tool | `tools/render/` | DSP core | `splinter_render`: run a WAV file through the engine without a DAW. |

## Signal flow

```
host block ──► GlitchEngine::process
                 1. append dry input to CaptureBuffer (absolute sample indices)
                 2. StepSequencer splits the block at step boundaries
                 3. per segment: the step's effect renders the wet signal
                    (reading from the CaptureBuffer), or dry for an empty step
                 4. crossfade between sources at step boundaries (~3 ms)
                 5. dry/wet mix, output gain
```

### Timing

- One beat = one quarter note. `samplesPerBeat = sampleRate * 60 / bpm`.
- Step length = `stepBeats * samplesPerBeat` samples.
- **Host playing with a PPQ position:** the step index is `floor(ppq / stepBeats) mod numSteps`,
  and step boundaries land on the exact sample where the PPQ crosses a multiple of
  `stepBeats`. A transport jump (loop, seek) re-syncs immediately.
- **Host stopped / no position (e.g. standalone):** the sequencer free-runs from an internal
  sample counter at the host BPM (120 if unknown), starting at step 0 after `reset()`.
- **Chance:** when a non-empty step begins, it fires with probability `chance`. The random
  generator is deterministic (seeded in `reset()`), so offline renders reproduce exactly.

### Effects

Every effect implements `GlitchEffect` (`source/dsp/GlitchEffect.h`); read the contract
there. Effects get their timing from `trigger(StepContext)` and their sound from the
`CaptureBuffer`, which holds the most recent dry input. The buffer is sized for
3x the longest possible step (4 s), so `capacity >= 12 s * sampleRate`.

| Effect | Behaviour over one step |
|---|---|
| **Stutter** | Captures the first `repeatBeats` of the step and loops it until the step ends. Each loop boundary is crossfaded. The pitch glides by `pitchGlideSemitones` over the step (playback rate `2^(st*progress/12)`), and repeats fade down according to `decay`. |
| **Reverse** | Plays the audio immediately before the step start backwards at `speed`: output at step offset `t` = input at `stepStart - 1 - t*speed`. Fades in and out at the edges. |
| **Tape Stop** | A read head starts at the live position and slows to a halt over `duration * stepLength`, following `curve` (linear to exponential). The pitch drops with it, then the output is silent for the rest of the step. |
| **Stretch** | Pitch-preserving granular time stretch. The read position advances at `1/factor` from the step start, while overlapping Hann-windowed grains of `grainMs` play at normal speed. The step's opening audio is smeared across the whole step. |
| **Crusher** | Bit-depth reduction to `bits`, and sample-and-hold every `downsample` samples. |
| **Gate** | Rhythmic gate with a `divisionBeats` cycle, open for `duty` of each cycle, with ~1 ms attack/release ramps so it does not click. |

### Real-time rules (DSP core and plugin)

- No allocation, locks, file I/O, logging or exceptions on the audio thread. Everything is
  sized in `prepare()`.
- Output stays finite for any parameter value inside the documented ranges, and for silence,
  DC, full-scale noise and denormal input.
- Any block size from 1 to `maxBlockSize` gives the same output as one big block (block-size
  independence), apart from floating-point rounding.
- No clicks: discontinuities are crossfaded (step boundaries, stutter loop points, gate edges).

## Plugin parameters

| ID | Name | Range / choices | Default |
|---|---|---|---|
| `mix` | Mix | 0–100 % | 100 |
| `output` | Output | -24–+12 dB | 0 |
| `stepRate` | Step Rate | 1/4, 1/8, 1/8T, 1/16, 1/16T, 1/32 → beats 1, 0.5, 1/3, 0.25, 1/6, 0.125 | 1/16 |
| `steps` | Steps | 1–16 | 16 |
| `chance` | Chance | 0–100 % | 100 |
| `step1`…`step16` | Step N | Off, Stutter, Reverse, Tape Stop, Stretch, Crusher, Gate | pattern below |
| `stutterRepeat` | Stutter Repeat | 1/8, 1/16, 1/32, 1/64, 1/128 → beats 0.5, 0.25, 0.125, 0.0625, 0.03125 | 1/32 |
| `stutterPitch` | Stutter Pitch | -24–+24 st | 0 |
| `stutterDecay` | Stutter Decay | 0–100 % | 0 |
| `reverseSpeed` | Reverse Speed | 0.5–2 x | 1 |
| `tapeStopTime` | Tape Stop Time | 5–100 % of step | 100 |
| `tapeStopCurve` | Tape Stop Curve | 0–100 % | 50 |
| `stretchFactor` | Stretch Factor | 1–16 x (skewed) | 4 |
| `stretchGrain` | Stretch Grain | 10–250 ms | 80 |
| `crusherBits` | Crusher Bits | 1–16 | 6 |
| `crusherDownsample` | Crusher Downsample | 1–64 x | 8 |
| `gateDivision` | Gate Division | same choices as Stutter Repeat | 1/32 |
| `gateDuty` | Gate Duty | 5–95 % | 50 |

Default pattern (16 steps): `Off Off Stutter Off | Off Off Reverse Off | Off Crusher Off Gate | Stretch Stretch TapeStop TapeStop`.

Parameter IDs and choice order are stored in presets and host sessions: **only append**.

## Building

```sh
# DSP core, tests, render tool (fast, no JUCE)
cmake -S . -B build-dsp -DSPLINTER_BUILD_PLUGIN=OFF
cmake --build build-dsp -j
ctest --test-dir build-dsp --output-on-failure

# Plugin (downloads JUCE 8.0.10, or pass -DFETCHCONTENT_SOURCE_DIR_JUCE=/path/to/JUCE)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release -j
```
