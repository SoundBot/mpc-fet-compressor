// tune_presets.cpp - sets Input/Output of FET Compressor's factory presets by measurement.
//
// SPDX-License-Identifier: MIT  (Segeda)
//
// For each preset: a synthetic instrument (peaks at -6 dBFS, a typical MPC track level) runs
// through the real plugin. Input is searched until the average gain reduction hits the preset's
// target; Output is then set so the loudness (RMS) matches the dry signal with the preset's own
// Color and Mix. Prints addProgram() lines for src/FETComp.cpp. Not part of the plugin build.
// The first program, "Default" (the parameter defaults), is written by hand and not tuned here.
//
//   clang++ -std=c++17 -O2 -I "$MPC_SDK/framework" dev/tune_presets.cpp -o build/tune
//   build/tune build/native/fet-compressor.dylib

#include "mpc_vst2_abi.h"

#include <dlfcn.h>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

static VstIntPtr host(AEffect*, VstInt32 op, VstInt32, VstIntPtr, void*, float) { return op == audioMasterVersion ? 2400 : 0; }

static const int SR = 44100, BLOCK = 128, SECONDS = 4;
static const double PI = 3.14159265358979;

struct Rng { uint32_t s = 12345; double next() { s = s * 1664525u + 1013904223u; return (s >> 8) / 8388608.0 - 1.0; } };

// ---------------------------------------------------------------- test instruments
using Sig = std::vector<float>;
static double env(double t, double d) { return t < 0 ? 0 : std::exp(-t / d); }

static Sig kick()  { Sig x(SR * SECONDS); for (size_t n = 0; n < x.size(); ++n) { double t = std::fmod(n / (double)SR, 0.5); double f = 50 + 90 * env(t, 0.03); x[n] = (float)(std::sin(2 * PI * f * t) * env(t, 0.28)); } return x; }
static Sig snare() { Rng r; Sig x(SR * SECONDS); for (size_t n = 0; n < x.size(); ++n) { double t = std::fmod(n / (double)SR + 0.5, 1.0) - 0.5; x[n] = (float)((0.5 * std::sin(2 * PI * 190 * t) + 0.7 * r.next()) * env(t, 0.11)); } return x; }
static Sig hats()  { Rng r; Sig x(SR * SECONDS); double prev = 0; for (size_t n = 0; n < x.size(); ++n) { double t = std::fmod(n / (double)SR, 0.25); double v = r.next(); double hp = v - prev; prev = v; double acc = ((n / (SR / 4)) % 2) ? 0.6 : 1.0; x[n] = (float)(hp * env(t, 0.035) * acc); } return x; }
static Sig drums() { Sig k = kick(), s = snare(), h = hats(), x(k.size()); for (size_t n = 0; n < x.size(); ++n) x[n] = k[n] + 0.8f * s[n] + 0.35f * h[n]; return x; }
static Sig bass()  { Sig x(SR * SECONDS); const double notes[8] = { 55, 55, 65.4, 55, 73.4, 55, 82.4, 65.4 }; double ph = 0; for (size_t n = 0; n < x.size(); ++n) { double t = n / (double)SR; int i = (int)(t / 0.25) % 8; double tt = std::fmod(t, 0.25); ph += notes[i] / SR; double saw = 0; for (int k = 1; k <= 8; ++k) saw += std::sin(2 * PI * k * ph) / k; x[n] = (float)(saw * (0.4 + 0.6 * env(tt, 0.08)) * (tt < 0.22 ? 1 : 0.2)); } return x; }
static Sig b808()  { Sig x(SR * SECONDS); double ph = 0; for (size_t n = 0; n < x.size(); ++n) { double t = std::fmod(n / (double)SR, 1.0); double f = 48 + 30 * env(t, 0.02); ph += f / SR; x[n] = (float)(std::tanh(1.6 * std::sin(2 * PI * ph)) * env(t, 0.7)); } return x; }
static Sig vocal() { Rng r; Sig x(SR * SECONDS); double ph = 0, lp = 0, lvl = 1; for (size_t n = 0; n < x.size(); ++n) { double t = n / (double)SR; double f = 220 * (1 + 0.012 * std::sin(2 * PI * 5.5 * t)); ph += f / SR; double saw = 2 * (ph - std::floor(ph)) - 1; lp += 0.12 * (saw - lp); if (n % (SR / 4) == 0) lvl = 0.35 + 0.65 * std::fabs(r.next()); double syl = std::pow(std::sin(PI * std::fmod(t * 4, 1.0)), 0.6); x[n] = (float)(lp * syl * lvl); } return x; }
static Sig guitar(){ Sig x(SR * SECONDS); const double ch[4] = { 98, 146.8, 196, 246.9 }; for (size_t n = 0; n < x.size(); ++n) { double t = n / (double)SR; double tt = std::fmod(t, 0.5); double v = 0; for (int k = 0; k < 4; ++k) for (int h = 1; h <= 6; ++h) v += std::sin(2 * PI * ch[k] * h * t) / (h * h) * env(tt - k * 0.008, 0.35 / h); x[n] = (float)v; } return x; }
static Sig piano() { Rng r; Sig x(SR * SECONDS); const double nt[4] = { 261.6, 329.6, 392, 523.3 }; double vel = 1; for (size_t n = 0; n < x.size(); ++n) { double t = n / (double)SR; int i = (int)(t / 0.375); double tt = std::fmod(t, 0.375); if (n % (SR * 3 / 8) == 0) vel = 0.4 + 0.6 * std::fabs(r.next()); double v = 0; for (int h = 1; h <= 5; ++h) v += std::sin(2 * PI * nt[i % 4] * h * tt) / h * env(tt, 0.6 / h); x[n] = (float)(v * vel); } return x; }
static Sig lead()  { Sig x(SR * SECONDS); const double nt[8] = { 440, 523, 587, 659, 587, 523, 440, 392 }; double ph = 0; for (size_t n = 0; n < x.size(); ++n) { double t = n / (double)SR; int i = (int)(t / 0.25) % 8; double tt = std::fmod(t, 0.25); ph += nt[i] / SR; double sq = std::fmod(ph, 1.0) < 0.5 ? 1 : -1; x[n] = (float)(sq * (0.7 + 0.3 * env(tt, 0.05)) * (tt < 0.2 ? 1 : 0)); } return x; }
static Sig pluck() { Sig x(SR * SECONDS); const double nt[4] = { 523, 659, 784, 659 }; double ph = 0; for (size_t n = 0; n < x.size(); ++n) { double t = n / (double)SR; int i = (int)(t / 0.125) % 4; double tt = std::fmod(t, 0.125); ph += nt[i] / SR; double saw = 2 * (ph - std::floor(ph)) - 1; x[n] = (float)(saw * env(tt, 0.045)); } return x; }
static Sig pad()   { Sig x(SR * SECONDS); const double ch[4] = { 220, 261.6, 329.6, 392 }; double ph[4] = {}; for (size_t n = 0; n < x.size(); ++n) { double t = n / (double)SR; double v = 0; for (int k = 0; k < 4; ++k) { ph[k] += ch[k] * (1 + 0.003 * k) / SR; v += 2 * (ph[k] - std::floor(ph[k])) - 1; } double swell = 0.55 + 0.45 * std::sin(2 * PI * 0.5 * t); x[n] = (float)(v * swell); } return x; }
static Sig mix()   { Sig d = drums(), b = bass(), p = pad(), x(d.size()); for (size_t n = 0; n < x.size(); ++n) x[n] = d[n] + 0.6f * b[n] + 0.25f * p[n]; return x; }

