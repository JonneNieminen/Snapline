// snapline-render: offline checks of the Snapline engine.
// Usage: snapline-render [wav-output-directory]
//
// Renders a synthetic, deliberately off-key vocal through the engine and checks
// corrected pitch accuracy, transparency at zero correction, reported latency,
// formant and transpose behaviour, MIDI forcing, block-size robustness, silence,
// NaNs and CPU cost, at 44.1, 48 and 96 kHz.
#include "SnaplineDsp.h"
#include "PitchDetector.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <algorithm>
#include <chrono>
#include <functional>
#include <cstdlib>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace snapline;

namespace
{
constexpr double pi = 3.14159265358979323846;

struct Note
{
    double start, length;   // seconds
    double midi;            // sung pitch, may be off-key
    double vibratoCents;    // natural vibrato depth
    double glideFrom = 0.0; // if non-zero: MIDI pitch the note slides in from
};

// An off-key melody in C major around the alto / tenor range.
const std::vector<Note> melody {
    { 0.20, 0.70, 57.35, 0.0 },        // A3 +35 ct
    { 1.00, 0.70, 59.72, 0.0 },        // B3 -28 ct
    { 1.80, 0.90, 62.15, 35.0 },       // D4 +15 ct with vibrato
    { 2.80, 0.70, 63.58, 0.0 },        // E4 -42 ct
    { 3.60, 0.70, 55.22, 0.0 },        // G3 +22 ct
    { 4.40, 0.90, 60.00, 0.0, 57.0 },  // slide A3 -> C4
    { 5.40, 0.70, 65.30, 0.0 },        // F4 +30 ct
};
constexpr double melodyEnd = 6.3;

/** Band-limited glottal-ish source through three formant resonators ("ah"). */
std::vector<float> makeVocal (double fs, const std::vector<Note>& notes, double seconds, double formantScale = 1.0)
{
    const auto total = (size_t) (seconds * fs);
    std::vector<float> source (total, 0.0f), out (total, 0.0f);
    juce::Random rng (42);
    double phase = 0.0;
    for (size_t i = 0; i < total; ++i)
    {
        const double t = (double) i / fs;
        const Note* active = nullptr;
        for (const auto& n : notes)
            if (t >= n.start && t < n.start + n.length) active = &n;
        if (active == nullptr)
            continue;
        const double age = t - active->start;
        double midi = active->midi;
        if (active->glideFrom != 0.0)
            midi = active->glideFrom + (active->midi - active->glideFrom) * std::min (1.0, age / 0.35);
        midi += active->vibratoCents / 100.0 * std::sin (2.0 * pi * 5.6 * age) * std::min (1.0, age / 0.2);
        const double f0 = 440.0 * std::pow (2.0, (midi - 69.0) / 12.0);
        phase += f0 / fs;
        phase -= std::floor (phase);
        double s = 0.0;
        const int harmonics = (int) std::min (60.0, (std::min (fs * 0.45, 9000.0)) / f0);
        for (int k = 1; k <= harmonics; ++k)
            s += std::sin (2.0 * pi * k * phase) / (double) k;
        const double env = std::min ({ 1.0, age / 0.03, (active->length - age) / 0.04 });
        source[i] = (float) (env * (0.5 * s + 0.02 * (rng.nextDouble() * 2.0 - 1.0)));
    }

    struct Resonator { double f, bw, gain; };
    const Resonator formants[] { { 700.0, 110.0, 1.0 }, { 1220.0, 120.0, 0.6 }, { 2600.0, 160.0, 0.3 } };
    for (const auto& r : formants)
    {
        const double rad = std::exp (-pi * r.bw / fs);
        const double theta = 2.0 * pi * r.f * formantScale / fs;
        const double a1 = -2.0 * rad * std::cos (theta), a2 = rad * rad;
        double y1 = 0, y2 = 0;
        for (size_t i = 0; i < total; ++i)
        {
            const double y = (1.0 - rad) * source[i] - a1 * y1 - a2 * y2;
            y2 = y1;
            y1 = y;
            out[i] += (float) (y * r.gain);
        }
    }
    float peak = 0.0f;
    for (auto v : out) peak = std::max (peak, std::abs (v));
    for (auto& v : out) v *= 0.4f / std::max (peak, 1.0e-6f); // about -8 dBFS peak
    return out;
}

struct Render
{
    std::vector<float> left, right;
    double seconds = 0.0; // processing time
    float minDelay = 1e9f, maxDelay = 0.0f; // effective delay after the first second (samples)
};

Render run (const std::vector<float>& in, double fs, int block, const SnaplineParams& p, bool stereo = true)
{
    SnaplineDsp dsp;
    dsp.prepare (fs, block, p.mode);
    Render r;
    r.left = in;
    r.right = in;
    const auto start = std::chrono::steady_clock::now();
    for (size_t pos = 0; pos < in.size(); pos += (size_t) block)
    {
        const int n = (int) std::min ((size_t) block, in.size() - pos);
        dsp.process (r.left.data() + pos, stereo ? r.right.data() + pos : nullptr,
                     r.left.data() + pos, stereo ? r.right.data() + pos : nullptr, n, p);
        DisplayFrame frames[256];
        while (dsp.popDisplayFrames (frames, 256) > 0) {}
        if ((double) pos > fs)
        {
            r.minDelay = std::min (r.minDelay, dsp.getEffectiveDelaySamples());
            r.maxDelay = std::max (r.maxDelay, dsp.getEffectiveDelaySamples());
        }
    }
    r.seconds = std::chrono::duration<double> (std::chrono::steady_clock::now() - start).count();
    return r;
}

struct PitchTrack
{
    std::vector<double> time, cents; // cents < 0 = unvoiced
};

PitchTrack track (const std::vector<float>& x, double fs)
{
    PitchDetector det;
    const int window = (int) std::ceil (fs / 70.0);
    det.prepare (fs, window);
    PitchTrack t;
    const int hop = (int) (fs / 200.0);
    for (size_t end = (size_t) (2 * window); end < x.size(); end += (size_t) hop)
    {
        const auto r = det.detect (x.data() + end - 2 * (size_t) window, window, (int) (fs / 900.0), 0.12f);
        t.time.push_back (((double) end - window) / fs);
        const bool voiced = r.hz > 0.0f && r.aperiodicity < 0.2f && r.rms > 0.005f;
        t.cents.push_back (voiced ? 6900.0 + 1200.0 * std::log2 (r.hz / 440.0) : -1.0);
    }
    return t;
}

int nearestInScale (double cents, NoteMask mask)
{
    int best = 0;
    double bestD = 1e9;
    for (int n = 0; n < 128; ++n)
        if (mask & (1u << (n % 12)))
            if (std::abs (cents - n * 100.0) < bestD) { bestD = std::abs (cents - n * 100.0); best = n; }
    return best;
}

/** Pitch error against an expected note for each note's steady section. */
struct Accuracy { double meanAbs = 0, maxAbs = 0, spread = 0; int frames = 0; };

Accuracy accuracy (const PitchTrack& t, double delaySeconds, const std::vector<Note>& notes,
                   const std::function<double (const Note&)>& expectedCents, double settle = 0.15)
{
    Accuracy a;
    double sum = 0;
    for (const auto& n : notes)
    {
        const double from = n.start + delaySeconds + settle + (n.glideFrom != 0.0 ? 0.35 : 0.0);
        const double to = n.start + n.length + delaySeconds - 0.08;
        const double expected = expectedCents (n);
        std::vector<double> errs;
        for (size_t i = 0; i < t.time.size(); ++i)
            if (t.time[i] >= from && t.time[i] <= to && t.cents[i] > 0)
                errs.push_back (t.cents[i] - expected);
        if (errs.empty()) { a.maxAbs = 9999; continue; }
        double mean = 0;
        for (double e : errs) mean += e;
        mean /= (double) errs.size();
        double var = 0;
        for (double e : errs) { var += (e - mean) * (e - mean); sum += std::abs (e); a.maxAbs = std::max (a.maxAbs, std::abs (e)); }
        a.spread = std::max (a.spread, std::sqrt (var / (double) errs.size()));
        if (std::getenv ("SNAPLINE_VERBOSE") != nullptr)
            std::printf ("      note %.2f: mean %+.1f ct, sd %.1f ct\n", n.midi, mean, std::sqrt (var / (double) errs.size()));
        a.frames += (int) errs.size();
    }
    a.meanAbs = a.frames > 0 ? sum / a.frames : 9999;
    return a;
}

double rmsOf (const std::vector<float>& x, size_t from = 0, size_t to = 0)
{
    if (to == 0 || to > x.size()) to = x.size();
    double s = 0;
    for (size_t i = from; i < to; ++i) s += (double) x[i] * x[i];
    return std::sqrt (s / (double) std::max<size_t> (1, to - from));
}

double toDb (double v) { return 20.0 * std::log10 (std::max (v, 1.0e-12)); }

/** Lag (samples) that best aligns y with x, searched in [0, maxLag]. */
int bestLag (const std::vector<float>& x, const std::vector<float>& y, int maxLag)
{
    int best = 0;
    double bestC = -1e30;
    for (int lag = 0; lag <= maxLag; ++lag)
    {
        double c = 0;
        for (size_t i = 0; i + (size_t) lag < y.size() && i < x.size(); i += 3)
            c += (double) x[i] * y[i + (size_t) lag];
        if (c > bestC) { bestC = c; best = lag; }
    }
    return best;
}

/** Residual after aligning y to x by `lag`, in dB below the signal. */
double nullDepth (const std::vector<float>& x, const std::vector<float>& y, int lag)
{
    double sig = 0, err = 0;
    for (size_t i = 0; i + (size_t) lag < y.size(); ++i)
    {
        const double d = (double) y[i + (size_t) lag] - x[i];
        sig += (double) x[i] * x[i];
        err += d * d;
    }
    return 10.0 * std::log10 (std::max (sig, 1e-20) / std::max (err, 1e-20));
}

double spectralCentroid (const std::vector<float>& x, double fs, double fromSec, double toSec)
{
    constexpr int order = 12, size = 1 << order;
    juce::dsp::FFT fft (order);
    std::vector<float> buf ((size_t) size * 2);
    double num = 0, den = 0;
    for (auto pos = (size_t) (fromSec * fs); pos + size < (size_t) (toSec * fs) && pos + size < x.size(); pos += size / 2)
    {
        std::fill (buf.begin(), buf.end(), 0.0f);
        for (int i = 0; i < size; ++i)
            buf[(size_t) i] = x[pos + (size_t) i] * (float) (0.5 - 0.5 * std::cos (2.0 * pi * i / size));
        fft.performFrequencyOnlyForwardTransform (buf.data());
        for (int k = 1; k < size / 2; ++k)
        {
            const double f = k * fs / size;
            if (f > 5000.0) break;
            num += f * buf[(size_t) k];
            den += buf[(size_t) k];
        }
    }
    return den > 0 ? num / den : 0;
}

bool finite (const Render& r)
{
    for (size_t i = 0; i < r.left.size(); ++i)
        if (! std::isfinite (r.left[i]) || ! std::isfinite (r.right[i])) return false;
    return true;
}

double peakOf (const Render& r)
{
    double p = 0;
    for (size_t i = 0; i < r.left.size(); ++i) p = std::max ({ p, (double) std::abs (r.left[i]), (double) std::abs (r.right[i]) });
    return p;
}

void writeWav (const juce::File& dir, const juce::String& name, const std::vector<float>& x, double fs)
{
    if (dir == juce::File()) return;
    dir.createDirectory();
    juce::WavAudioFormat wav;
    auto file = dir.getChildFile (name + ".wav");
    file.deleteFile();
    std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());
    if (stream == nullptr) return;
    const auto options = juce::AudioFormatWriterOptions().withSampleRate (fs).withNumChannels (1).withBitsPerSample (24);
    auto writer = wav.createWriterFor (stream, options);
    if (writer == nullptr) return;
    const float* chans[] { x.data() };
    writer->writeFromFloatArrays (chans, 1, (int) x.size());
}

