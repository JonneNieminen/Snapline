# How Snapline works

Snapline corrects a monophonic vocal in three stages: it **tracks** the pitch, **decides** where that pitch should be, and **resynthesizes** the voice at the new pitch. All three run inside the audio callback with memory allocated up front.

## 1. Tracking

- The input is summed to mono and analysed 375 times a second (a hop of about 2.7 ms).
- Each frame runs [YIN](https://audition.ens.fr/adc/pdf/2002_JASA_YIN.pdf): the difference function is computed with an FFT cross-correlation, normalised, and the first dip under 0.15 is refined with parabolic interpolation.
- **Input type** sets the search range (Soprano 130–1100 Hz, Alto/Tenor 80–800 Hz, Low Male 55–420 Hz, Instrument 45–1300 Hz) and the window length.
- A frame counts as voiced when it is periodic enough, above −60 dBFS and within 22 dB of the recent vocal level. Short dropouts of up to 30 ms inside a note are bridged, and sudden factor-of-two jumps are treated as octave errors.

## 2. Correction

For every voiced frame:

1. A median over neighbouring frames picks the **target note**: the nearest allowed note from the key and scale, the custom notes, or the held MIDI notes. *Force to held note* uses the held note's exact octave. A 25-cent hysteresis stops wobbles from flipping between two notes.
2. The **correction** is the distance from the note to where the singer *is*. For the first 120 ms of a note that is the instantaneous pitch; after that it is the singer's slow pitch centre (a 3 Hz low-pass), so the vibrato around the centre is not corrected away. *Tolerance* fades the correction out when the singer is far from the note.
3. *Retune speed* smooths the correction with a one-pole filter (time constant = the retune time; 0 ms = instant). *Humanize* lengthens that time on notes held longer than about 120 ms. After every onset or note change the correction also fades in over 20-80 ms (longer with Humanize), so a scoop keeps its shape, and it never overshoots the note.
4. *Natural vibrato* then scales the singer's own vibrato: -100 % flattens it, 0 % keeps it, +100 % doubles it. *Amount* scales the correction; *Vibrato* and *Transpose* are added on top.

**Studio** mode decides each frame 8 frames (about 21 ms) late, so the note decision can look ahead. **Live** mode decides at once and briefly extrapolates the correction for audio that is newer than the last analysed frame.

## 3. Resynthesis (TD-PSOLA)

- **Pitch marks** are placed once per period, on the peak of a low-passed copy of the signal near where the next period is expected. Unvoiced sound gets marks every 5.6 ms. Each mark is refined by cross-correlating the waveform with the one before it, which keeps low and breathy voices from turning rough. Near the edge of a voiced sound the shift fades in over about 5 ms, so consonants do not buzz.
- Each output **grain** is two periods long, centred on a mark and Hann-windowed. Grains are overlap-added at the *target* period, so the pitch changes while each grain keeps the vocal tract's response: the formants stay where they were.
- **Formant** and **Throat** resample the grains themselves (factor 0.5–2), which moves the formants without changing the pitch. With *Preserve formants* off, the grains are also resampled by the pitch ratio, so formants follow the pitch.
- When correction is zero, grains are laid exactly where they came from and the output reconstructs the input (the Studio null test reaches about −150 dB).

### Delay

A grain can only be built once a full period after its centre has arrived. **Studio** delays the output by a fixed 100 ms, which always leaves enough input for every grain and the 21 ms look-ahead. That delay is reported to the host, so playback stays in time. **Live** starts at 8 ms and adds delay only when a grain's source isn't available yet, then slowly gives it back. In practice it settles at 8–20 ms (about two to three periods of the voice), and the header shows the current figure. The host can't compensate for this varying delay, so use Studio for the final pass.

## Checks

`SnaplineRender` renders a synthetic vocal (band-limited glottal source through three formant resonators, sung up to 42 cents off the scale, with vibrato and a slide) at 44.1, 48 and 96 kHz. It verifies:

- hard-tune accuracy in both modes (about 1 cent mean error)
- that natural settings at least halve the error
- level, clipping and NaNs
- the Studio null and its reported latency, and the Live delay range
- vibrato kept or flattened by *Natural vibrato*, scoops kept and settled, no clicks or spectral jumps at voiced/unvoiced edges, and period jitter on a breathy low voice
- transpose, MIDI forcing, formant shift, odd block sizes, silence and CPU cost

## Limits

- Monophonic sources only. Chords, doubles and heavy reverb confuse the tracker.
- Stereo inputs are tuned as a mono sum.
- Very breathy or growled notes may be treated as unvoiced and passed through unchanged.
- There is no graphical note editing; Snapline works in real time.
