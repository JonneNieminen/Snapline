# Snapline design

Snapline uses Prism's visual language so the two feel like one family: a flat macOS style with solid surfaces, hairline borders and one calm indigo accent, in dark and light. Snapline adds a few things of its own:

- **Vocal colour** (Prism's vocal stem pink) marks everything that is *corrected*: the output pitch trace, the correction meter, the current note on the keyboard and held MIDI notes. Sung pitch is drawn in the faint grey.
- **Layout**: a sidebar for musical context (preset, key and scale, target notes, input type, MIDI, theme), then the pitch display and four cards (Correction, Vibrato, Character, Output). Every control is a labelled horizontal slider with its value on the right, like Prism's stem rows.
- **Header chips** summarise the scale, voice and mode with its *current* delay.
- **Mark**: a wavering line (the sung pitch) that settles onto a straight line with a dot (the corrected note). The wordmark is lowercase with an accent i-dot, as in Prism.

## Rules

- Colours are defined only in [`Source/DesignTokens.h`](../Source/DesignTokens.h), mirroring Prism's `tokens.css` values.
- No gradients, glass or glows.
- Every control has a tooltip that says what it does in plain words.
- The theme is saved with the session (not a parameter).
- Update `docs/screenshots` with `SnaplineSnapshot` whenever the interface changes.
