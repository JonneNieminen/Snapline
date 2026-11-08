# Contributing to Snapline

Thanks for helping! Snapline is a small project, so the process is light.

## Setup

```bash
git clone --depth 1 --branch 9.0.5 https://github.com/juce-framework/JUCE.git third_party/JUCE
cmake -S . -B build -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release -DSNAPLINE_BUILD_TOOLS=ON
cmake --build build --config Release --target Snapline_AU Snapline_VST3 SnaplineRender SnaplineSnapshot -j 4
```

Copy `build/Snapline_artefacts/Release/AU/Snapline.component` to `~/Library/Audio/Plug-Ins/Components` to try it in Logic or another AU host.

## Before you open a pull request

- Build `Snapline_AU` and `Snapline_VST3` with no new warnings.
- Run `git diff --check`.
- After DSP or preset changes, run `build/SnaplineRender_artefacts/Release/SnaplineRender renders`. It must end with "All checks passed." Listen to the WAV files it writes.
- After interface changes, run `build/SnaplineSnapshot_artefacts/Release/SnaplineSnapshot docs/screenshots` and check both themes.
- Validate the AU with `auval -strict -v aumf SNL1 SNPL`.

CI builds AU and VST3 for Apple Silicon and Intel and the VST3 plus installer on Windows x64, and runs the DSP checks and `auval`. On Windows, configure with `cmake -S . -B build -A x64 -DSNAPLINE_BUILD_TOOLS=ON` and build `Snapline_VST3` and `SnaplineRender` in `Release`.

Read [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) before larger changes; its last section lists the next work.

## Guidelines

- **Real-time safe audio.** No allocation, file I/O, locks, logging, UI calls or parameter lookups by ID on the audio thread.
- **Stable parameters.** Preserve parameter IDs and their meanings; DAW sessions store them.
- **Honest latency.** Studio mode reports its exact delay; keep it that way.
- **Explicit channel and sample-rate handling.** Mono and stereo, 44.1 to 192 kHz.
- **Design tokens only.** Colours live in `Source/DesignTokens.h`; every control has a tooltip. Keep `docs/DESIGN.md` and the screenshots aligned with the interface.
- **Accurate docs.** Keep labels, limitations and latency figures truthful.

## Reporting bugs

Use the bug report template, and include your DAW, sample rate and mode (Live or Studio). Please don't attach copyrighted audio; describe the vocal instead.
