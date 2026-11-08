# Snapline design

Snapline uses Prism's visual language so the two feel like one family: a flat macOS style with solid surfaces, hairline borders and one calm indigo accent, in dark and light. Snapline adds a few things of its own:

- **Vocal colour** (Prism's vocal stem pink) marks everything that is *corrected*: the output pitch trace, the correction meter, the current note on the keyboard and held MIDI notes. Sung pitch is drawn in the faint grey.
- **Layout**: a sidebar for musical context (preset, key and scale, target notes, input type, MIDI, theme), then the pitch display and four cards (Correction, Vibrato, Character, Output). Every control is a labelled horizontal slider with its value on the right, like Prism's stem rows.
- **Header chips** summarise the scale, voice and mode with its *current* delay.
- **Mark ("Snap")**: a wobbling sung line (Prism's beam colour) that decays and snaps onto the vocal lane, between two quieter note lanes (Prism's bass and drums, mixed into the tile), in Prism's monoline weight with lane dots on the deep `#1F2144` tile. Masters come from `design/logo/make.py`; the plugin draws the same paths. The wordmark is lowercase with an accent i-dot, as in Prism.
- **README art**: `design/banners/art.html` (hero, social preview and four feature cards in dark and light), rendered by `design/banners/render.sh` with Prism's banner palette and layout.

## Rules

- Colours are defined in [`design/theme/tokens.css`](../design/theme/tokens.css) and mirrored in [`Source/DesignTokens.h`](../Source/DesignTokens.h); both use Prism's token values.
- No gradients, glass or glows.
- Every control has a tooltip that says what it does in plain words.
- The theme is saved with the session (not a parameter).
- Update `docs/screenshots` with `SnaplineSnapshot` whenever the interface changes.
