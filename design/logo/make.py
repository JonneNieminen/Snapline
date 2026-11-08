"""Snapline logo masters ("Snap"). A wavering sung line settles and snaps onto the
vocal lane, between two faint note lanes. Prism's language: one monoline weight,
round caps, lane dots, deep tile.

python3 design/logo/make.py  ->  SVGs in design/logo/ and docs/logo.svg
The in-plugin mark is drawn from the same geometry in Source/PluginEditor.cpp
(SnaplineAudioProcessorEditor::paintWordmark); keep the two in step.
"""
import pathlib

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent.parent
TILE = "#1F2144"

PALETTES = {
    # line, vocal lane, upper lane, lower lane. Prism's beam and vocal colours; the
    # side lanes are Prism's bass and drums mixed 50 % into the background, solid so
    # the dots don't darken where they meet their lane.
    "dark": ("#F4EFE2", "#E48AA0", "#5E5A94", "#7E6957"),
    "light": ("#24264A", "#C95F78", "#B8B3E2", "#DCC397"),
}

# The sung line: a wobble that decays and lands on y = 32 at x = 33.
WAVE = "M10 32C11.5 22 16.5 22 18 32S23.5 39 25 32S28.8 29 30 32S32 32.8 33 32"
VOCAL = "M33 32H49"
UPPER = "M35 19H49"
LOWER = "M35 45H49"


def symbol(line, vocal, upper, lower, mono=False):
    def stroke(d, c, w, o=1.0):
        op = f' stroke-opacity="{o}"' if o < 1 else ""
        return (f'<path d="{d}" fill="none" stroke="{c}"{op} stroke-width="{w}" '
                f'stroke-linecap="round" stroke-linejoin="round"/>')

    def dot(x, y, c, o=1.0, r=3):
        op = f' fill-opacity="{o}"' if o < 1 else ""
        return f'<circle cx="{x}" cy="{y}" r="{r}" fill="{c}"{op}/>'

    lane_o = 0.45 if mono else 1.0
    return "".join([
        stroke(UPPER, upper, 4.5, lane_o), dot(49, 19, upper, lane_o),
        stroke(LOWER, lower, 4.5, lane_o), dot(49, 45, lower, lane_o),
        stroke(VOCAL, vocal, 4.5), dot(49, 32, vocal, r=3.6),
        stroke(WAVE, line, 4.5),
    ])


def svg(body, view="0 0 64 64", extra=""):
    return f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="{view}" role="img" aria-label="Snapline">{extra}{body}</svg>\n'


dark = symbol(*PALETTES["dark"])
light = symbol(*PALETTES["light"])
mono = symbol(*(["currentColor"] * 4), mono=True)

files = {
    HERE / "snapline-symbol-on-dark.svg": svg(dark),
    HERE / "snapline-symbol-on-light.svg": svg(light),
    HERE / "snapline-symbol-mono.svg": svg(mono),
    # Square tile (README, web, social): full-bleed like Prism's icon.
    HERE / "snapline-icon.svg": svg(dark, extra=f'<rect width="64" height="64" rx="15" fill="{TILE}"/>'),
    # macOS-style icon on the 1024 grid (824 tile, 100 margin) with a soft shadow,
    # for installers, release pages and stores.
    HERE / "snapline-app-icon.svg": svg(
        f'<g transform="translate(100 100) scale(12.875)">{dark}</g>',
        view="0 0 1024 1024",
        extra=(
            '<defs><filter id="s" x="-20%" y="-20%" width="140%" height="140%">'
            '<feDropShadow dx="0" dy="10" stdDeviation="12" flood-color="#000" flood-opacity="0.28"/></filter></defs>'
            f'<rect x="100" y="100" width="824" height="824" rx="185" fill="{TILE}" filter="url(#s)"/>'
        ),
    ),
}
files[ROOT / "docs/logo.svg"] = files[HERE / "snapline-icon.svg"]
for p, s in files.items():
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(s)
print("\n".join(str(p.relative_to(ROOT)) for p in files))
