#!/bin/bash
# Remove Snapline (both formats). Double-click in Finder or run in Terminal; asks for your password.
set -e
echo "This removes Snapline from /Library/Audio/Plug-Ins."
sudo rm -rf /Library/Audio/Plug-Ins/Components/Snapline.component /Library/Audio/Plug-Ins/VST3/Snapline.vst3
sudo pkgutil --forget audio.snapline.pitchcorrection.au >/dev/null 2>&1 || true
sudo pkgutil --forget audio.snapline.pitchcorrection.vst3 >/dev/null 2>&1 || true
killall -9 AudioComponentRegistrar 2>/dev/null || true
echo "Snapline is removed."
