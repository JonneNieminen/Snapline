# Snapline contributor guide

This file is the shared source of project guidance for coding agents. Provider-specific instruction files should point here instead of duplicating rules.

## Project

- JUCE 9.0.5+ and C++20 real-time vocal pitch correction plugin.
- Formats: AU (`aumf`, so it can take MIDI) and VST3 on macOS; VST3 on Windows x64 with an Inno Setup installer (`build/windows/snapline.iss`). No standalone app. CMake owns the build.
- `Source/SnaplineDsp.*`: the engine (YIN tracking → correction → PSOLA resynthesis). It has no JUCE plugin types, so `tools/render` can use it. `Source/PitchDetector.*`: FFT YIN with pYIN-style candidate scoring (it also returns the scored candidates, for Studio's Viterbi). `Source/LpcEnvelope.*`: LPC analysis, envelope warping and lattice filters for the LPC formant methods (plus discrete all-pole and true-envelope fits that only `SnaplineRender --envelopes` uses). `Source/Scales.h`, `Source/Presets.h`: scales and factory presets. `Source/PluginProcessor.*`: parameters, MIDI notes, presets as programs, latency and state. `Source/PluginEditor.*`, `Source/PitchGraph.*`: UI. `Source/DesignTokens.h`: dark and light palettes.
- **System design and next work: `docs/ARCHITECTURE.md`** (read it first). How the engine works: `docs/HOW_IT_WORKS.md`. Look: `docs/DESIGN.md`.
- Design follows Prism (flat macOS style, one accent, dark + light). Theme tokens: `design/theme/tokens.css` ↔ `Source/DesignTokens.h`. Logo masters: `design/logo/make.py` (the in-plugin mark uses the same paths). README art: `design/banners/render.sh` (headless Chrome) → `docs/art`.

## Rules

- Keep `processBlock` and `SnaplineDsp::process` real-time safe: no allocation, file access, mutex waits, UI calls, logging or parameter lookups by ID (use the cached `raw` pointers).
- The engine talks to the UI only through the lock-free display FIFO and atomics.
- Studio latency is reported exactly (`SnaplineRender` checks it). Change `setLatencySamples` only from the message thread (`handleAsyncUpdate`).
- Change parameters through host-aware APIs. Preserve parameter IDs and their meanings for existing sessions; `mode` stays non-automatable.
- Presets change only correction style, never key, scale, input type, mode, MIDI, mix or output.
- Preserve mono/stereo support and sample-rate safety (44.1–192 kHz). Keep code portable to MSVC (Windows VST3).
- Performance budget: about 1 % of one core per instance at 48 kHz; measure before and after DSP changes (`SnaplineRender` prints it).
- Colours come only from `Source/DesignTokens.h`. Every control gets a tooltip. Use Prism as a design reference; keep Snapline's logo and art original.
- Don't use other products' trademarks as control or product names.

## Build

Follow `README.md` to fetch JUCE and build. After DSP changes, configure with `-DSNAPLINE_BUILD_TOOLS=ON`, run `SnaplineRender`, and make sure it ends with "All checks passed." (it includes the voice suite; `--suite` runs that alone, `--file <wav> <out-dir>` measures a real take) After UI changes, run `SnaplineSnapshot --check` and `SnaplineSnapshot docs/screenshots` and check both themes. New controls need a tooltip and an accessibility title (`--check` fails without them). At minimum, after code changes, build `Snapline_VST3` and `Snapline_AU`, then run `git diff --check`. Validate the AU with `auval -strict -v aumf SNL1 SNPL` when plugin behavior changes. CI (`build.yml`) mirrors Axiom: macOS arm64/x86_64 and Windows x64, DSP checks, plugin checks, `auval`, pluginval (strictness 10), installer; `release.yml` is manual (SemVer bump → GitHub release, including the universal macOS `.pkg` from `build/macos/make-pkg.sh`; keep its screens plain-language and one click per step). Don't claim host compatibility beyond what was actually built or checked.
