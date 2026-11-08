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
#include <string>
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


// ---------------------------------------------------------------------------- real recordings
struct Bands { double edges[4] { 0.0, 250.0, 2000.0, 1.0e9 }; };

/** Per-band damage between an aligned reference and a processed signal. */
struct BandReport { double nullDb[3] {}; double lsdDb[3] {}; };

BandReport compareBands (const std::vector<float>& ref, const std::vector<float>& out, double fs)
{
    constexpr int order = 12, size = 1 << order;
    juce::dsp::FFT fft (order);
    std::vector<float> a ((size_t) size * 2), b ((size_t) size * 2);
    const Bands bands;
    double sig[3] {}, err[3] {}, lsdSum[3] {};
    long lsdCount[3] {};
    const size_t length = std::min (ref.size(), out.size());
    for (size_t pos = 0; pos + (size_t) size < length; pos += (size_t) size / 2)
    {
        std::fill (a.begin(), a.end(), 0.0f);
        std::fill (b.begin(), b.end(), 0.0f);
        for (int i = 0; i < size; ++i)
        {
            const float w = (float) (0.5 - 0.5 * std::cos (2.0 * pi * i / size));
            a[(size_t) i] = ref[pos + (size_t) i] * w;
            b[(size_t) i] = out[pos + (size_t) i] * w;
        }
        fft.performRealOnlyForwardTransform (a.data());
        fft.performRealOnlyForwardTransform (b.data());
        double frameMax = 0;
        for (int k = 1; k < size / 2; ++k)
            frameMax = std::max (frameMax, (double) std::hypot (a[(size_t) (2 * k)], a[(size_t) (2 * k + 1)]));
        if (frameMax < 1.0e-3) continue; // silence
        for (int k = 1; k < size / 2; ++k)
        {
            const double f = k * fs / size;
            const int band = f < bands.edges[1] ? 0 : f < bands.edges[2] ? 1 : 2;
            const double ar = a[(size_t) (2 * k)], ai = a[(size_t) (2 * k + 1)], br = b[(size_t) (2 * k)], bi = b[(size_t) (2 * k + 1)];
            const double ma = std::hypot (ar, ai), mb = std::hypot (br, bi);
            sig[band] += ma * ma;
            err[band] += (ar - br) * (ar - br) + (ai - bi) * (ai - bi);
            if (ma > frameMax * 1.0e-3) // bins within 60 dB of the frame's peak
            {
                lsdSum[band] += std::abs (20.0 * std::log10 (std::max (mb, 1.0e-9) / ma));
                ++lsdCount[band];
            }
        }
    }
    BandReport r;
    for (int i = 0; i < 3; ++i)
    {
        r.nullDb[i] = 10.0 * std::log10 (std::max (sig[i], 1.0e-20) / std::max (err[i], 1.0e-20));
        r.lsdDb[i] = lsdCount[i] > 0 ? lsdSum[i] / (double) lsdCount[i] : 0.0;
    }
    return r;
}

/** Side-to-mid energy ratio in dB: how wide a stereo signal is. */
double sideToMidDb (const std::vector<float>& l, const std::vector<float>& r)
{
    double mid = 0, side = 0;
    for (size_t i = 0; i < std::min (l.size(), r.size()); ++i)
    {
        const double m = 0.5 * ((double) l[i] + r[i]), sd = 0.5 * ((double) l[i] - r[i]);
        mid += m * m;
        side += sd * sd;
    }
    return 10.0 * std::log10 (std::max (side, 1.0e-20) / std::max (mid, 1.0e-20));
}


/** Artifact metrics on voiced frames of an aligned reference and processed signal (mono). */
struct Artifacts
{
    double envelope[3] {};       // mean |difference| of the 1/3-octave spectral envelope, dB (lows / mids / highs)
    double flutterIn[3] {}, flutterOut[3] {}; // short-time level flutter per region, dB
    double roughIn = 0, roughOut = 0;          // mean YIN aperiodicity of voiced frames
    double microIn = 0, microOut = 0;          // pitch micro-movement (cents RMS around a 120 ms moving mean)
};