static void normalise(Sig& x, double peakDb)
{
    float pk = 1e-9f; for (float v : x) pk = std::fmax(pk, std::fabs(v));
    const float g = (float)(std::pow(10.0, peakDb / 20) / pk); for (float& v : x) v *= g;
}

// ---------------------------------------------------------------- plugin driver
struct Preset
{
    const char* name; const char* source; double targetGr;
    double attackUs, releaseMs; int ratio, allBtn; double mix; int hpf; double color;
};

static AEffectDispatcherProc g_disp;
static AEffect* (*g_entry)(audioMasterCallback);

static double rmsDb(const Sig& x, size_t from) { double s = 0; for (size_t n = from; n < x.size(); ++n) s += (double)x[n] * x[n]; return 10 * std::log10(s / (x.size() - from) + 1e-20); }

// Runs the plugin over `in`; returns output RMS (dB) after the first second.
static double render(const Sig& in, const Preset& p, double inputDb, double outputDb, bool clean)
{
    AEffect* e = g_entry(host); auto d = e->dispatcher;
    d(e, effOpen, 0, 0, nullptr, 0); d(e, effSetSampleRate, 0, 0, nullptr, SR); d(e, effSetBlockSize, 0, BLOCK, nullptr, 0); d(e, effMainsChanged, 0, 1, nullptr, 0);
    auto set = [&](int i, double v) { e->setParameter(e, i, (float)std::fmin(1.0, std::fmax(0.0, v))); };
    set(0, (outputDb + 24) / 42); set(1, (inputDb + 12) / 48);
    set(2, std::log(p.attackUs / 20) / std::log(40.0)); set(3, std::log(p.releaseMs / 50) / std::log(22.0));
    set(4, p.ratio / 3.0); set(5, p.allBtn); set(6, clean ? 1.0 : p.mix / 100); set(7, 0); set(8, p.hpf / 3.0); set(9, clean ? 0 : p.color / 100);
    Sig out(in.size()); std::vector<float> L(BLOCK), R(BLOCK); float* io[2] = { L.data(), R.data() };
    for (size_t pos = 0; pos + BLOCK <= in.size(); pos += BLOCK)
    {
        for (int i = 0; i < BLOCK; ++i) L[i] = R[i] = in[pos + i];
        e->processReplacing(e, io, io, BLOCK);
        for (int i = 0; i < BLOCK; ++i) out[pos + i] = L[i];
    }
    d(e, effClose, 0, 0, nullptr, 0);
    return rmsDb(out, SR);
}

