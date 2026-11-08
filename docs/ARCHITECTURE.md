# Snapline architecture

The build brief for Snapline: a real-time, organic-sounding vocal pitch corrector shipped as an Audio Unit (macOS) and VST3 (macOS and Windows). Read this together with [`AGENTS.md`](../AGENTS.md) (rules), [`HOW_IT_WORKS.md`](HOW_IT_WORKS.md) (the engine in plain words) and [`DESIGN.md`](DESIGN.md) (the look).

The repository already contains a working first version. **Build on it; don't rewrite it.** Section 9 lists what is done and what to do next.

---

## 1. Product goals

1. **Sounds human by default.** With the *Natural* preset, a listener should hear a better-tuned singer, not an effect: vibrato, scoops, breath and consonants survive; formants never move unless asked.
2. **Can also sound like the effect.** Retune speed 0 gives the hard, stepped sound; *Robot* lets formants follow the pitch.
3. **Real time and light.** Safe to leave on every vocal track while tracking: ≤ 1 % of one Apple-silicon core per instance at 48 kHz, no allocation or locks on the audio thread, a stable latency report.
4. **Honest latency.** Studio mode reports an exact, fixed delay the DAW compensates. Live mode keeps the delay low (8–20 ms) and shows the current figure.
5. **Prism family look.** Flat macOS style, dark and light, one indigo accent, vocal pink for corrected pitch.

Non-goals: polyphonic correction, graphical note editing (Melodyne-style), cloud features, standalone app.

## 2. Platforms, formats and toolchain

| | |
|---|---|
| Framework | JUCE 9.0.5+, C++17, CMake 3.22+ |
| macOS | AU (`aumf` music effect, so it receives MIDI), VST3; arm64 and x86_64 |
| Windows | VST3 x64, Inno Setup installer (`build/windows/snapline.iss`) |
| Codes | Manufacturer `SNPL`, plugin `SNL1`, bundle ID `audio.snapline.pitchcorrection` |
| Buses | Mono→mono or stereo→stereo (`isBusesLayoutSupported`) |
| CI | `.github/workflows/build.yml`: macOS arm64/x86_64 (AU+VST3, DSP and plugin checks, `auval -strict`, pluginval strictness 10 on AU and VST3), Windows x64 (VST3, DSP checks, pluginval strictness 10, installer). `release.yml`: manual SemVer release with macOS plugin zips, the Windows installer and a Windows VST3 zip. Same shape and runners as Axiom/Prism. |

## 3. Module map

```
Source/
  SnaplineDsp.h/.cpp     Engine: tracking → correction → PSOLA resynthesis. No plugin types.
  PitchDetector.h/.cpp   FFT-based YIN estimator (allocation-free after prepare()).
  Scales.h               12-bit note masks, 12 scales + Custom, note names.
  Presets.h              Factory correction styles (never key/scale/mode).
  PluginProcessor.h/.cpp APVTS parameters, cached raw parameter pointers, MIDI note state,
                         presets as programs, latency reporting, state save/restore, theme.
  PluginEditor.h/.cpp    LookAndFeel, Segmented, ParameterRow, NoteStrip, editor layout, preset menu, scaling.
  PitchGraph.h/.cpp      Scrolling pitch display + note/deviation/correction readouts.
  DesignTokens.h         Dark and light palettes (mirrors design/theme/tokens.css).
tools/render             SnaplineRender: offline DSP quality, latency and CPU checks.
tools/snapshot           SnaplineSnapshot: renders the editor (both themes) to PNG; `--check` runs plugin-level tests (presets, state, resizing, titles).
design/                  logo (make.py), banners (art.html + render.sh), theme (tokens.css).
build/windows            Inno Setup script.
```

Dependency rule: `SnaplineDsp` and `PitchDetector` depend only on `juce_core`/`juce_dsp`, so tools and tests link them without the plugin. The UI never touches the engine directly; it reads the processor.

## 4. Threading and data flow