Artifacts artifactMetrics (const std::vector<float>& ref, const std::vector<float>& out, double fs, InputType type)
{
    Artifacts a;
    const size_t length = std::min (ref.size(), out.size());

    // Voiced mask on a 10 ms grid from the reference, plus roughness of both signals.
    const int window = (int) std::ceil (fs / 55.0);
    PitchDetector det;
    det.prepare (fs, window);
    const size_t step = (size_t) (fs / 100.0);
    std::vector<bool> voiced (length / step + 1, false);
    double roughRef = 0, roughOut = 0;
    int count = 0;
    const int minLag = (int) (fs / (type == InputType::soprano ? 1100.0 : 900.0));
    for (size_t end = (size_t) (2 * window), idx = 0; end < length; end += step, ++idx)
    {
        const auto r = det.detect (ref.data() + end - 2 * (size_t) window, window, minLag, 0.15f);
        if (r.hz > 0.0f && r.aperiodicity < 0.25f && r.rms > 0.003f)
        {
            voiced[idx] = true;
            roughRef += r.aperiodicity;
            roughOut += det.detect (out.data() + end - 2 * (size_t) window, window, minLag, 0.15f).aperiodicity;
            ++count;
        }
    }
    a.roughIn = count > 0 ? roughRef / count : 0;
    a.roughOut = count > 0 ? roughOut / count : 0;

    // Pitch micro-movement: how much natural wobble around the slow pitch line survives.
    {
        const size_t pitchStep = (size_t) (fs / 200.0);
        std::vector<double> centsIn, centsOut;
        for (size_t end = (size_t) (2 * window); end < length; end += pitchStep)
        {
            const auto ri = det.detect (ref.data() + end - 2 * (size_t) window, window, minLag, 0.15f);
            const auto ro = det.detect (out.data() + end - 2 * (size_t) window, window, minLag, 0.15f);
            const bool ok = ri.hz > 0.0f && ri.aperiodicity < 0.2f && ri.rms > 0.003f && ro.hz > 0.0f && ro.aperiodicity < 0.3f;
            centsIn.push_back (ok ? 1200.0 * std::log2 (ri.hz / 440.0) : -1.0e9);
            centsOut.push_back (ok ? 1200.0 * std::log2 (ro.hz / 440.0) : -1.0e9);
        }
        const int half = 12; // 25 ms either side of the point at 5 ms steps = about 120 ms in total
        double sumIn = 0, sumOut = 0;
        long n = 0;
        for (int i = half; i + half < (int) centsIn.size(); ++i)
        {
            bool run = true;
            for (int k = -half; k <= half && run; ++k)
                run = centsIn[(size_t) (i + k)] > -1.0e8 && centsOut[(size_t) (i + k)] > -1.0e8
                      && (k == -half || (std::abs (centsIn[(size_t) (i + k)] - centsIn[(size_t) (i + k - 1)]) < 40.0
                                         && std::abs (centsOut[(size_t) (i + k)] - centsOut[(size_t) (i + k - 1)]) < 40.0)); // skip octave errors and note changes
            if (! run) continue;
            double meanIn = 0, meanOut = 0;
            for (int k = -half; k <= half; ++k) { meanIn += centsIn[(size_t) (i + k)]; meanOut += centsOut[(size_t) (i + k)]; }
            meanIn /= 2 * half + 1;
            meanOut /= 2 * half + 1;
            sumIn += (centsIn[(size_t) i] - meanIn) * (centsIn[(size_t) i] - meanIn);
            sumOut += (centsOut[(size_t) i] - meanOut) * (centsOut[(size_t) i] - meanOut);
            ++n;
        }
        a.microIn = n > 0 ? std::sqrt (sumIn / (double) n) : 0;
        a.microOut = n > 0 ? std::sqrt (sumOut / (double) n) : 0;
    }

    // Spectral envelope (1/3-octave band levels) and level flutter, STFT hop 128 / window 1024.
    constexpr int order = 10, size = 1 << order;
    constexpr int hop = 128;
    juce::dsp::FFT fft (order);
    std::vector<float> buf ((size_t) size * 2);
    const double edges[] { 80.0, 100.0, 125.0, 160.0, 200.0, 250.0, 315.0, 400.0, 500.0, 630.0, 800.0, 1000.0, 1250.0, 1600.0, 2000.0,
                           2500.0, 3150.0, 4000.0, 5000.0, 6300.0, 8000.0, 10000.0 };
    constexpr int bandCount = (int) (sizeof (edges) / sizeof (edges[0])) - 1;
    const auto region = [&] (int band) { return edges[band] < 250.0 ? 0 : edges[band] < 2000.0 ? 1 : 2; };
    const auto bandLevels = [&] (const std::vector<float>& x, size_t pos, double* levels)
    {
        std::fill (buf.begin(), buf.end(), 0.0f);
        for (int i = 0; i < size; ++i)
            buf[(size_t) i] = x[pos + (size_t) i] * (float) (0.5 - 0.5 * std::cos (2.0 * pi * i / size));
        fft.performRealOnlyForwardTransform (buf.data());
        double power[bandCount] {};
        int bins[bandCount] {};
        for (int k = 1; k < size / 2; ++k)
        {
            const double f = k * fs / size;
            for (int b = 0; b < bandCount; ++b)
                if (f >= edges[b] && f < edges[b + 1])
                {
                    power[b] += (double) buf[(size_t) (2 * k)] * buf[(size_t) (2 * k)] + (double) buf[(size_t) (2 * k + 1)] * buf[(size_t) (2 * k + 1)];
                    ++bins[b];
                }
        }
        for (int b = 0; b < bandCount; ++b)
            levels[b] = 10.0 * std::log10 (std::max (power[b] / std::max (1, bins[b]), 1.0e-14));
    };

    double envSum[3] {}, flutterSumIn[3] {}, flutterSumOut[3] {};
    long envN[3] {}, flutterN[3] {};
    double prevIn[3][2] {}, prevOut[3][2] {}; // two previous region levels for the second difference
    bool havePrev = false;
    for (size_t pos = 0; pos + (size_t) size < length; pos += (size_t) hop)
    {
        const size_t centre = pos + (size_t) size / 2;
        const size_t idx = std::min (voiced.size() - 1, centre / step);
        double lin[bandCount], lout[bandCount];
        bandLevels (ref, pos, lin);
        bandLevels (out, pos, lout);
        double regIn[3] {}, regOut[3] {};
        int regN[3] {};
        for (int b = 0; b < bandCount; ++b)
        {
            const int r = region (b);
            regIn[r] += lin[b];
            regOut[r] += lout[b];
            ++regN[r];
            if (voiced[idx] && lin[b] > -60.0)
            {
                envSum[r] += std::abs (lout[b] - lin[b]);
                ++envN[r];
            }
        }
        for (int r = 0; r < 3; ++r) { regIn[r] /= regN[r]; regOut[r] /= regN[r]; }
        if (havePrev && voiced[idx])
            for (int r = 0; r < 3; ++r)
                if (regIn[r] > -60.0)
                {
                    const double d2In = regIn[r] - 2.0 * prevIn[r][0] + prevIn[r][1];
                    const double d2Out = regOut[r] - 2.0 * prevOut[r][0] + prevOut[r][1];
                    flutterSumIn[r] += d2In * d2In;
                    flutterSumOut[r] += d2Out * d2Out;
                    ++flutterN[r];
                }
        for (int r = 0; r < 3; ++r)
        {
            prevIn[r][1] = prevIn[r][0];
            prevIn[r][0] = regIn[r];
            prevOut[r][1] = prevOut[r][0];
            prevOut[r][0] = regOut[r];
        }
        havePrev = pos > 0;
    }
    for (int r = 0; r < 3; ++r)
    {
        a.envelope[r] = envN[r] > 0 ? envSum[r] / (double) envN[r] : 0.0;
        a.flutterIn[r] = flutterN[r] > 0 ? std::sqrt (flutterSumIn[r] / (double) flutterN[r]) : 0.0;
        a.flutterOut[r] = flutterN[r] > 0 ? std::sqrt (flutterSumOut[r] / (double) flutterN[r]) : 0.0;
    }
    return a;
}

