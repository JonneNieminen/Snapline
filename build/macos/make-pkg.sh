#!/bin/bash
# Build the macOS installer: one .pkg that installs the Audio Unit and the VST3 for every user.
#
#   build/macos/make-pkg.sh <folder containing Snapline.component and Snapline.vst3> <version> <output.pkg>
#
# Optional environment:
#   PKG_SIGN_IDENTITY   "Developer ID Installer: Name (TEAMID)" - signs the package
#   NOTARY_PROFILE      notarytool keychain profile - notarizes and staples (needs PKG_SIGN_IDENTITY)
# Without PKG_SIGN_IDENTITY the package is unsigned: macOS asks the user to approve it once
# (right-click > Open, or System Settings > Privacy & Security > Open Anyway).
set -euo pipefail

if [[ $# -ne 3 ]]; then
    sed -n '2,11p' "$0" | sed 's/^# \{0,1\}//'
    exit 2
fi
plugins=$(cd "$1" && pwd)
version=$2
output=$3
here=$(cd "$(dirname "$0")" && pwd)

for bundle in Snapline.component Snapline.vst3; do
    [[ -d "$plugins/$bundle" ]] || { echo "error: $plugins/$bundle not found" >&2; exit 1; }
done

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# One payload per format, rooted at /. Relocation is off so the installer never "updates"
# a stray copy it finds elsewhere (a build folder, say) instead of the system folder.
build_component() { # name, bundle, install folder, identifier
    local name=$1 bundle=$2 folder=$3 identifier=$4
    local root="$work/root-$name"
    mkdir -p "$root$folder"
    ditto "$plugins/$bundle" "$root$folder/$bundle"
    local plist="$work/$name.plist"
    pkgbuild --analyze --root "$root" "$plist" >/dev/null
    # One bundle per payload (index 0). The AU and the VST3 share a bundle identifier, so
    # version checks are off: each file is simply put at its path, every time.
    for key in BundleIsRelocatable BundleIsVersionChecked BundleHasStrictIdentifier; do
        /usr/libexec/PlistBuddy -c "Delete :0:$key" "$plist" 2>/dev/null || true
        /usr/libexec/PlistBuddy -c "Add :0:$key bool false" "$plist"
    done
    local scripts=()
    [[ $name == au ]] && scripts=(--scripts "$here/scripts")
    mkdir -p "$work/packages"
    pkgbuild --root "$root" --component-plist "$plist" --identifier "$identifier" --version "$version" \
             --install-location / ${scripts[@]+"${scripts[@]}"} "$work/packages/$name.pkg" >/dev/null
}
build_component au Snapline.component /Library/Audio/Plug-Ins/Components audio.snapline.pitchcorrection.au
build_component vst3 Snapline.vst3 /Library/Audio/Plug-Ins/VST3 audio.snapline.pitchcorrection.vst3

sed "s/@VERSION@/$version/g" "$here/distribution.xml" > "$work/distribution.xml"
sign=()
[[ -n "${PKG_SIGN_IDENTITY:-}" ]] && sign=(--sign "$PKG_SIGN_IDENTITY" --timestamp)
mkdir -p "$(dirname "$output")"
productbuild --distribution "$work/distribution.xml" --resources "$here/resources" \
             --package-path "$work/packages" --version "$version" ${sign[@]+"${sign[@]}"} "$output"

if [[ -n "${NOTARY_PROFILE:-}" ]]; then
    [[ -n "${PKG_SIGN_IDENTITY:-}" ]] || { echo "error: notarization needs PKG_SIGN_IDENTITY" >&2; exit 1; }
    xcrun notarytool submit "$output" --keychain-profile "$NOTARY_PROFILE" --wait
    xcrun stapler staple "$output"
fi
echo "Wrote $output"