```
Audio thread                                   Message thread (UI, 30 Hz timer)
──────────────────────────────────────────     ─────────────────────────────────
processBlock
  updateHeldNotes(MIDI)  → heldMask (atomic) ─▶ NoteStrip
  readParameters()       (cached atomics)
  SnaplineDsp::process
    analysis → DisplayFrame ─▶ AbstractFifo ──▶ PitchGraph.push / readouts
    effectiveDelay (atomic) ─────────────────▶ header chip "Live · 14 ms"
  latency changed? → triggerAsyncUpdate ─────▶ handleAsyncUpdate → setLatencySamples
  outputPeak (atomic) ───────────────────────▶ meter
                                                Parameter edits → APVTS (host-notifying)
                                                toggleNote → note params + scale=Custom
```

Rules: single-producer/single-consumer FIFO (capacity 4096 frames), atomics with relaxed ordering for display values, `setLatencySamples` only from the message thread, no `String` building or parameter lookups by ID on the audio thread.

## 5. Signal path

All sample positions are absolute `int64` counters into power-of-two rings sized in `prepare()` (≥ 0.4 s + 8 analysis windows).

### 5.1 Input and analysis
- Mono sum (`(L+R)/2`) into the input ring, plus a 2-pole 900 Hz low-pass copy for pitch-mark picking, plus per-channel dry rings for the latency-aligned dry mix.
- Every `hop = fs/375` samples (2.7 ms): YIN on the last `2W` samples, `W = fs/minHz` for the selected **Input type** (Soprano 130–1100 Hz, Alto/Tenor 80–800, Low Male 55–420, Instrument 45–1300).
- Voicing: aperiodicity < 0.25 to start and < 0.35 to continue; RMS > −60 dBFS and within 22 dB of a 0.6 s peak envelope; 30 ms hold bridges dropouts; ×2 / ×½ jumps inside a voiced run are folded back (octave guard).

### 5.2 Correction (per frame)
1. **Note decision**: median of neighbouring frames (Live: last 3; Studio: ±3 with 8-frame look-ahead) → nearest allowed note; 25-cent hysteresis; MIDI *force* overrides.
2. **Raw correction** = target − *reference* (single-frame outliers > 70 ct replaced by the median). The reference is the instantaneous pitch for the first 120 ms of a note and blends to the singer's **slow pitch centre** by 350 ms. The centre is two cascaded 3 Hz one-pole low-passes of the measured pitch, restarted on every onset and note change and kept within 60 ct of the pitch. Vibrato and micro-movement sit around the centre, so they pass through at any retune speed.
3. **Tolerance**: fade the correction out when the singer is more than `50 − 0.45·tolerance` cents (+20 ct soft edge) from the target.
4. **Retune speed**: one-pole smoothing of the *correction* (τ = retune ms; 0 = instant). Smoothing the correction rather than the pitch is what lets vibrato through at slow speeds.
5. **Humanize**: τ grows by up to 250 ms × humanize % on notes held longer than 0.12 s (ramp to 0.52 s).
6. **Onset ramp**: the wanted correction is scaled by a smoothstep over `(20 + 60·humanize %) ms` (× min(1, retune / 20 ms), so 0 ms stays instant) after every voiced onset or note change, so scoops and attacks keep their shape.
7. **No overshoot**: the smoothed correction never exceeds the current error, so a correction built up during a scoop is released as soon as the singer arrives.
8. **Natural vibrato** adds `naturalVibrato % × (measured − centre)`: −100 flattens the singer's own vibrato, 0 keeps it, +100 doubles it. **Amount** scales the correction and this term; synthetic **Vibrato** (depth, rate, onset with 250 ms fade-in) and **Transpose** are added.
7. Output: `shiftCents` per frame, and a `DisplayFrame` for the UI.

### 5.3 Pitch marks
One mark per period at the peak of the low-passed signal within ±P/4 of `last + P`, weighted towards the expected position (keeps a steady phase). The peak is then **refined** within ±0.1 P by normalised cross-correlation of the low-passed waveform around the candidate with the waveform around the previous mark (strided, ≈ 1 k multiply-adds per mark). Waveform shape is far more reliable than a flat, noisy peak on low and breathy voices. Unvoiced regions get marks every `fs/180` samples.

