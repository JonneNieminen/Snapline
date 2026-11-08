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
    double glideTime = 0.35; // seconds the slide takes
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
std::vector<float> makeVocal (double fs, const std::vector<Note>& notes, double seconds, double formantScale = 1.0, double noise = 0.02)
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
            midi = active->glideFrom + (active->midi - active->glideFrom) * std::min (1.0, age / active->glideTime);
        midi += active->vibratoCents / 100.0 * std::sin (2.0 * pi * 5.6 * age) * std::min (1.0, age / 0.2);
        const double f0 = 440.0 * std::pow (2.0, (midi - 69.0) / 12.0);
        phase += f0 / fs;
        phase -= std::floor (phase);
        double s = 0.0;
        const int harmonics = (int) std::min (60.0, (std::min (fs * 0.45, 9000.0)) / f0);
        for (int k = 1; k <= harmonics; ++k)
            s += std::sin (2.0 * pi * k * phase) / (double) k;
        const double env = std::min ({ 1.0, age / 0.03, (active->length - age) / 0.04 });
        source[i] = (float) (env * (0.5 * s + noise * (rng.nextDouble() * 2.0 - 1.0)));
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


/** Peak vibrato depth (cents) in [from, to]: linear trend removed, sqrt(2) * standard deviation. */
double vibratoDepth (const PitchTrack& t, double from, double to)
{
    std::vector<double> x, y;
    for (size_t i = 0; i < t.time.size(); ++i)
        if (t.time[i] >= from && t.time[i] <= to && t.cents[i] > 0)
        {
            x.push_back (t.time[i]);
            y.push_back (t.cents[i]);
        }
    if (x.size() < 8) return 0.0;
    double mx = 0, my = 0;
    for (size_t i = 0; i < x.size(); ++i) { mx += x[i]; my += y[i]; }
    mx /= (double) x.size();
    my /= (double) x.size();
    double sxx = 0, sxy = 0;
    for (size_t i = 0; i < x.size(); ++i) { sxx += (x[i] - mx) * (x[i] - mx); sxy += (x[i] - mx) * (y[i] - my); }
    const double slope = sxx > 0 ? sxy / sxx : 0.0;
    double var = 0;
    for (size_t i = 0; i < x.size(); ++i)
    {
        const double r = y[i] - my - slope * (x[i] - mx);
        var += r * r;
    }
    return std::sqrt (2.0 * var / (double) x.size());
}

/** Mean tracked pitch (cents) in [from, to], or a negative number if there was none. */
double meanCents (const PitchTrack& t, double from, double to)
{
    double sum = 0;
    int n = 0;
    for (size_t i = 0; i < t.time.size(); ++i)
        if (t.time[i] >= from && t.time[i] <= to && t.cents[i] > 0) { sum += t.cents[i]; ++n; }
    return n > 0 ? sum / n : -1.0;
}

/**
    Cycle-to-cycle period jitter in percent: the mean absolute change between consecutive
    periods over the mean period. Periods are measured by matching each cycle against the
    next one (normalised cross-correlation, parabolic peak), independently of the engine.
*/
double periodJitter (const std::vector<float>& x, double fs, double from, double to, double f0)
{
    double period = fs / f0;
    double pos = from * fs;
    std::vector<double> taus;
    while (pos + 2.4 * period < to * fs && pos + 2.4 * period < (double) x.size())
    {
        const int len = (int) period;
        const int lo = (int) std::floor (0.8 * period), hi = (int) std::ceil (1.2 * period);
        std::vector<double> score ((size_t) hi + 2, -2.0);
        const auto base = (std::int64_t) pos;
        double e0 = 0;
        for (int i = 0; i < len; ++i) e0 += (double) x[(size_t) (base + i)] * x[(size_t) (base + i)];
        for (int lag = lo; lag <= hi; ++lag)
        {
            double c = 0, e1 = 0;
            for (int i = 0; i < len; ++i)
            {
                const double a = x[(size_t) (base + i)], b = x[(size_t) (base + lag + i)];
                c += a * b;
                e1 += b * b;
            }
            score[(size_t) lag] = c / std::sqrt (std::max (e0 * e1, 1.0e-20));
        }
        int best = lo;
        for (int lag = lo; lag <= hi; ++lag)
            if (score[(size_t) lag] > score[(size_t) best]) best = lag;
        double refined = best;
        if (best > lo && best < hi)
        {
            const double a = score[(size_t) best - 1], b = score[(size_t) best], c = score[(size_t) best + 1];
            const double d = a - 2.0 * b + c;
            if (std::abs (d) > 1.0e-12) refined += std::max (-0.5, std::min (0.5, 0.5 * (a - c) / d));
        }
        taus.push_back (refined);
        pos += refined;
        period = 0.6 * period + 0.4 * refined;
    }
    if (taus.size() < 6) return 0.0;
    double diff = 0, mean = 0;
    for (size_t i = 0; i < taus.size(); ++i)
    {
        mean += taus[i];
        if (i > 0) diff += std::abs (taus[i] - taus[i - 1]);
    }
    return 100.0 * (diff / (double) (taus.size() - 1)) / (mean / (double) taus.size());
}