int failures = 0;

void check (bool ok, const char* what, const juce::String& detail)
{
    std::printf ("  [%s] %-44s %s\n", ok ? " ok " : "FAIL", what, detail.toRawUTF8());
    if (! ok) ++failures;
}
}

int main (int argc, char** argv)
{
    const juce::File outDir = argc > 1 ? juce::File::getCurrentWorkingDirectory().getChildFile (argv[1]) : juce::File();
    const NoteMask cMajor = maskForKey (scales[1].intervals, 0);

    for (double fs : { 44100.0, 48000.0, 96000.0 })
    {
        std::printf ("\n== %.1f kHz ==\n", fs / 1000.0);
        const auto vocal = makeVocal (fs, melody, melodyEnd + 0.4);
        const auto inputRms = rmsOf (vocal);
        const juce::String tag = juce::String ((int) (fs / 100.0) / 10.0, 1) + "k";
        if (fs == 48000.0) writeWav (outDir, "input-" + tag, vocal, fs);

        const auto inTrack = track (vocal, fs);
        const auto inAcc = accuracy (inTrack, 0.0, melody, [&] (const Note& n) { return nearestInScale (n.midi * 100.0, cMajor) * 100.0; });
        std::printf ("  input: mean %.1f ct off the scale\n", inAcc.meanAbs);

        for (Mode mode : { Mode::studio, Mode::live })
        {
            const bool studio = mode == Mode::studio;
            const char* name = studio ? "studio" : "live";
            SnaplineParams hard;
            hard.mode = mode;
            hard.allowed = cMajor;
            hard.retuneMs = 0.0f;
            hard.humanize = 0.0f;
            const auto r = run (vocal, fs, 256, hard);
            const double latency = (double) SnaplineDsp().getLatencySamples (mode);
            SnaplineDsp probe;
            probe.prepare (fs, 256, mode);
            const double delay = probe.getLatencySamples (mode) / fs + (studio ? 0.0 : 0.03);
            juce::ignoreUnused (latency);
            const auto t = track (r.left, fs);
            const auto acc = accuracy (t, delay, melody, [&] (const Note& n) { return nearestInScale (n.midi * 100.0, cMajor) * 100.0; });
            check (acc.meanAbs < 4.0 && acc.maxAbs < 15.0, juce::String (juce::String ("hard tune accuracy (") + name + ")").toRawUTF8(),
                   juce::String::formatted ("mean %.2f ct, max %.1f ct, spread %.2f ct, %d frames", acc.meanAbs, acc.maxAbs, acc.spread, acc.frames));
            check (finite (r) && peakOf (r) < 0.99, juce::String (juce::String ("finite, no clipping (") + name + ")").toRawUTF8(),
                   juce::String::formatted ("peak %.1f dBFS", toDb (peakOf (r))));
            const double levelDiff = toDb (rmsOf (r.left)) - toDb (inputRms);
            check (std::abs (levelDiff) < 1.5, juce::String (juce::String ("level matches input (") + name + ")").toRawUTF8(),
                   juce::String::formatted ("%+.2f dB", levelDiff));
            check (r.seconds < melodyEnd * 0.1, juce::String (juce::String ("CPU cost (") + name + ")").toRawUTF8(),
                   juce::String::formatted ("%.1fx real time", (melodyEnd + 0.4) / r.seconds));
            if (fs == 48000.0) writeWav (outDir, juce::String ("hardtune-") + name + "-" + tag, r.left, fs);

            // Natural settings keep the singer's vibrato but still pull notes in.
            SnaplineParams natural = hard;
            natural.retuneMs = 85.0f;
            natural.humanize = 45.0f;
            const auto rn = run (vocal, fs, 256, natural);
            const auto an = accuracy (track (rn.left, fs), delay, melody, [&] (const Note& n) { return nearestInScale (n.midi * 100.0, cMajor) * 100.0; }, 0.3);
            check (an.meanAbs < inAcc.meanAbs * 0.5, juce::String (juce::String ("natural setting improves tuning (") + name + ")").toRawUTF8(),
                   juce::String::formatted ("mean %.1f ct (input %.1f ct)", an.meanAbs, inAcc.meanAbs));
            if (fs == 48000.0) writeWav (outDir, juce::String ("natural-") + name + "-" + tag, rn.left, fs);

            // Zero correction must be transparent (and time-aligned in studio).
            SnaplineParams none = hard;
            none.amount = 0.0f;
            const auto r0 = run (vocal, fs, 256, none);
            if (studio)
            {
                const int lag = probe.getLatencySamples (mode);
                const double depth = nullDepth (vocal, r0.left, lag);
                check (depth > 40.0, "transparent at 0 % amount (studio)",
                       juce::String::formatted ("null %.1f dB at %d samples (%.1f ms)", depth, lag, lag * 1000.0 / fs));
            }
            else
            {
                // Live delay adapts to the voice (and is not constant), so check that the
                // pitch passes through untouched and the delay stays in its expected range.
                const auto a0 = accuracy (track (r0.left, fs), r0.maxDelay / fs, melody, [] (const Note& n) { return n.midi * 100.0; }, 0.2);
                check (a0.meanAbs < 6.0, "pitch untouched at 0 % amount (live)",
                       juce::String::formatted ("mean %.2f ct from the sung pitch", a0.meanAbs));
                check (r0.minDelay >= (float) probe.getLatencySamples (mode) && r0.maxDelay < 0.03 * fs, "live delay stays under 30 ms",
                       juce::String::formatted ("%.1f-%.1f ms", r0.minDelay * 1000.0 / fs, r0.maxDelay * 1000.0 / fs));
            }
        }

        // Reported studio latency equals the true delay.
        {
            SnaplineParams none;
            none.mode = Mode::studio;
            none.amount = 0.0f;
            const auto r0 = run (vocal, fs, 512, none);
            SnaplineDsp probe;
            probe.prepare (fs, 512, Mode::studio);
            const int reported = probe.getLatencySamples (Mode::studio);
            const int measured = bestLag (vocal, r0.left, (int) (0.15 * fs));
            check (std::abs (measured - reported) <= 2, "studio latency matches report",
                   juce::String::formatted ("reported %d, measured %d samples", reported, measured));
        }

        // Block size independence (odd sizes, mono bus).
        {
            SnaplineParams p;
            p.allowed = cMajor;
            p.retuneMs = 10.0f;
            double worst = 0;
            for (int block : { 31, 64, 997, 2048 })
            {
                const auto r = run (vocal, fs, block, p, block != 31);
                const auto a = accuracy (track (r.left, fs), 0.1, melody, [&] (const Note& n) { return nearestInScale (n.midi * 100.0, cMajor) * 100.0; }, 0.2);
                worst = std::max (worst, a.meanAbs);
                if (! finite (r)) worst = 9999;
            }
            check (worst < 5.0, "block sizes 31 / 64 / 997 / 2048", juce::String::formatted ("worst mean error %.2f ct", worst));
        }

        // Transpose up an octave.
        {
            SnaplineParams p;
            p.allowed = cMajor;
            p.retuneMs = 0.0f;
            p.transpose = 12.0f;
            const auto r = run (vocal, fs, 256, p);
            const auto a = accuracy (track (r.left, fs), 0.1, melody, [&] (const Note& n) { return nearestInScale (n.midi * 100.0, cMajor) * 100.0 + 1200.0; }, 0.2);
            check (a.meanAbs < 8.0, "transpose +12 st", juce::String::formatted ("mean %.1f ct", a.meanAbs));
        }

        // Force to a MIDI note.
        {
            SnaplineParams p;
            p.forceNote = 60;
            p.allowed = 1u << 0;
            p.retuneMs = 0.0f;
            const auto r = run (vocal, fs, 256, p);
            const auto a = accuracy (track (r.left, fs), 0.1, melody, [] (const Note&) { return 6000.0; }, 0.2);
            check (a.meanAbs < 8.0, "MIDI force to C4", juce::String::formatted ("mean %.1f ct", a.meanAbs));
        }

        // Formant shift moves the spectral envelope, not the pitch.
        {
            SnaplineParams base;
            base.allowed = cMajor;
            base.retuneMs = 0.0f;
            auto up = base;
            up.formantShift = 4.0f;
            const auto r0 = run (vocal, fs, 256, base);
            const auto r1 = run (vocal, fs, 256, up);
            const double c0 = spectralCentroid (r0.left, fs, 0.4, melodyEnd);
            const double c1 = spectralCentroid (r1.left, fs, 0.4, melodyEnd);
            const auto a = accuracy (track (r1.left, fs), 0.1, melody, [&] (const Note& n) { return nearestInScale (n.midi * 100.0, cMajor) * 100.0; }, 0.2);
            check (c1 > c0 * 1.12 && a.meanAbs < 8.0, "formant +4 st keeps pitch",
                   juce::String::formatted ("centroid %.0f -> %.0f Hz, pitch error %.1f ct", c0, c1, a.meanAbs));
            if (fs == 48000.0) writeWav (outDir, "formant-up-" + tag, r1.left, fs);
        }

        // Silence in, silence out.
        {
            std::vector<float> silence ((size_t) fs, 0.0f);
            const auto r = run (silence, fs, 128, SnaplineParams {});
            check (finite (r) && peakOf (r) < 1.0e-6, "silence stays silent", juce::String::formatted ("peak %.1f dBFS", toDb (peakOf (r))));
        }
    }

    std::printf ("\n%s\n", failures == 0 ? "All checks passed." : juce::String (juce::String (failures) + " check(s) failed.").toRawUTF8());
    return failures == 0 ? 0 : 1;
}