### 5.4 Resynthesis (TD-PSOLA)
- Grain `j`: centred on mark `a_j`, asymmetric Hann halves of `a_j − a_{j−1}` and `a_{j+1} − a_j` (perfect reconstruction at ratio 1).
- Synthesis positions advance by `(a_{j+1} − a_j) / ratio`; the grain for synthesis time `s` is the mark nearest `u = s − latency − extraDelay`.
- **Formant / Throat**: grains are read at `a_j + d·φ` (φ = 2^(formant/12) · 100/throat, × ratio when *Preserve formants* is off; φ clamped to 0.5–2). Grains are stretched (never shortened) so overlap stays ≥ 50 %.
- Overlap-add into `acc`/`win` rings; output = `acc / max(win, 0.5)`; 4-point Hermite interpolation for fractional reads.
- **Voiced/unvoiced edges**: the shift of a voiced grain fades in with a smoothstep over max(5 ms, 1.5 grains) from the nearest unvoiced mark, so grains at the shifted and the unshifted period cross over smoothly instead of switching (no buzz on consonants).
- Ratio from `shiftAt(a_j)`: linear interpolation between decided frames; in Live mode, a short (≤ 15 ms, ±40 ct) extrapolation beyond the newest frame.

### 5.5 Latency model
- **Studio**: fixed `latency = 0.1·fs`, reported exactly; grain sources are always available.
- **Live**: base 8 ms reported; when a grain's source isn't available yet, step back to the newest usable mark minus one period (`extraDelay`), then give delay back at 0.5 % of a period per grain once there are > 2.5 periods of headroom. Typical 8–20 ms.
- Mode changes are applied inside `process` and reported via `AsyncUpdater`; `mode` is not automatable.

### 5.6 Output
Dry (delayed by `latency`, per channel) / wet (mono) mix and output gain, both smoothed (20 ms). **Bypass** crossfades (about 12 ms) from that result to the same latency-delayed dry signal, so bypassing never moves the audio in time relative to what the host compensates (in Live mode the processed path can be a few ms later than the reported 8 ms, so a fade there is a brief comb, not a click). Once bypass is settled, or Mix is 0, the resynthesis is skipped; tracking continues so the display and the retune state stay valid.

## 6. Parameters (IDs are permanent)