/** Highpassed noise burst with short raised-cosine edges: a stand-in for a fricative. */
void addNoiseBurst (std::vector<float>& x, double fs, double start, double length, float level)
{
    juce::Random rng (7);
    float previous = 0.0f;
    const auto first = (size_t) (start * fs), count = (size_t) (length * fs);
    const double edge = 0.004 * fs;
    for (size_t i = 0; i < count && first + i < x.size(); ++i)
    {
        const float white = rng.nextFloat() * 2.0f - 1.0f;
        const float high = white - previous; // first difference: tilts towards high frequencies
        previous = white;
        const double env = std::min ({ 1.0, (double) i / edge, (double) (count - i) / edge });
        x[first + i] += (float) (0.5 * level * env * high);
    }
}

/** Largest second difference relative to the local RMS of the second difference (click detector). */
double clickIndex (const std::vector<float>& x, double fs, size_t from, size_t to)
{
    const size_t win = (size_t) (0.02 * fs);
    double worst = 0;
    to = std::min (to, x.size());
    for (size_t i = from + win; i + win < to; ++i)
    {
        const double d2 = std::abs ((double) x[i] - 2.0 * x[i - 1] + x[i - 2]);
        if (d2 < 2.0e-3) continue; // ignore silence
        if ((i & 15) == 0 || d2 > 0.0)
        {
            double e = 0;
            for (size_t k = i - win; k < i + win; k += 8)
            {
                const double v = (double) x[k] - 2.0 * x[k - 1] + x[k - 2];
                e += v * v;
            }
            const double rms = std::sqrt (e / (double) (2 * win / 8));
            worst = std::max (worst, d2 / std::max (rms, 1.0e-6));
        }
    }
    return worst;
}

/**
    Largest frame-to-frame spectral flux (rising magnitude / total magnitude), ignoring
    onsets out of near-silence, which are always 1.
*/
double maxSpectralFlux (const std::vector<float>& x, double fs)
{
    int order = 8;
    while ((1 << order) < (int) (fs * 0.0107)) ++order;
    const int size = 1 << order, hop = size / 4;
    juce::dsp::FFT fft (order);
    std::vector<float> buf ((size_t) size * 2), previous ((size_t) size / 2, 0.0f);
    std::vector<double> rises, totals;
    for (size_t pos = 0; pos + (size_t) size < x.size(); pos += (size_t) hop)
    {
        std::fill (buf.begin(), buf.end(), 0.0f);
        for (int i = 0; i < size; ++i)
            buf[(size_t) i] = x[pos + (size_t) i] * (float) (0.5 - 0.5 * std::cos (2.0 * pi * i / size));
        fft.performFrequencyOnlyForwardTransform (buf.data());
        double rise = 0, total = 0;
        for (int k = 1; k < size / 2; ++k)
        {
            rise += std::max (0.0f, buf[(size_t) k] - previous[(size_t) k]);
            total += buf[(size_t) k];
            previous[(size_t) k] = buf[(size_t) k];
        }
        rises.push_back (rise);
        totals.push_back (total);
    }
    double peak = 0;
    for (double t : totals) peak = std::max (peak, t);
    double worst = 0;
    for (size_t i = 1; i < totals.size(); ++i)
        if (totals[i - 1] > 0.05 * peak && totals[i] > 0.05 * peak)
            worst = std::max (worst, rises[i] / totals[i]);
    return worst;
}

int failures = 0;

void check (bool ok, const char* what, const juce::String& detail)
{
    std::printf ("  [%s] %-44s %s\n", ok ? " ok " : "FAIL", what, detail.toRawUTF8());
    if (! ok) ++failures;
}