int main(int argc, char** argv)
{
    if (argc < 2) { std::fprintf(stderr, "usage: tune <fet-compressor.dylib|.so>\n"); return 1; }
    void* h = dlopen(argv[1], RTLD_NOW); if (!h) { std::fprintf(stderr, "%s\n", dlerror()); return 1; }
    g_entry = (AEffect * (*)(audioMasterCallback)) dlsym(h, "VSTPluginMain");

    struct Src { const char* name; Sig sig; };
    std::vector<Src> srcs = { {"kick", kick()}, {"snare", snare()}, {"hats", hats()}, {"drums", drums()}, {"bass", bass()},
                              {"808", b808()}, {"vocal", vocal()}, {"guitar", guitar()}, {"piano", piano()}, {"lead", lead()},
                              {"pluck", pluck()}, {"pad", pad()}, {"mix", mix()} };
    for (auto& s : srcs) normalise(s.sig, -6.0);

    // name, source, target GR dB, attack us, release ms, ratio (0=4 1=8 2=12 3=20), all, mix %, hpf (0 off 1=60 2=120 3=200), color %
    const Preset presets[] = {
        { "Vocal Smooth",    "vocal",  5, 300, 400, 0, 0, 100, 1, 35 },
        { "Vocal Upfront",   "vocal",  9, 120, 150, 1, 0, 100, 1, 50 },
        { "Drum Bus Glue",   "drums",  3, 600, 200, 0, 0, 100, 2, 40 },
        { "Drum Smash",      "drums", 15,  60,  60, 0, 1,  45, 1, 70 },
        { "Room Nuke",       "drums", 20,  20,  50, 0, 1, 100, 0, 80 },
        { "Bass Grip",       "bass",   6, 500, 300, 1, 0, 100, 0, 55 },
        { "Snare Crack",     "snare",  8, 800,  80, 2, 0, 100, 2, 50 },
        { "Peak Limit",      "mix",    2,  20, 250, 3, 0, 100, 0, 20 },
        { "Kick Punch",      "kick",   6, 800, 100, 0, 0, 100, 0, 40 },
        { "808 Control",     "808",    5, 300, 400, 1, 0, 100, 0, 30 },
        { "Hats Tame",       "hats",   4,  20,  50, 1, 0, 100, 0, 20 },
        { "Loop Pump",       "drums",  8, 400,  50, 3, 0, 100, 1, 45 },
        { "NY Drums",        "drums", 14,  20, 120, 3, 0,  35, 1, 60 },
        { "Rap Vocal",       "vocal", 10,  60, 100, 2, 0, 100, 1, 45 },
        { "Backing Vox",     "vocal",  6, 250, 500, 0, 0, 100, 2, 25 },
        { "Electric Guitar", "guitar", 6, 400, 250, 1, 0, 100, 1, 55 },
        { "Acoustic Guitar", "guitar", 5, 300, 350, 0, 0, 100, 2, 20 },
        { "Piano",           "piano",  4, 500, 500, 0, 0, 100, 0, 25 },
        { "Keys Bite",       "piano",  5, 300, 200, 1, 0, 100, 0, 45 },
        { "Synth Lead",      "lead",   6, 150, 120, 1, 0, 100, 0, 50 },
        { "Pluck Snap",      "pluck",  7, 800,  60, 2, 0, 100, 0, 40 },
        { "Pad Even",        "pad",    4, 400, 900, 0, 0, 100, 0, 20 },
        { "Brass & Strings", "pad",    4, 500, 600, 0, 0, 100, 1, 30 },
        { "Sample Crunch",   "drums", 12, 100, 150, 0, 1,  70, 1, 90 },
        { "Mix Bus",         "mix",    2, 800, 600, 0, 0, 100, 2, 20 },
    };

    std::printf("        //                         Out    In    Atk    Rel  Ratio All  Mix Meter HPF Color\n");
    for (const Preset& p : presets)
    {
        const Sig* sig = nullptr; for (auto& s : srcs) if (std::string(s.name) == p.source) sig = &s.sig;
        const double dryDb = rmsDb(*sig, SR);
        // Gain reduction = (dry level + Input) - clean wet level (no colour, no mix, Output 0).
        auto gr = [&](double in) { return dryDb + in - render(*sig, p, in, 0, true); };
        double lo = -12, hi = 36;
        for (int it = 0; it < 24; ++it) { const double mid = 0.5 * (lo + hi); (gr(mid) < p.targetGr ? lo : hi) = mid; }
        const double in = std::round(0.5 * (lo + hi) * 2) / 2;
        // Output: match loudness with the real Color and Mix.
        double out = 0;
        for (int it = 0; it < 3; ++it) out += dryDb - render(*sig, p, in, out, false);
        out = std::fmin(18, std::fmax(-24, std::round(out * 2) / 2));
        std::printf("        addProgram(%-19s { %5.1ff, %5.1ff, %5.0f.0f, %5.0f.0f, %d, %d, %3.0f, 0, %d, %2.0f });   // %s, %.1f dB GR\n",
                    ("\"" + std::string(p.name) + "\",").c_str(), out, in, p.attackUs, p.releaseMs, p.ratio, p.allBtn, p.mix, p.hpf, p.color,
                    p.source, gr(in));
    }
    return 0;
}