| ID | Name | Range / values | Default |
|---|---|---|---|
| `mode` | Mode | Live, Studio (not automatable) | Studio |
| `inputType` | Input Type | Soprano, Alto/Tenor, Low Male, Instrument | Alto/Tenor |
| `key` | Key | C … B | C |
| `scale` | Scale | Chromatic, Major, Minor, Harmonic Minor, Melodic Minor, Major/Minor Pentatonic, Blues, Dorian, Phrygian, Lydian, Mixolydian, Custom | Chromatic |
| `note0`…`note11` | Note C…B | bool (Custom scale) | on |
| `midiMode` | MIDI Notes | Off, Snap to Held Notes, Force to Held Note | Off |
| `retune` | Retune Speed | 0–400 ms (skewed) | 18 |
| `humanize` | Humanize | 0–100 % | 20 |
| `tolerance` | Tolerance | 0–100 % | 0 |
| `amount` | Amount | 0–100 % | 100 |
| `transpose` | Transpose | −12…12 st | 0 |
| `reference` | A4 Reference | 415–466 Hz | 440 |
| `vibratoDepth` / `vibratoRate` / `vibratoOnset` | Vibrato | 0–100 ct / 2–9 Hz / 0–1000 ms | 0 / 5.5 / 250 |
| `naturalVibrato` | Natural Vibrato | −100…+100 % (flatten … keep … double the singer's own vibrato) | 0 |
| `preserveFormants` | Preserve Formants | bool | on |
| `formantShift` | Formant Shift | −6…6 st | 0 |
| `throat` | Throat Length | 60–160 % | 100 |
| `mix` / `output` | Mix / Output | 0–100 % / −24…12 dB | 100 / 0 |
| `bypass` | Bypass | bool; also the host's bypass (`getBypassParameter`) | off |

New parameters get new IDs (version 1 in `juce::ParameterID`). Never rename or reinterpret an existing ID. Non-parameter state (preset index, modified flag, user preset name, theme, editor scale) is stored as properties of the APVTS state tree.

## 7. Interface and theme

- Size 1080 × 760 at 100 %; resizable from 75 to 150 % at a fixed aspect ratio (corner handle, `ComponentBoundsConstrainer`). All controls live in a `holder` component laid out at 1080 × 760 and scaled with one transform; `editor::paint` applies the same scale. The size is saved with the session (`editorScale`). Sidebar 248 px (`sidebar` colour): wordmark, Preset, Key + Scale, Target notes (one-octave `NoteStrip`, click toggles a note and switches to Custom), Input type, MIDI notes, Bypass switch, Dark/Light switch, footer note.
- Main column: header (preset name; chips for scale, voice and mode with live delay; preset description; Live/Studio `Segmented`; output meter), `PitchGraph` card (current target note, sung deviation, ±100 ct correction meter, 5 s of sung (faint) and corrected (vocal) pitch over semitone lanes with target notes highlighted), then four cards: **Correction** (Retune speed, Humanize, Tolerance, Amount), **Vibrato** (Depth, Rate, Onset), **Character** (Formant, Throat, Transpose, Preserve formants switch), **Output** (Mix, Output, A4 tuning).
- Components: `ParameterRow` (label, flat slider, value; double-click resets), `Segmented`, macOS-style switch, flat combo boxes, rounded tooltips. Every control has a plain-language tooltip.
- Tokens: `Source/DesignTokens.h` ↔ `design/theme/tokens.css` (Prism's values + `vocal`, `sung`, lanes, `tile`). No gradients, glass or glows. Mark ("Snap"): `design/logo/make.py`, drawn in-plugin from the same SVG paths.

## 8. Quality, performance and verification

`SnaplineRender` (built with `-DSNAPLINE_BUILD_TOOLS=ON`) must end with **"All checks passed."** on macOS and Windows. Current measured results (synthetic vocal, up to 42 ct off):

| Check | Target | Now |
|---|---|---|
| Hard-tune accuracy (Studio / Live) | mean < 4 ct, max < 15 ct | ≈ 1.1 / 1.3 ct |
| Natural preset | error ≤ ½ of input | 5.5 ct vs 27 ct |
| Studio null at 0 % amount | > 40 dB | ≈ 154 dB |
| Studio latency | reported = measured ± 2 samples | exact |
| Live delay | 8–30 ms | 8–20 ms |
| Level change | < 1.5 dB | < 0.1 dB |
| CPU (one core, 48 kHz) | > 10× real time; goal ≤ 1 % | melody with pauses ≈ 167× (**0.60 %**); continuous voice, no pauses ≈ 141× (**0.71 %**) |

Human-by-default checks (Studio and Live, 44.1 / 48 / 96 kHz; synthetic 40 ct vibrato at 5.6 Hz, scoop 100 ct flat rising in 120 ms, noise bursts abutting notes, breathy low voice at noise 0.6):

| Check | Target | Before | Now |
|---|---|---|---|
| Vibrato kept at retune 5 ms | ≥ 70 % of input depth | 34 % | 116 % |
| `naturalVibrato` −100 % / +100 % | ≤ 30 % / ≥ 150 % | – | ≈ 21 % / ≈ 226 % |
| Scoop dip kept at +30…50 ms (Natural speeds) | ≥ 60 % | 68 % Studio, 43–47 % Live | 92 % / 92–95 % |
| Click index at voiced/unvoiced edges | ≤ 1.5 × input + 1 | 4.2 (input 4.6) | 4.5 (input 4.4–4.6) |
| Period jitter, low breathy voice, pulled 35 ct | ≤ input + 1 % | 0.54 % Studio, 1.00 % Live (input 0.76 %) | 0.53 % / 0.66 % |
| Natural preset error (mean, 48 kHz) | ≤ ½ input | 5.5 ct vs 27 ct | 6.2 ct vs 27 ct (vibrato now kept) |
| Hard tune (mean, 48 kHz) | < 4 ct | 1.06 / 1.12 ct | 1.09 / 1.16 ct |

The tracked vibrato is above 100 % because the two-pole centre leaks about a quarter of a 5.6 Hz vibrato with a phase lag. Live delay is now 8–22 ms (about 2 ms more: the mark refinement looks further ahead). CPU was unchanged (≈ 88× real time at 48 kHz).

CPU, measured by `SnaplineRender` (wall time of the engine alone, Release, one Apple-silicon core, 256-sample blocks, mean of the first run; the continuous-voice figure is the best of three runs). Not an Instruments profile; the plugin wrapper adds a negligible amount per block:

| 48 kHz | Before step 2 | After step 2 |
|---|---|---|
| Melody with pauses (Studio / Live) | 1.14 % / 1.14 % | 0.60 % / 0.59 % |
| Continuous voice (no pauses) | 1.34 % | 0.71 % |
| 44.1 kHz / 96 kHz (melody) | 0.6 % / 2.4 % | 0.6 % / 1.2 % |

Where it went: YIN used one 4096-point FFT size for every input type (now the smallest power of two that fits 2·W: 2048 for Alto/Tenor, 1024 for Soprano); quiet frames skip the FFT; grain windows come from a 1024-entry table (symmetric, so overlapping grains still sum to one) instead of a `cos` per sample; grains at an unchanged formant factor use constant 4-tap Hermite weights (or plain copies when the read position is integral); grains cut from digitally silent input are skipped; with Mix at 0 the resynthesis is skipped (tracking continues for the display). Accuracy, null and latency numbers did not change.

Also run: `auval -strict -v aumf SNL1 SNPL`, `SnaplineSnapshot docs/screenshots` after UI changes, and a listen to the WAVs `SnaplineRender renders` writes.

## 9. Status and next work

**Done:** everything in sections 2–7, the render and snapshot tools, CI and release workflows (Windows jobs not yet run on GitHub), README and docs, branch rules.

**Next (in priority order):**

1. ~~**More human by default**~~ (done: natural vibrato with a `naturalVibrato` parameter, onset ramp, cross-correlation mark refinement, voiced/unvoiced edge fade, and render checks for each; see section 8. Factory presets set `naturalVibrato` to 0 except Hard Tune and Robot (−100). Sessions saved before this change load with the new parameter at 0, so vibrato now passes through at their retune speeds.)
2. ~~**Efficiency**~~ (done: window table, constant-weight or integral reads, silence and `mix = 0` early-outs, minimal YIN FFT per input type; 0.60 % / 0.71 % of one core at 48 kHz, see section 8. An Instruments profile of a host session is still worth doing.)
3. ~~**Completeness**~~ (done: soft `bypass` parameter exposed through `getBypassParameter`; user presets as XML in the application-data folder, saved and loaded on the message thread; resizable editor 75–150 %; accessibility titles for every control. Checked by `SnaplineRender` (bypass) and `SnaplineSnapshot --check` (presets, state, sizes, titles, tooltips). Known gap: the note strip is mouse-only and the Live/Studio and Dark/Light switches take left/right keys but show no focus ring.)

   *User presets*: `~/Library/Application Support/Snapline/Presets/*.xml` on macOS, `%APPDATA%\Snapline\Presets` on Windows. A file is `<SnaplinePreset version="1" name="…">` with one `<Param id value/>` per correction-style parameter (the same 11 a factory preset sets; never key, scale, input type, mode, MIDI, mix, output or bypass). File names are sanitised; unknown IDs in a file are ignored.
4. **Validation**: [pluginval](https://github.com/Tracktion/pluginval) (`v1.0.3`, strictness 10, `--validate-in-process`) is in `build.yml` for AU and VST3 on macOS and VST3 on Windows. **Not yet run:** neither pluginval nor the Windows jobs and installer have run on GitHub (nothing was run from the authoring machine; the workflow YAML was only syntax-checked). Next: watch the first run, fix whatever pluginval reports, and confirm the Windows installer.

Each change: keep the rules in `AGENTS.md`, update `HOW_IT_WORKS.md`, README tables and screenshots when behaviour or UI changes, and keep "All checks passed."