/** Checks of the "human by default" behaviour: vibrato, scoops, clicks, period jitter. */
void humanChecks (double fs, Mode mode, const NoteMask cMajor)
{
    const bool studio = mode == Mode::studio;
    const juce::String m = studio ? " (studio)" : " (live)";
    SnaplineDsp probe;
    probe.prepare (fs, 256, mode);
    const double delay = probe.getLatencySamples (mode) / fs + (studio ? 0.0 : 0.03);
    const auto verbose = std::getenv ("SNAPLINE_VERBOSE") != nullptr;

    // Vibrato: a sustained note with 40 ct vibrato, corrected quickly.
    {
        const std::vector<Note> sustained { { 0.2, 2.0, 62.15, 40.0 } };
        const auto vocal = makeVocal (fs, sustained, 2.5);
        const double from = 0.8, to = 2.05;
        const double inDepth = vibratoDepth (track (vocal, fs), from, to);
        SnaplineParams p;
        p.mode = mode;
        p.allowed = cMajor;
        p.retuneMs = 5.0f;    // fast: the old engine pulled vibrato out at these speeds
        p.humanize = 0.0f;
        const auto keep = vibratoDepth (track (run (vocal, fs, 256, p).left, fs), from + delay, to + delay);
        check (keep >= 0.7 * inDepth, ("natural vibrato keeps >= 70 % of the depth" + m).toRawUTF8(),
               juce::String::formatted ("%.1f of %.1f ct (%.0f %%)", keep, inDepth, 100.0 * keep / std::max (inDepth, 1.0e-6)));
        p.naturalVibrato = -100.0f;
        const auto flat = vibratoDepth (track (run (vocal, fs, 256, p).left, fs), from + delay, to + delay);
        check (flat <= 0.3 * inDepth, ("natural vibrato -100 % removes it" + m).toRawUTF8(),
               juce::String::formatted ("%.1f ct (%.0f %%)", flat, 100.0 * flat / std::max (inDepth, 1.0e-6)));
        p.naturalVibrato = 100.0f;
        const auto more = vibratoDepth (track (run (vocal, fs, 256, p).left, fs), from + delay, to + delay);
        check (more >= 1.5 * inDepth, ("natural vibrato +100 % exaggerates it" + m).toRawUTF8(),
               juce::String::formatted ("%.1f ct (%.0f %%)", more, 100.0 * more / std::max (inDepth, 1.0e-6)));
    }

    // Scoop: a note attacked 100 ct flat that rises in 120 ms, with the Natural preset's speeds.
    {
        const std::vector<Note> scoop { { 0.3, 1.2, 62.0, 0.0, 61.0, 0.12 } };
        const auto vocal = makeVocal (fs, scoop, 1.9);
        const auto inTrack = track (vocal, fs);
        SnaplineParams p;
        p.mode = mode;
        p.allowed = cMajor;
        p.retuneMs = 85.0f;
        p.humanize = 45.0f;
        double d = delay;
        if (! studio) // the live delay adapts to the voice: measure it on the same signal
        {
            auto none = p;
            none.amount = 0.0f;
            d = bestLag (vocal, run (vocal, fs, 256, none).left, (int) (0.05 * fs)) / fs;
        }
        const auto outTrack = track (run (vocal, fs, 256, p).left, fs);
        const double inDip = 6200.0 - meanCents (inTrack, 0.3 + 0.03, 0.3 + 0.05);
        const double outDip = 6200.0 - meanCents (outTrack, 0.3 + 0.03 + d, 0.3 + 0.05 + d);
        const double settled = std::abs (meanCents (outTrack, 0.3 + 0.75 + d, 0.3 + 1.05 + d) - 6200.0);
        if (verbose)
            for (double rel : { 0.02, 0.04, 0.06, 0.08, 0.10, 0.14, 0.2, 0.3 })
                std::printf ("      scoop +%.0f ms: in %+.0f ct, out %+.0f ct\n", rel * 1000, meanCents (inTrack, 0.3 + rel - 0.005, 0.3 + rel + 0.005) - 6200.0,
                             meanCents (outTrack, 0.3 + rel - 0.005 + d, 0.3 + rel + 0.005 + d) - 6200.0);
        check (outDip >= 0.6 * inDip, ("scoop shape preserved" + m).toRawUTF8(),
               juce::String::formatted ("dip %.0f ct kept of %.0f ct (%.0f %%)", outDip, inDip, 100.0 * outDip / std::max (inDip, 1.0e-6)));
        check (settled < 6.0, ("scoop settles on the note" + m).toRawUTF8(), juce::String::formatted ("%.1f ct off after 0.75 s", settled));
    }

    // Consonants: noise bursts abutting two notes must not click or smear the spectrum.
    {
        const std::vector<Note> two { { 0.5, 0.7, 57.35, 0.0 }, { 1.5, 0.7, 59.72, 0.0 } };
        auto vocal = makeVocal (fs, two, 2.6);
        addNoiseBurst (vocal, fs, 0.40, 0.10, 0.30f);
        addNoiseBurst (vocal, fs, 1.20, 0.09, 0.30f);
        addNoiseBurst (vocal, fs, 1.40, 0.10, 0.30f);
        addNoiseBurst (vocal, fs, 2.20, 0.09, 0.30f);
        SnaplineParams p;
        p.mode = mode;
        p.allowed = cMajor;
        p.retuneMs = 20.0f;
        p.humanize = 20.0f;
        const auto out = run (vocal, fs, 256, p).left;
        const double inClick = clickIndex (vocal, fs, 0, vocal.size());
        const double outClick = clickIndex (out, fs, 0, out.size());
        check (outClick <= 1.5 * inClick + 1.0, ("no clicks at voiced / unvoiced edges" + m).toRawUTF8(),
               juce::String::formatted ("click index %.1f (input %.1f)", outClick, inClick));
        const double inFlux = maxSpectralFlux (vocal, fs), outFlux = maxSpectralFlux (out, fs);
        check (outFlux <= inFlux * 1.15 + 0.02, ("spectral flux bounded at transitions" + m).toRawUTF8(),
               juce::String::formatted ("max flux %.3f (input %.3f)", outFlux, inFlux));
    }

    // Pitch-mark robustness: a low, breathy voice pulled 35 ct flat must not turn rough.
    {
        const std::vector<Note> low { { 0.2, 1.6, 45.35, 0.0 } };
        const auto vocal = makeVocal (fs, low, 2.0, 1.0, 0.6);
        const double f0In = 440.0 * std::pow (2.0, (45.35 - 69.0) / 12.0);
        const double f0Out = 110.0;
        SnaplineParams p;
        p.mode = mode;
        p.inputType = InputType::lowMale;
        p.allowed = cMajor;
        p.retuneMs = 0.0f;
        p.naturalVibrato = -100.0f;
        const auto out = run (vocal, fs, 256, p).left;
        const double inJitter = periodJitter (vocal, fs, 0.6, 1.7, f0In);
        const double outJitter = periodJitter (out, fs, 0.6 + delay, 1.7 + delay, f0Out);
        check (outJitter <= inJitter + 1.0, ("period jitter <= input + 1 %" + m).toRawUTF8(),
               juce::String::formatted ("%.2f %% (input %.2f %%)", outJitter, inJitter));
        if (verbose) std::printf ("      jitter in %.3f %% out %.3f %%\n", inJitter, outJitter);
    }
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
            hard.naturalVibrato = -100.0f; // hard tune flattens the singer's vibrato too
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
                   juce::String::formatted ("%.1fx real time (%.2f %% of one core)", (melodyEnd + 0.4) / r.seconds, 100.0 * r.seconds / (melodyEnd + 0.4)));
            if (fs == 48000.0) writeWav (outDir, juce::String ("hardtune-") + name + "-" + tag, r.left, fs);

            // Natural settings keep the singer's vibrato but still pull notes in.
            SnaplineParams natural = hard;
            natural.retuneMs = 85.0f;
            natural.humanize = 45.0f;
            natural.naturalVibrato = 0.0f;
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
            p.naturalVibrato = -100.0f; // this check is about block sizes, not about vibrato
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
            p.naturalVibrato = -100.0f;
            const auto r = run (vocal, fs, 256, p);
            const auto a = accuracy (track (r.left, fs), 0.1, melody, [&] (const Note& n) { return nearestInScale (n.midi * 100.0, cMajor) * 100.0 + 1200.0; }, 0.2);
            check (a.meanAbs < 8.0, "transpose +12 st", juce::String::formatted ("mean %.1f ct", a.meanAbs));
        }

        // Force to a MIDI note.
        {
            SnaplineParams p;
            p.forceNote = 60;
            p.naturalVibrato = -100.0f;
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
            base.naturalVibrato = -100.0f;
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

        // Mix at 0 % skips the resynthesis and must give back the dry signal, delayed.
        {
            SnaplineParams dry;
            dry.mode = Mode::studio;
            dry.allowed = cMajor;
            dry.mix = 0.0f;
            const auto r = run (vocal, fs, 256, dry);
            SnaplineDsp lagProbe;
            lagProbe.prepare (fs, 256, Mode::studio);
            const double depth = nullDepth (vocal, r.left, lagProbe.getLatencySamples (Mode::studio));
            check (depth > 100.0, "mix 0 % is the delayed dry signal", juce::String::formatted ("null %.0f dB, %.2f %% of one core", depth,
                   100.0 * r.seconds / (melodyEnd + 0.4)));
        }

        // Soft bypass: engaged for the second half it must give the delayed dry signal without clicks.
        {
            SnaplineParams p;
            p.mode = Mode::studio;
            p.allowed = cMajor;
            p.retuneMs = 0.0f;
            p.naturalVibrato = -100.0f;
            SnaplineDsp dsp;
            dsp.prepare (fs, 256, Mode::studio);
            auto out = vocal;
            const size_t switchAt = (size_t) (3.0 * fs);
            for (size_t pos = 0; pos < vocal.size(); pos += 256)
            {
                const int n = (int) std::min<size_t> (256, vocal.size() - pos);
                p.bypass = pos >= switchAt;
                dsp.process (out.data() + pos, nullptr, out.data() + pos, nullptr, n, p);
                DisplayFrame frames[256];
                while (dsp.popDisplayFrames (frames, 256) > 0) {}
            }
            const int lag = dsp.getLatencySamples (Mode::studio);
            // After the 12 ms fade (and a margin) the output is the dry signal at the reported latency.
            const size_t settled = switchAt + (size_t) (0.1 * fs);
            std::vector<float> a (vocal.begin() + (std::ptrdiff_t) (settled - (size_t) lag), vocal.end() - lag), b (out.begin() + (std::ptrdiff_t) settled, out.end());
            const size_t len = std::min (a.size(), b.size());
            a.resize (len);
            b.resize (len);
            const double depth = nullDepth (a, b, 0);
            // The jump is judged against the processed signal it fades from.
            const double click = clickIndex (out, fs, switchAt - (size_t) (0.05 * fs), switchAt + (size_t) (0.1 * fs));
            const double ref = clickIndex (vocal, fs, switchAt - (size_t) (0.05 * fs), switchAt + (size_t) (0.1 * fs));
            check (depth > 90.0 && click <= 1.5 * ref + 1.0, "soft bypass: dry at the reported latency, no click",
                   juce::String::formatted ("null %.0f dB, click index %.1f (input %.1f)", depth, click, ref));
        }

        // Worst case for the CPU: a voice that never pauses (nothing to skip), best of three runs.
        {
            const std::vector<Note> sung { { 0.0, 1.6, 57.35, 30.0 }, { 1.6, 1.6, 59.72, 30.0 }, { 3.2, 1.6, 62.15, 30.0 }, { 4.8, 1.6, 60.4, 30.0 } };
            const auto continuous = makeVocal (fs, sung, 6.4);
            SnaplineParams busy;
            busy.allowed = cMajor;
            busy.retuneMs = 40.0f;
            busy.humanize = 30.0f;
            double best = 1.0e9;
            for (int pass = 0; pass < 3; ++pass)
                best = std::min (best, run (continuous, fs, 256, busy).seconds);
            check (best < 6.4 * 0.1, "CPU, continuous voice (worst case)",
                   juce::String::formatted ("%.1fx real time (%.2f %% of one core)", 6.4 / best, 100.0 * best / 6.4));
        }

        // Human-by-default behaviour (vibrato, scoops, consonant edges, roughness).
        humanChecks (fs, Mode::studio, cMajor);
        humanChecks (fs, Mode::live, cMajor);
    }

    std::printf ("\n%s\n", failures == 0 ? "All checks passed." : juce::String (juce::String (failures) + " check(s) failed.").toRawUTF8());
    return failures == 0 ? 0 : 1;
}