int analyseFile (const juce::File& file, const juce::File& outDir, InputType inputType)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
    if (reader == nullptr) { std::fprintf (stderr, "Cannot read %s\n", file.getFullPathName().toRawUTF8()); return 2; }
    const double fs = reader->sampleRate;
    const int length = (int) reader->lengthInSamples;
    juce::AudioBuffer<float> buffer (2, length);
    reader->read (&buffer, 0, length, 0, true, true);
    if (reader->numChannels == 1) buffer.copyFrom (1, 0, buffer, 0, 0, length);
    const std::vector<float> inL (buffer.getReadPointer (0), buffer.getReadPointer (0) + length);
    const std::vector<float> inR (buffer.getReadPointer (1), buffer.getReadPointer (1) + length);

    std::printf ("%s: %.1f s, %.0f Hz, %d channel(s), side/mid %.1f dB\n", file.getFileName().toRawUTF8(),
                 length / fs, fs, (int) reader->numChannels, sideToMidDb (inL, inR));

    // Voicing and pitch distribution with the engine's own detector.
    int detectedKey = 0;
    {
        PitchDetector det;
        const int window = (int) std::ceil (fs / 55.0);
        det.prepare (fs, window);
        std::vector<float> mono ((size_t) length);
        for (int i = 0; i < length; ++i) mono[(size_t) i] = 0.5f * (inL[(size_t) i] + inR[(size_t) i]);
        std::vector<float> hz;
        int frames = 0;
        for (size_t end = (size_t) (2 * window); end < mono.size(); end += (size_t) (fs / 100.0), ++frames)
        {
            const auto r = det.detect (mono.data() + end - 2 * (size_t) window, window, (int) (fs / 1100.0), 0.15f);
            if (r.hz > 0.0f && r.aperiodicity < 0.25f && r.rms > 0.003f) hz.push_back (r.hz);
        }
        // Best major key: the one the sung pitches sit closest to.
        if (! hz.empty())
        {
            double best = 1e9;
            for (int key = 0; key < 12; ++key)
            {
                const auto mask = maskForKey (scales[1].intervals, key);
                double sum = 0;
                for (float h : hz)
                {
                    const double cents = 6900.0 + 1200.0 * std::log2 (h / 440.0);
                    sum += std::abs (cents - nearestInScale (cents, mask) * 100.0);
                }
                if (sum / (double) hz.size() < best) { best = sum / (double) hz.size(); detectedKey = key; }
            }
            double chromatic = 0;
            for (float h : hz)
            {
                const double cents = 6900.0 + 1200.0 * std::log2 (h / 440.0);
                chromatic += std::abs (cents - std::round (cents / 100.0) * 100.0);
            }
            std::printf ("  best-fitting major key: %s (mean distance to its notes %.0f ct; to the nearest semitone %.0f ct)\n",
                         noteNames[(size_t) detectedKey], best, chromatic / (double) hz.size());
        }
        std::sort (hz.begin(), hz.end());
        if (! hz.empty())
            std::printf ("  voiced %.0f %% of frames; pitch 10/50/90 %%: %.0f / %.0f / %.0f Hz\n", 100.0 * (double) hz.size() / frames,
                         hz[hz.size() / 10], hz[hz.size() / 2], hz[hz.size() * 9 / 10]);
    }

    struct Config { const char* name; SnaplineParams p; };
    std::vector<Config> configs;
    SnaplineParams base;
    base.inputType = inputType;
    base.allowed = maskForKey (scales[1].intervals, detectedKey);
    {
        auto c = base; c.mode = Mode::studio; c.amount = 0.0f; configs.push_back ({ "studio-amount0", c });
        auto live = c; live.mode = Mode::live; configs.push_back ({ "live-amount0", live });
        auto pop = base; pop.mode = Mode::studio; pop.retuneMs = 18.0f; pop.humanize = 20.0f; configs.push_back ({ "studio-pop", pop });
        auto nat = base; nat.mode = Mode::studio; nat.retuneMs = 85.0f; nat.humanize = 45.0f; nat.tolerance = 35.0f; configs.push_back ({ "studio-natural", nat });
        auto livePop = pop; livePop.mode = Mode::live; configs.push_back ({ "live-pop", livePop });
        auto chroma = pop; chroma.allowed = allNotes; configs.push_back ({ "studio-pop-chrom", chroma });
    }
    std::printf ("  %-16s %-9s %-26s %-26s %-27s %-27s %-18s %s\n", "config", "side/mid", "null dB  <250 / mid / >2k", "spectral dist dB",
                 "envelope dist dB", "level flutter dB in->out", "roughness in->out", "pitch micro-movement ct");
    for (auto& cfg : configs)
    {
        SnaplineDsp dsp;
        const int block = std::getenv ("SNAPLINE_BLOCK") != nullptr ? std::atoi (std::getenv ("SNAPLINE_BLOCK")) : 512;
        dsp.prepare (fs, block, cfg.p.mode);
        std::vector<float> l = inL, r = inR;
        std::vector<float> delayAtBlock;
        std::vector<float> trackedHz;
        float dMin = 1e9f, dMax = 0.0f;
        double dSum = 0.0;
        int dCount = 0;
        const auto start = std::chrono::steady_clock::now();
        for (int pos = 0; pos < length; pos += block)
        {
            const int n = std::min (block, length - pos);
            dsp.process (l.data() + pos, r.data() + pos, l.data() + pos, r.data() + pos, n, cfg.p);
            DisplayFrame frames[256];
            for (int got; (got = dsp.popDisplayFrames (frames, 256)) > 0;)
                for (int i = 0; i < got; ++i)
                    trackedHz.push_back (frames[i].inputHz);
            delayAtBlock.push_back (dsp.getEffectiveDelaySamples());
            if (pos > (int) fs)
            {
                dMin = std::min (dMin, dsp.getEffectiveDelaySamples());
                dMax = std::max (dMax, dsp.getEffectiveDelaySamples());
                dSum += dsp.getEffectiveDelaySamples();
                ++dCount;
            }
        }
        const double seconds = std::chrono::duration<double> (std::chrono::steady_clock::now() - start).count();
        // Align: the reported latency in Studio. Live's delay changes over time, so each block is
        // read back at the delay the engine reported for it.
        const int lag = dsp.getLatencySamples (cfg.p.mode);
        std::vector<float> refL, refR, outL, outR;
        if (cfg.p.mode == Mode::studio)
        {
            refL.assign (inL.begin(), inL.end() - lag);
            refR.assign (inR.begin(), inR.end() - lag);
            outL.assign (l.begin() + lag, l.end());
            outR.assign (r.begin() + lag, r.end());
        }
        else
        {
            const int usable = length - (int) (0.05 * fs);
            for (int n = 0; n < usable; ++n)
            {
                const int d = juce::roundToInt (delayAtBlock[(size_t) (n / block)]);
                refL.push_back (inL[(size_t) n]);
                refR.push_back (inR[(size_t) n]);
                outL.push_back (l[(size_t) (n + d)]);
                outR.push_back (r[(size_t) (n + d)]);
            }
        }
        {
            // Tracker glitches: a jump of more than 70 ct between measured frames that comes back within
            // 5 frames (a one-off error), or a jump by about an octave.
            int octave = 0, spikes = 0, steps = 0;
            float previous = 0.0f;
            for (size_t i = 0; i < trackedHz.size(); ++i)
            {
                const float hz = trackedHz[i];
                if (hz <= 0.0f) { previous = 0.0f; continue; } // a pause is not a glitch: a new note may leap
                if (previous > 0.0f)
                {
                    ++steps;
                    const float ratio = hz / previous;
                    if (ratio > 1.8f || ratio < 0.55f)
                    {
                        ++octave;
                        if (std::getenv ("SNAPLINE_VERBOSE") != nullptr && octave <= 30)
                            std::printf ("      jump at %.2f s: %.0f -> %.0f Hz (x%.2f)\n", (double) i / (fs / (double) 118), (double) previous, (double) hz, (double) ratio);
                    }
                    else if (std::abs (1200.0f * std::log2 (ratio)) > 70.0f)
                        for (size_t k = i + 1; k < std::min (trackedHz.size(), i + 6); ++k)
                            if (trackedHz[k] > 0.0f && std::abs (1200.0f * std::log2 (trackedHz[k] / previous)) < 35.0f) { ++spikes; break; }
                }
                previous = hz;
            }
            if (cfg.p.mode == Mode::studio && std::string (cfg.name) == "studio-pop")
                std::printf ("  [tracker: %d octave jumps and %d one-off spikes in %d measured frames]\n", octave, spikes, steps);
        }
        if (cfg.p.mode == Mode::live)
            std::printf ("  [%s: effective delay %.1f-%.1f ms, mean %.1f ms]\n", cfg.name,
                         dMin * 1000.0 / fs, dMax * 1000.0 / fs, dSum / std::max (1, dCount) * 1000.0 / fs);
        const auto rep = compareBands (refL, outL, fs);
        const auto art = artifactMetrics (refL, outL, fs, inputType);
        std::printf ("  %-16s %+7.1f   %6.1f / %6.1f / %6.1f      %5.2f / %5.2f / %5.2f      %4.2f / %4.2f / %4.2f        %4.2f>%4.2f %4.2f>%4.2f %4.2f>%4.2f   %.3f>%.3f   %4.1f>%4.1f   (%.2f %% CPU)\n", cfg.name,
                     sideToMidDb (outL, outR), rep.nullDb[0], rep.nullDb[1], rep.nullDb[2], rep.lsdDb[0], rep.lsdDb[1], rep.lsdDb[2],
                     art.envelope[0], art.envelope[1], art.envelope[2], art.flutterIn[0], art.flutterOut[0], art.flutterIn[1], art.flutterOut[1],
                     art.flutterIn[2], art.flutterOut[2], art.roughIn, art.roughOut, art.microIn, art.microOut, 100.0 * seconds / (length / fs));
        if (outDir != juce::File())
        {
            outDir.createDirectory();
            juce::WavAudioFormat wav;
            auto out = outDir.getChildFile (juce::String (cfg.name) + ".wav");
            out.deleteFile();
            std::unique_ptr<juce::OutputStream> stream (out.createOutputStream());
            if (stream != nullptr)
                if (auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions().withSampleRate (fs).withNumChannels (2).withBitsPerSample (24)))
                {
                    const float* ch[] { l.data(), r.data() };
                    writer->writeFromFloatArrays (ch, 2, length);
                }
        }
    }
    return 0;
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

    // The detector must not report a multiple of the period (1/2, 1/3 ... of the real pitch) on
    // noisy, breathy frames. Counted over many frames of a steady, noisy harmonic tone.
    if (studio)
    {
        juce::Random rng (11);
        PitchDetector det;
        const int window = (int) std::ceil (fs / 55.0);
        det.prepare (fs, window);
        int frames = 0, wrong = 0;
        for (double f0 : { 110.0, 165.0, 220.0, 330.0 })
            for (double noise : { 0.5, 0.9, 1.3 })
            {
                std::vector<float> x ((size_t) (0.6 * fs));
                double phase = 0.0, lp = 0.0;
                for (auto& v : x)
                {
                    phase += f0 / fs;
                    phase -= std::floor (phase);
                    double s2 = 0.0;
                    for (int k = 1; k <= 8; ++k) s2 += std::sin (2.0 * pi * k * phase) / k;
                    lp += 0.4 * ((rng.nextDouble() * 2.0 - 1.0) - lp);   // low-passed noise: breath in the same region
                    v = (float) (0.3 * s2 + 0.3 * noise * lp);
                }
                for (size_t end = (size_t) (2 * window); end < x.size(); end += (size_t) (fs / 200.0))
                {
                    const auto r = det.detect (x.data() + end - 2 * (size_t) window, window, (int) (fs / 900.0), 0.15f);
                    if (r.hz <= 0.0f || r.aperiodicity > 0.35f) continue; // not voiced, as far as the engine is concerned
                    ++frames;
                    if (std::abs (1200.0 * std::log2 ((double) r.hz / f0)) > 120.0) ++wrong;
                }
            }
        check (frames > 500 && wrong * 100 <= frames * 4, "detector keeps the period on noisy frames (<= 4 %)",
               juce::String::formatted ("%d of %d accepted frames off by more than 120 ct", wrong, frames));
    }

    // True stereo: the channels differ in level and timing (and R partly cancels L in a mono sum).
    // The width must survive, at 0 % amount exactly and while tuning.
    {
        const auto vocal = makeVocal (fs, melody, melodyEnd + 0.4);
        std::vector<float> inL = vocal, inR (vocal.size(), 0.0f);
        const size_t shift = (size_t) (0.002 * fs);
        for (size_t i = shift; i < vocal.size(); ++i)
            inR[i] = -0.6f * vocal[i - shift]; // inverted, quieter and 2 ms later: nothing like a mono signal
        const double widthIn = sideToMidDb (inL, inR);

        const auto render = [&] (SnaplineParams p, std::vector<float>& l, std::vector<float>& r)
        {
            SnaplineDsp dsp;
            dsp.prepare (fs, 256, p.mode);
            l = inL;
            r = inR;
            for (size_t pos = 0; pos < l.size(); pos += 256)
            {
                const int n = (int) std::min<size_t> (256, l.size() - pos);
                dsp.process (l.data() + pos, r.data() + pos, l.data() + pos, r.data() + pos, n, p);
                DisplayFrame frames[256];
                while (dsp.popDisplayFrames (frames, 256) > 0) {}
            }
        };
        std::vector<float> l, r;
        SnaplineParams p;
        p.mode = mode;
        p.allowed = cMajor;
        p.amount = 0.0f;
        render (p, l, r);
        const int lag = probe.getLatencySamples (mode);
        if (studio)
        {
            const double nullL = nullDepth (inL, l, lag), nullR = nullDepth (inR, r, lag);
            check (nullL > 60.0 && nullR > 60.0, "stereo at 0 % amount is transparent per channel (studio)",
                   juce::String::formatted ("null L %.0f dB, R %.0f dB", nullL, nullR));
        }
        else
        {
            const double a = sideToMidDb (l, r);
            check (std::abs (a - widthIn) < 1.5, "stereo width at 0 % amount (live)", juce::String::formatted ("side/mid %.1f dB (input %.1f dB)", a, widthIn));
        }
        p.amount = 100.0f;
        p.retuneMs = 0.0f;
        p.naturalVibrato = -100.0f;
        render (p, l, r);
        const double widthOut = sideToMidDb (l, r);
        const auto accL = accuracy (track (l, fs), delay, melody, [&] (const Note& n) { return nearestInScale (n.midi * 100.0, cMajor) * 100.0; });
        std::vector<float> flipped = r;
        for (auto& v : flipped) v = -v / 0.6f;
        const auto accR = accuracy (track (flipped, fs), delay, melody, [&] (const Note& n) { return nearestInScale (n.midi * 100.0, cMajor) * 100.0; });
        check (std::abs (widthOut - widthIn) < 1.5 && accL.meanAbs < 4.0 && accR.meanAbs < 4.0, ("stereo kept while tuning" + m).toRawUTF8(),
               juce::String::formatted ("side/mid %.1f dB (input %.1f), pitch error L %.1f ct, R %.1f ct", widthOut, widthIn, accL.meanAbs, accR.meanAbs));
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
    // snapline-render --file <recording.wav> [output-directory] [soprano|alto|low|instrument]
    if (argc > 2 && juce::String (argv[1]) == "--file")
    {
        const juce::String type = argc > 4 ? juce::String (argv[4]) : juce::String ("alto");
        const auto inputType = type == "soprano" ? InputType::soprano : type == "low" ? InputType::lowMale : type == "instrument" ? InputType::instrument : InputType::altoTenor;
        return analyseFile (juce::File::getCurrentWorkingDirectory().getChildFile (argv[2]),
                            (argc > 3 && juce::String (argv[3]).isNotEmpty()) ? juce::File::getCurrentWorkingDirectory().getChildFile (argv[3]) : juce::File(), inputType);
    }

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
