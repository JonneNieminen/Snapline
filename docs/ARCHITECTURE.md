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
| Framework | JUCE 9.0.5+, C++20, CMake 3.22+ (MSVC on Windows: the CI runner's Visual Studio, `windows-2025-vs2026`) |
| macOS | AU (`aumf` music effect, so it receives MIDI), VST3; arm64 and x86_64 |
| Windows | VST3 x64, Inno Setup installer (`build/windows/snapline.iss`) |
| Codes | Manufacturer `SNPL`, plugin `SNL1`, bundle ID `audio.snapline.pitchcorrection` |
| Buses | Mono→mono or stereo→stereo (`isBusesLayoutSupported`) |
| CI | `.github/workflows/build.yml`: macOS arm64/x86_64 (AU+VST3, DSP and plugin checks, `auval -strict`, pluginval strictness 10 on AU and VST3), Windows x64 (VST3, DSP checks, pluginval strictness 10, installer). `release.yml`: manual SemVer release with the macOS installer (`Snapline-x.y.z-macos.pkg`, universal), macOS plugin zips, the Windows installer and a Windows VST3 zip. Same shape and runners as Axiom/Prism. |

## 3. Module map

```
Source/
  SnaplineDsp.h/.cpp     Engine: tracking → correction → PSOLA resynthesis. No plugin types.
  PitchDetector.h/.cpp   FFT-based YIN with pYIN-style candidate scoring; returns the scored candidates too
                         (Studio's Viterbi). Allocation-free after prepare().
  LpcEnvelope.h/.cpp     LPC vocal-tract model: Levinson-Durbin, frequency-warped envelopes, lattice filters;
                         discrete all-pole and true-envelope fits (compared by the render tool, not used by the engine).
  Scales.h               12-bit note masks, 12 scales + Custom, note names.
  Presets.h              Factory correction styles (never key/scale/mode).
  PluginProcessor.h/.cpp APVTS parameters, cached raw parameter pointers, MIDI note state,
                         presets as programs, latency reporting, state save/restore, theme.
  PluginEditor.h/.cpp    LookAndFeel, Segmented, ParameterRow, NoteStrip, editor layout, preset menu, scaling.
  PitchGraph.h/.cpp      Scrolling pitch display + note/deviation/correction readouts.
  DesignTokens.h         Dark and light palettes (mirrors design/theme/tokens.css).
tools/render             SnaplineRender: offline DSP quality, latency and CPU checks; voice suite (--suite), envelope
                         study (--envelopes), real-recording analysis (--file).
tools/snapshot           SnaplineSnapshot: renders the editor (both themes) to PNG; `--check` runs plugin-level tests (presets, state, resizing, titles).
design/                  logo (make.py), banners (art.html + render.sh), theme (tokens.css).
build/windows            Inno Setup script.
build/macos              Installer: `make-pkg.sh` (pkgbuild + productbuild; AU to /Library/Audio/Plug-Ins/Components, VST3 to /Library/Audio/Plug-Ins/VST3), welcome/finish screens, `uninstall.sh`. Optional Developer ID signing and notarization through `PKG_SIGN_IDENTITY` / `NOTARY_PROFILE`.
```

Dependency rule: `SnaplineDsp`, `PitchDetector` and `LpcEnvelope` depend only on `juce_core`/`juce_dsp`, so tools and tests link them without the plugin. The UI never touches the engine directly; it reads the processor.

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
- Mono sum (`(L+R)/2`) into the input ring for analysis, plus a 2-pole 900 Hz low-pass copy for pitch-mark picking, plus per-channel rings (the latency-aligned dry mix, and the sources the grains are cut from). Pitch is tracked on the sum; **each channel is resynthesised** from the shared marks, so the stereo image survives (before, the wet signal was the mono sum on both outputs, which collapsed stereo even at Amount 0).
- Every `hop = fs/375` samples (2.7 ms): YIN on the last `2W` samples, `W = fs/minHz` for the selected **Input type** (Soprano 130–1100 Hz, Alto/Tenor 80–800, Low Male 55–420, Instrument 45–1300).
- Voicing: aperiodicity < 0.25 to start and < 0.35 to continue; RMS > −60 dBFS and within 22 dB of a 0.6 s peak envelope; 30 ms hold bridges dropouts. **pYIN-style candidate scoring** in the detector: every local minimum of d′(τ) is a candidate; with a Beta(2, 18) prior on the YIN threshold, a minimum's probability is the prior mass of the thresholds for which it is the first dip found (`CDF(m_i) − CDF(v_i)`, `m_i` the lowest earlier dip), and the sum is the frame's voiced probability. pYIN's Viterbi pass is collapsed into one online step: candidates are weighted by a continuity prior around the previous voiced pitch (Gaussian, σ = 300 ct, floor 0.15, so real leaps still win). Before, the first dip under 0.15 won. **Multiple-of-period guard** in the YIN detector: if a lag 2–6 times shorter is nearly as periodic (within 0.10, and below 0.35) it wins, because the first dip under the threshold can sit at several periods on breathy or fading notes. **Jump guard** in the engine: inside a voiced run a detection 2–6 times off the previous pitch is folded back (octave and sub-harmonic errors); any other jump of more than 3 semitones is ignored for a frame and believed only if it repeats in the next one (a legato leap does, a glitch does not).
- **Frame time** is where YIN actually measured: d′(τ) over a frame starting at `now − 2W` uses samples `[0, W + τ)`, so the frame is stamped `now − 2W + (W + τ)/2` (unvoiced frames use the last period; times stay increasing). The window centre used before was 4–5 ms late for an alto, so every correction landed that much late.
- **Studio: fixed-lag Viterbi** (Mauch & Dixon 2014). `PitchDetector` returns every scored candidate (pYIN probability, refined lag) as well as the winner. Studio decodes the most likely path over the candidates within 250 ct of the online pitch, plus the online pitch itself and a dropout state (“this frame is wrong, the voice carries on”, log 0.02 a frame). Observations are normalised within the frame. Transitions are −½(Δct/30)², floored at log 10⁻⁴, so a one- to four-frame glitch costs more than dropout and is removed, while a real leap is kept. The path is read back 5 frames (13 ms) after the newest frame; voicing and octave decisions stay with the online tracker. Live keeps the online path.

### 5.2 Correction (per frame)
1. **Note decision**: median of neighbouring frames (Live: last 3; Studio: ±3, on Viterbi-settled frames) → nearest allowed note; 25-cent hysteresis; MIDI *force* overrides.
2. **Raw correction** = target − *reference* (single-frame outliers > 70 ct replaced by the median). The reference is the instantaneous pitch for the first 120 ms of a note and blends to the singer's **pitch centre** by 350 ms. The centre comes from an **extended Kalman filter of centre + vibrato** (`VibratoTracker`; La Scala & Bitmead 1996): state `[c, p, q, ω]`, measured pitch `z = c + p`, `(p, q)` a quadrature oscillator at ω (kept within 4–8 Hz, Prame 1994), the centre a random walk (about a 2 Hz low-pass when there is no vibrato), the depth limited to 120 ct. It is restarted on every onset and note change, and kept within 60 ct plus the vibrato depth of the pitch. The oscillator absorbs the vibrato, so the centre no longer leaks it (the old two cascaded 3 Hz poles let about a quarter of a 5.6 Hz vibrato through, with a phase lag). Vibrato and micro-movement sit around the centre, so they pass through at any retune speed.
3. **Tolerance**: fade the correction out when the singer is more than `50 − 0.45·tolerance` cents (+20 ct soft edge) from the target.
4. **Retune speed**: a critically damped spring-mass-damper on the *correction* (two identical one-pole stages, each τ/2.146, so it reaches 63 % in τ = retune ms like the old one-pole; 0 = instant). It starts with zero slope instead of a kink at every new target and never overshoots. Smoothing the correction rather than the pitch is what lets vibrato through at slow speeds. **Studio runs the second stage backwards** over the look-ahead (12 frames, 32 ms): `H₁(z)H₁(1/z)` has the same magnitude response as `H₁(z)²` and zero phase (forward-backward filtering), so the correction is not late. Attacks keep the forward spring's lag (it lets scoops through): the result blends from the forward to the zero-phase spring over 120–350 ms after an onset or note change, and is limited to between 0 and the frame's full error. The decision is split into a causal half (`decideCorrection`) and this backward half (`finishCorrection`); in Live they run back to back with the forward spring, as before.
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
- **Formant / Throat**: grains are read at `a_j + d·φ` (φ = 2^(formant/12) · 100/throat, × ratio when *Preserve formants* is off; φ clamped to 0.5–2). Grains are stretched (never shortened) so overlap stays ≥ 50 %. This is the **Classic** formant method.
- **LPC formant methods** (`formantMethod`; `LpcEnvelope.*`). An LPC frame is fitted every 2 hops (5.3 ms) on a Hann window of two periods of the lowest pitch for the input type (10–32 ms), pre-emphasised (0.97), with a 60 Hz Gaussian lag window and a −40 dB noise floor; order 16 up to 48 kHz, scaled above (24 at 72 kHz, 32 at 96 kHz and up). Knots: the window centre in Studio, `now + 2 hops` in Live (causal). Coefficients are kept and interpolated as reflection coefficients (every 8 samples), which keeps every intermediate filter stable. Frames more than 40 dB under the recent voice repeat the last envelope.
  - *LPC envelope*: grains are never resampled. While Formant, Throat or Preserve-off would move the envelope, the PSOLA output is whitened by its source envelope `A_u` and re-coloured by `g′/A′_u`, where `A′` is the envelope sampled at `ω/φ` on 256 bins, turned back into an autocorrelation and refitted, and `g′ = √(E′/E)`. Otherwise the filters are off and the output is Classic's, sample for sample (checked to > 230 dB).
  - *LPC source-filter*: the input is inverse-filtered to its residual `e = A_t x` as it arrives (per channel, shared envelope); PSOLA cuts grains from the residual; the output is re-coloured with `g′/A′_u`. The envelope function of time is the same on both sides, so at φ = 1 the pair is an exact inverse (Studio null ≈ 100–115 dB).
  - `u` is the true source time of each output sample: grains accumulate `w · (synthesis centre − source centre)` next to their weights, so Live catch-ups and PSOLA's repeated or skipped periods are followed exactly.
  - A guard limits the re-coloured output to +6 dB over the short-term (3 ms) power of the input it came from, with one gain for both channels; it only acts where grains from both sides of a sudden envelope change overlap (the first onset after silence) and is inactive in normal operation.
- Overlap-add into `acc`/`win` rings; output = `acc / max(win, 0.5)`; 4-point Hermite interpolation for fractional reads.
- **Voiced/unvoiced edges**: the shift of a voiced grain fades in with a smoothstep over max(5 ms, 1.5 grains) from the nearest unvoiced mark, so grains at the shifted and the unshifted period cross over smoothly instead of switching (no buzz on consonants).
- Ratio from `shiftAt(a_j)`: linear interpolation between decided frames; in Live mode, a short (≤ 15 ms, ±40 ct) extrapolation beyond the newest frame.

### 5.5 Latency model
- **Studio**: fixed `latency = 0.1·fs`, reported exactly; grain sources are always available. Decisions are made `lookahead` frames late: `lookahead·hop ≤ latency − 1.5 W − longest period − 2 hops`, capped at 20 frames: 20 (53 ms) for Soprano and Alto/Tenor, 18 for Low Male, 14 for Instrument. Of those, 5 settle the Viterbi path, 3 feed the median and up to 12 the backward retune stage (fewer for Low Male and Instrument). The block size does not matter: a block is fully analysed before its grains are placed.
- **Live**: base 8 ms reported; when a grain's source isn't available yet, step back to the newest usable mark minus one period (`extraDelay`), then give delay back only in unvoiced sound and pauses (25 % of a grain per grain, once there are > 2.5 periods of headroom) and never below `delayFloor`, the largest delay the voice needed lately, which decays over about 4 s. Giving delay back during a note stretches the audio (the old 0.5 %/grain was a steady 8 ct detune) and was the main source of Live-mode damage. Typical 8–22 ms, mean about 13 ms on a real vocal.
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
| `retune` | Retune Speed | 0–400 ms (skewed) | 85 |
| `humanize` | Humanize | 0–100 % | 45 |
| `tolerance` | Tolerance | 0–100 % | 35 |
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
| `formantMethod` | Formant Method | Classic, LPC Envelope, LPC Source-Filter (version 2, appended last; not part of presets) | Classic |

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
| Hard-tune accuracy (Studio / Live) | mean < 4 ct, max < 15 ct | ≈ 0.4 / 0.4 ct (spread 1.6 ct) |
| Natural preset | error ≤ ½ of input | 5.2 ct vs 27 ct |
| Studio null at 0 % amount | > 40 dB | ≈ 154 dB |
| Studio latency | reported = measured ± 2 samples | exact |
| Live delay | 8–30 ms | 8–20 ms |
| Level change | < 1.5 dB | < 0.1 dB |
| CPU (one core, 48 kHz) | > 10× real time; goal ≤ 1 % | melody with pauses ≈ 158× (**0.63 %**); continuous voice, no pauses ≈ 133× (**0.75 %**) (re-measured for round 2, same before and after it; earlier runs gave 0.60 / 0.71 %) |

Human-by-default checks (Studio and Live, 44.1 / 48 / 96 kHz; synthetic 40 ct vibrato at 5.6 Hz, scoop 100 ct flat rising in 120 ms, noise bursts abutting notes, breathy low voice at noise 0.6):

| Check | Target | First version | Before round 2 | Now (48 kHz, Studio / Live) |
|---|---|---|---|---|
| Vibrato kept at retune 5 ms | ≥ 70 % of input depth | 34 % | 116 % | 100 % / 99 % |
| `naturalVibrato` −100 % / +100 % | ≤ 30 % / ≥ 150 % | – | 21 % / 226 % | 6 % / 198 % |
| Scoop dip kept at +30…50 ms (Natural speeds) | ≥ 60 % | 68 % Studio, 43–47 % Live | 97 % / 99 % | 95 % / 98 % |
| Click index at voiced/unvoiced edges | ≤ 1.5 × input + 1 | 4.2 (input 4.6) | 4.4 / 4.5 (input 4.6) | 4.7 / 4.6 (input 4.6) |
| Period jitter, low breathy voice, pulled 35 ct | ≤ input + 1 % | 0.54 % / 1.00 % (input 0.76 %) | 0.53 % / 0.66 % | 0.46 % / 0.72 % |
| Natural preset error (mean) | ≤ ½ input | 5.5 ct vs 27 ct | 5.7 / 5.6 ct | 5.2 / 5.3 ct |
| Hard tune (mean) | < 4 ct | 1.06 / 1.12 ct | 1.09 / 1.14 ct (spread 5.3 ct) | 0.38 / 0.40 ct (spread 1.6 ct) |

Round 2 is described below (“Engine improvements 1–7”). The tracked vibrato used to be above 100 % because the two-pole centre leaked about a quarter of a 5.6 Hz vibrato; the Kalman centre removed that, and the YIN-centred frame time removed most of the vibrato left at −100 %.

CPU, measured by `SnaplineRender` (wall time of the engine alone, Release, one Apple-silicon core, 256-sample blocks, mean of the first run; the continuous-voice figure is the best of three runs). Not an Instruments profile; the plugin wrapper adds a negligible amount per block:

| 48 kHz | Before step 2 | After step 2 |
|---|---|---|
| Melody with pauses (Studio / Live) | 1.14 % / 1.14 % | 0.60 % / 0.59 % |
| Continuous voice (no pauses) | 1.34 % | 0.71 % |
| 44.1 kHz / 96 kHz (melody) | 0.6 % / 2.4 % | 0.6 % / 1.2 % |

Where it went: YIN used one 4096-point FFT size for every input type (now the smallest power of two that fits 2·W: 2048 for Alto/Tenor, 1024 for Soprano); quiet frames skip the FFT; grain windows come from a 1024-entry table (symmetric, so overlapping grains still sum to one) instead of a `cos` per sample; grains at an unchanged formant factor use constant 4-tap Hermite weights (or plain copies when the read position is integral); grains cut from digitally silent input are skipped; with Mix at 0 the resynthesis is skipped (tracking continues for the display). Accuracy, null and latency numbers did not change.

**Real-recording analysis** (`SnaplineRender --file <wav> [out-dir] [soprano|alto|low|instrument]`, offline, also writes the processed WAVs into `out-dir`, which must not be inside the repository): spectral distance, 1/3-octave envelope distance (does the tone survive), short-time level flutter, YIN roughness, pitch micro-movement, tracker glitch count and Live delay statistics for Amount 0, Pop, Natural and a chromatic Pop, in Studio and Live. Live is aligned block by block at the delay the engine reports. Use the song's own key: tuning to the wrong key makes Pop look much worse than it is. Results from private recordings are kept out of the repository.

Synthetic regression checks added with it (`SnaplineRender`): stereo with differing channels (R = −0.6 × L delayed 2 ms): before, Amount 0 nulled at 7 / 2 dB and the side signal was gone (−234 dB side/mid); after, 148 dB per channel and the width is unchanged. Detector on noisy harmonic frames: before, 13–19 % of accepted frames were off by more than 120 ct (multiples of the period); after, about 2 % (the check allows 4 %).

Also run: `auval -strict -v aumf SNL1 SNPL`, `SnaplineSnapshot docs/screenshots` after UI changes, and a listen to the WAVs `SnaplineRender renders` writes.

**Engine upgrade (pYIN scoring, retune spring, LPC formant methods)**. Measured with `SnaplineRender`, 48 kHz unless noted:

| Check | Before | After |
|---|---|---|
| Detector on noisy frames (accepted frames off by > 120 ct, 44.1 / 48 / 96 kHz) | 26 / 31 / 36 of 1356 | 0 / 0 / 0 (check now allows 1 %) |
| Real vocal, Pop: tracker one-off spikes | 8 | 4 (0 octave jumps before and after) |
| Natural preset error (Studio / Live) | 6.2 / 6.4 ct | 5.7 / 6.1 ct |
| Scoop shape kept (Studio / Live) | 92 % / 92 % | 97 % / 97 % |
| Hard tune, vibrato, clicks, jitter, null, latency, CPU (Classic) | – | unchanged |

LPC methods (new checks at 44.1 / 48 / 96 kHz, Studio and Live): hard tune 1.1–1.8 ct, level within 0.1 dB, peaks as Classic; ±4 st formant moves the spectral centroid 578 → 683 → 807 Hz with pitch error ≤ 1.5 ct; *Preserve formants* off makes formants follow a +5 st transpose; odd block sizes and a mono bus; Studio null at 0 % amount 148 dB (envelope) and 102–115 dB (source-filter). CPU at 48 kHz: envelope 0.6 % (Classic's) with nothing moved and about 1.1 % while moving; source-filter about 1.0–1.1 % (2.5 % at 96 kHz, order 32). On the private real-vocal take, Pop with source-filter measures the same as Classic (spectral, envelope, flutter and roughness within 0.03); with a +3 st formant shift both LPC methods are cleaner than Classic (roughness 0.030 vs 0.032, flutter and spectral distance lower), because they move only the resonances while Classic also rescales the glottal pulse (so the centroid moves less: +18 % vs +27 % for +4 st on the synthetic voice).

Tried and dropped: a 5–6 ms Live base delay. On the real vocal the mean Live delay only fell from 12.4 to 11.5 ms (the voice, not the base, sets it), while catch-ups rose: Live Pop spectral distance 2.98 → 3.27 dB, envelope distance 3.10 → 3.41 dB, roughness 0.026 → 0.027, hard tune at 44.1 kHz 1.74 → 2.15 ct. The base stays at 8 ms. Live's floor is physical: a PSOLA grain needs about one period of input after its centre and the tracker about two periods, so sub-5 ms is only possible for voices above ~400 Hz.

**Engine improvements 1–7 (round 2).** All numbers from `SnaplineRender` at 48 kHz (Release, Apple silicon) and the private real take (`--file`, F# major, 232–557 Hz, Alto/Tenor), unless noted. Kept only where the measurement beat the state before.

*7. Yardstick (kept).* `SnaplineRender` now also runs a **voice suite** (`--suite` runs it alone): `makeVoice` adds a pressed/belt source (harmonics 1/k^0.55), vocal fry (3 % period jitter, 15 % shimmer, alternating cycles at 35 %), breath that follows the glottal cycle, aspiration after the tract, and a harmonic ceiling; voices are alto, low male (~80–112 Hz), soprano (~430–590 Hz), breathy 0.6 and 0.9 (harmonics stop at 4 kHz, so > 5 kHz is breath only), fry and belt, each in Studio and Live at Amount 0, Pop, hard tune and hard tune + 5 st. New metrics, per voice and as means: harmonics-to-noise ratio in → out (Boersma 1993: `HNR = 10 log10 (r / (1 − r))`, `r` the normalised correlation one period apart), periodicity per band (< 1 k, 1–3 k, > 3 k), **aspiration periodicity** in the breath-only band (input 0.06), period jitter, envelope distance, click index on the raw output and roughness, with loose guards on the means. The real-take report adds HNR, per-band periodicity, the distance to the nearest scale note in and out (“off-note”), the mean applied correction and Pop + 5 st configs. `--envelopes` runs the envelope study of item 5. What the suite showed before any engine change: normal corrections barely periodise breath (Pop: air 0.062 → 0.063), + 5 st does (0.062 → 0.176 Studio, 0.163 Live; > 3 kHz 0.68 → 0.74); the low voice's 40 ct vibrato crossing an E/F boundary flips the note (9.9 ct hard-tune error); breath 0.9 is mostly unvoiced (≈ 20 ct, the documented limit).

*3. Vibrato-aware centre (kept), plus YIN-centred frame times (kept).* Vibrato kept 116 → 100 % (Studio) / 99 % (Live); −100 % 21 → 6 %; +100 % 226 → 198 %; hard tune 1.09 / 1.14 → 0.38 / 0.40 ct (spread 5.3 → 1.6 ct); Natural 5.7 / 5.6 → 5.3 / 5.3 ct; suite mean hard tune 6.48 / 6.77 → 4.83 / 5.26 ct; scoop 97 / 99 → 95 / 98 %; jitter 0.53 / 0.66 → 0.46 / 0.72 % (input 0.76 %). Real take: off-note after Pop 24.5 → 22.7 ct (Studio), 25.0 → 23.5 ct (Live); Live Pop roughness 0.026 → 0.027, envelope distance 3.10 / 1.40 / 0.88 → 3.47 / 1.51 / 0.93 dB (the correction applied grew 16.2 → 16.8 ct, which raises these distances too). The Kalman filter alone fixed the vibrato numbers (100 %, −100 % still 22 %); the frame time alone fixed the −100 % residue and gave the off-note gain. CPU unchanged (0.63 % melody, 0.75 % continuous). *Tried and dropped:* deciding the note on the Kalman centre (suite 6.48 → 5.22 ct, but the real take got worse, 24.5 → 28.7 ct off-note: the centre lags legato semitone steps); deciding on the pitch minus the tracked vibrato (the oscillator also soaks up slides: hard tune max 101 ct); no restart of the centre on note changes; natural vibrato from the oscillator only (suite 3.95 → 4.24 ct).

*4. Studio look-ahead (kept).* Look-ahead 8 → 20 frames (53 ms; 18 Low Male, 14 Instrument); the reported 100 ms latency is unchanged (measured = reported, null 148 dB). Kept: the fixed-lag Viterbi (lag 5) and the zero-phase retune stage blended in after attacks. Real take, Studio Pop: tracker one-off spikes 4 → 1 (0 octave jumps); off-note 22.7 → 22.6 ct while the correction applied fell 16.8 → 14.4 ct; pitch micro-movement 22.1 → 19.5 ct (input 18.1–19.4); spectral distance 1.43 / 3.25 / 3.87 → 1.37 / 3.07 / 3.65 dB, envelope distance 2.89 / 1.35 / 0.85 → 2.86 / 1.25 / 0.76 dB; chromatic Pop off-note 15.9 → 15.1 ct; Natural 31.6 → 31.5 ct. Synthetic: Natural 5.3 → 5.2 ct, scoop 95 %, hard tune and suite unchanged. CPU unchanged. *Tried and dropped:* a Viterbi free to choose any candidate (a note started on its sub-octave: one new octave jump), with raw probabilities (604 frames lost to dropout on breathy passages), or with lag 17 (same as lag 5); a 40 ms forward note decision at onsets (off-note 22.8 → 22.4 ct alone, nothing once zero-phase used the look-ahead); a fully zero-phase spring (off-note 19.6–20.0 ct, chromatic 12.9–13.1, Natural 30.3, but scoops kept only 58–64 %, failing the 60 % check); the onset ramp applied after smoothing (51–56 %); ramp after + blend (20.9 ct, scoop 76 %).

*1. Glottal-closure pitch marks (dropped).* A SEDREAMS-style detector (Drugman & Dutoit 2009; Drugman et al. 2012): Blackman mean-based signal over 1.75 periods (strided at fs / 4000 on the 900 Hz low-pass), the LP residual of an order-16 fit refitted every 10.7 ms, the closure as the largest residual sample in `[minimum, + 0.35 P]` with the residual's skew as polarity. Plain: breathy 0.6 + 5 st tuning 2.7 → 277 ct, suite hard tune 4.83 → 11.5 ct, real Live Pop roughness 0.027 → 0.052, HNR 20.54 → 18.29 dB, Live jitter 0.72 → 0.90 % (1.23 % at 44.1 kHz), CPU 0.63 → 0.78 %. With polarity fixed per voiced run and a prominence gate (peak > 3 × the interval's RMS, otherwise the old mark): best Live Pop roughness 0.031, HNR 19.98 dB, suite hard tune 10.4 ct, soft-bypass click check failed. On breathy voices the residual is noise and its peaks are anywhere; mixing two phase anchors in one note adds roughness. The peak + cross-correlation marks stay.

*2. Harmonic + noise split (dropped).* A pitch-synchronous comb along the marks (`h = x(t−)/4 + x/2 + x(t+)/4`, `n = x − h`, after HNM: Laroche, Stylianou & Moulines 1993; Stylianou 2001), `n` above 2 kHz added back unshifted at each output sample's source time, Studio only. Ungated, it lowered + 5 st aspiration periodicity 0.169 → 0.116 and breathy 0.6 > 3 kHz periodicity 0.29 → 0.22 (HNR 17.3 → 16.4 dB, input 15.7), but raised it for ordinary corrections (Pop air 0.064 → 0.075: the comb's estimate of pure noise is itself correlated one period apart) and failed the click guard at + 5 st (5.1 → 8.1). Gated to shifts of 50–300 ct and up (5 ms fades) it left ordinary corrections untouched, cost + 0.03 % CPU, and kept the + 5 st gain. On the real take at Pop + 5 st it was worse: > 5 kHz periodicity 0.643 → 0.577 and > 3 kHz 0.781 → 0.737 (input 0.602 / 0.761), 1–3 kHz 0.917 → 0.906, HNR 19.75 → 18.74 dB (input 20.84), roughness 0.031 → 0.036, high-band flutter 0.35 → 0.42: real voices have harmonics above 3 kHz, with jitter and vibrato, that leak into the unshifted part. A real breathy recording is needed before trying again (with a breathiness gate).

*5. Envelope for high voices (engine unchanged; study kept as `SnaplineRender --envelopes`).* Additive steady vowels from a five-formant all-pole tract (700 / 1220 / 2600 / 3300 / 3750 Hz, and 15 % higher), harmonics to 0.45 fs, one Soprano-length frame; RMS dB error of the all-pole envelope against the true one over `[max (150, 0.8 f0), 5 kHz]`:

| f0 (Hz) | 130 | 200 | 300 | 450 | 650 | 1000 | mean |
|---|---|---|---|---|---|---|---|
| LPC (now), tract ×1.0 / ×1.15 | 10.3 / 5.8 | 10.3 / 5.9 | 10.6 / 6.2 | 10.5 / 6.1 | 10.7 / 6.1 | 11.7 / 8.1 | 8.53 |
| True envelope (Röbel & Rodet 2005) | 7.1 / 5.3 | 7.6 / 6.3 | 8.0 / 6.3 | 8.8 / 6.7 | 11.2 / 7.3 | 13.9 / 8.5 | 8.08 |
| Discrete all-pole (El-Jaroudi & Makhoul 1991) | 6.8 / 3.8 | 6.8 / 3.9 | 6.9 / 3.9 | 8.7 / 5.8 | 9.8 / 6.2 | 10.9 / 8.1 | 6.80 |

Cost per frame: LPC 5 µs, DAP 16–27 µs, true envelope 0.8–1.3 ms (about 20 % of a core at the LPC frame rate). DAP needs at least p + 1 harmonics and drops to a lower order above that (at 1000 Hz only 5–6 harmonics are within 90 dB). End to end (Formant + 4 st on the same vowel, error of each harmonic's level change against `|H(f/φ)| / |H(f)|`, harmonics ≤ 4 kHz within 40 dB): LPC 6.23 / 6.28 / 6.44 / 4.45 / 13.31 dB at 200 / 300 / 450 / 650 / 900 Hz (mean 7.34), DAP 5.82 / 6.25 / 7.56 / 5.74 / 10.00 (7.07): better on average, worse at 450 and 650 Hz, so not integrated. (On makeVoice's parallel three-formant tract all-pole fits are equal, ≈ 4.3–7 dB: that tract has zeros between its formants.) Classic is unchanged.

*6. Prediction-based Live latency (prototype, dropped; no new mode).* Waveform substitution (Goodman et al. 1986): while voiced, the newest period (lag refined by normalised cross-correlation within 0.9–1.1 P, NCC ≥ 0.6) is repeated one period past `now`, and marks and grains may use it. Mean Live delay on the real take (Amount 0 / Pop): now 12.4 / 12.9 ms; with prediction 12.3 / 13.2 ms; prediction + 5 ms base 12.7 / 12.9 ms; 5 ms base alone 13.1 / 11.8 ms. Suite Live mean delay 18.4–18.7 → 18.0–18.1 ms (17.5–17.8 with a 5 ms base), while Live jitter rose 0.76 → 1.24 % (44.1 kHz) and the suite's Live click index 4.6–5.1 → 5.4–6.1. Live's delay is set by catch-ups at onsets, where there is no period to extrapolate yet, and is given back only in pauses, so extrapolating inside steady notes cannot lower it.

## 9. Status and next work

**Done:** everything in sections 2–7, the render and snapshot tools, CI and release workflows (Windows jobs not yet run on GitHub), README and docs, branch rules.

**Next (in priority order):**

1. ~~**More human by default**~~ (done: natural vibrato with a `naturalVibrato` parameter, onset ramp, cross-correlation mark refinement, voiced/unvoiced edge fade, and render checks for each; see section 8. Factory presets set `naturalVibrato` to 0 except Hard Tune and Robot (−100). Sessions saved before this change load with the new parameter at 0, so vibrato now passes through at their retune speeds.)
2. ~~**Efficiency**~~ (done: window table, constant-weight or integral reads, silence and `mix = 0` early-outs, minimal YIN FFT per input type; 0.60 % / 0.71 % of one core at 48 kHz, see section 8. An Instruments profile of a host session is still worth doing.)
3. ~~**Completeness**~~ (done: soft `bypass` parameter exposed through `getBypassParameter`; user presets as XML in the application-data folder, saved and loaded on the message thread; resizable editor 75–150 %; accessibility titles for every control. Checked by `SnaplineRender` (bypass) and `SnaplineSnapshot --check` (presets, state, sizes, titles, tooltips). Known gap: the note strip is mouse-only and the Live/Studio and Dark/Light switches take left/right keys but show no focus ring.)

   *User presets*: `~/Library/Application Support/Snapline/Presets/*.xml` on macOS, `%APPDATA%\Snapline\Presets` on Windows. A file is `<SnaplinePreset version="1" name="…">` with one `<Param id value/>` per correction-style parameter (the same 11 a factory preset sets; never key, scale, input type, mode, MIDI, mix, output or bypass). File names are sanitised; unknown IDs in a file are ignored.
4. **Validation**: [pluginval](https://github.com/Tracktion/pluginval) (`v1.0.3`, strictness 10, `--validate-in-process`) is in `build.yml` for AU and VST3 on macOS and VST3 on Windows. **Not yet run:** neither pluginval nor the Windows jobs and installer have run on GitHub (nothing was run from the authoring machine; the workflow YAML was only syntax-checked). Next: watch the first run, fix whatever pluginval reports, and confirm the Windows installer.

5. ~~**Engine upgrade**~~ (done: pYIN-style candidate scoring, critically damped retune spring, `formantMethod` with LPC envelope and LPC source-filter, C++20; see section 8.)
6. ~~**Engine improvements 1–7**~~ (round 2, section 8: voice suite and artifact metrics; Kalman vibrato-aware centre and YIN-centred frame times; Studio look-ahead 53 ms with a fixed-lag Viterbi and a zero-phase retune stage. Tried and dropped: glottal-closure marks, a harmonic + noise split, DAP / true-envelope fits in the engine, Live prediction.)
7. **Real recordings.** Only one real take (an alto/tenor pop vocal) has been measured. Wanted: a low male voice, a soprano, a breathy take and a belted take, to check round 2 on real voices and to revisit the harmonic + noise split (with a breathiness gate) and DAP for high voices.
8. **Windows.** Round 2 has been built and checked on macOS arm64 only; the MSVC build and the Windows DSP checks run in CI.

Each change: keep the rules in `AGENTS.md`, update `HOW_IT_WORKS.md`, README tables and screenshots when behaviour or UI changes, and keep "All checks passed."
