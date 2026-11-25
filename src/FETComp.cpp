// FET Compressor - a classic FET-style peak compressor for the MPC, by Segeda.
//
// SPDX-License-Identifier: MIT  (original code, Segeda)
//
// Signal flow (stereo, linked detector):
//
//   in --> Input gain --+--> x gain reduction --> FET colour --> DC block --> Output gain --+
//                       |         ^                                                       |
//                       +--> SC HPF --> peak --> dB --> knee/ratio --> attack/release      |
//                                                                                          v
//   in (dry) ------------------------------------------------------------------> Mix --> out
//
// Like the hardware it is modelled on, the threshold is fixed: turn Input up to compress
// harder, then bring the level back with Output. Ratio buttons 4:1, 8:1, 12:1, 20:1; the
// "All Buttons" switch is the famous all-buttons-in mode (over-compression: louder input
// gives a *quieter* output, harder knee, more grit, the needle slams left).
//
// The analog meter: the touch UI shows a VU meter whose needle is a film strip bound to the
// display-only parameter "GR Meter" (always the last parameter). The audio thread computes
// the needle position with meter ballistics and stores it in an atomic; a small worker thread
// writes it into the parameter ~30 times a second and tells the host, so the screen redraws.
// Meter = GR shows gain reduction (rest at 0 VU, swings left); Meter = Out shows the output
// level on a VU scale (0 VU = -18 dBFS RMS).

#include "mpc_plugin.h"
#include "dsp/biquad.h"
#include "dsp/smoother.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>

namespace {

// Parameter order is permanent (saved projects). kMeter stays where it is too: add any
// new parameters after it.
enum ParamIndex
{
    kOutput = 0,   // dB, make-up gain after compression (param 0: harmless when nudged)
    kInput,        // dB, drive into the fixed threshold = amount of compression
    kAttack,       // microseconds (20..800, like the hardware)
    kRelease,      // ms (50..1100)
    kRatio,        // 4:1 / 8:1 / 12:1 / 20:1
    kAllButtons,   // all-buttons-in mode, overrides Ratio
    kMix,          // % compressed signal (parallel compression below 100)
    kMeterMode,    // what the needle shows: GR or Out
    kScHpf,        // detector high-pass, keeps the kick from pumping everything
    kColor,        // % FET / class-A colouration
    kMeter,        // display only: needle position 0..1 (driven by the plugin)
    kNumParams
};

constexpr int   kMaxChannels  = 2;
constexpr int   kMeterFrames  = 48;        // frames in Plugin Skins/vu_meter.png
constexpr float kZeroVuPos    = 0.70795f;  // needle position of 0 VU (scale ends at +3 VU)
constexpr float kOutRefRms    = 0.12589f;  // -18 dBFS RMS = 0 VU on the output meter

// Per ratio button: threshold (dBFS, after Input), knee width (dB), slope (1 - 1/ratio).
struct RatioShape { float thr, knee, slope; };
constexpr RatioShape kRatios[5] = {
    { -18.0f, 8.0f, 1.0f - 1.0f / 4.0f  },
    { -16.0f, 6.0f, 1.0f - 1.0f / 8.0f  },
    { -14.0f, 4.0f, 1.0f - 1.0f / 12.0f },
    { -12.0f, 3.0f, 1.0f - 1.0f / 20.0f },
    { -14.0f, 1.5f, 1.25f },               // all buttons: slope > 1 = over-compression
};

// ~0.03 dB accurate; cheap enough for every sample on the MPC.
inline float fastLog2(float x)
{
    uint32_t i; std::memcpy(&i, &x, 4);
    const float e = (float)(int)((i >> 23) & 255) - 127.0f;
    i = (i & 0x007FFFFFu) | 0x3F800000u;
    float m; std::memcpy(&m, &i, 4);                  // m in [1, 2)
    return e + (-0.34484843f * m + 2.02466578f) * m - 0.67487759f;
}
inline float fastExp2(float x)
{
    if (x < -60.0f) return 0.0f;
    if (x > 30.0f) x = 30.0f;
    const float fl = std::floor(x);
    const float f = x - fl;
    const float p = 1.0f + f * (0.69606564f + f * (0.22449433f + f * 0.07944023f));
    const uint32_t bits = (uint32_t)((int)fl + 127) << 23;
    float s; std::memcpy(&s, &bits, 4);
    return s * p;
}
constexpr float kDbPerLog2 = 6.0205999f;

} // namespace

class FETComp : public mpc::Plugin
{
public:
    FETComp() : mpc::Plugin(makeInfo())
    {
        addParam(mpc::floatParam("output", "Output", -24.0f, 18.0f, 0.0f, "dB", 1));
        addParam(mpc::floatParam("input",  "Input",  -12.0f, 36.0f, 0.0f, "dB", 1));
        addParam(mpc::logParam  ("attack", "Attack",  20.0f, 800.0f, 200.0f, "us", 0));
        addParam(mpc::logParam  ("release","Release", 50.0f, 1100.0f, 250.0f, "ms", 0));
        addParam(mpc::choiceParam("ratio", "Ratio", {"4:1", "8:1", "12:1", "20:1"}, 0));
        addParam(mpc::boolParam ("allbtn", "All Buttons", false));
        addParam(mpc::floatParam("mix",    "Mix",      0.0f, 100.0f, 100.0f, "%", 0));
        addParam(mpc::choiceParam("meter", "Meter", {"GR", "Out"}, 0));
        addParam(mpc::choiceParam("schpf", "SC HPF", {"Off", "60", "120", "200"}, 0));
        addParam(mpc::floatParam("color",  "Color",    0.0f, 100.0f, 40.0f, "%", 0));
        mpc::Param meter = mpc::floatParam("needle", "GR Meter", 0.0f, 1.0f, kZeroVuPos, "", 3);
        meter.automatable = false;   // driven by the plugin, not by the user or automation
        addParam(meter);

        // Programs set only the musical parameters listed (Output .. Color), never the meter.
        // Input/Output are measured by dev/tune_presets.cpp: material peaking at -6 dBFS,
        // target gain reduction per preset, loudness matched. Keep the order: projects store
        // the program number, so append new presets at the end. "Default" comes first and equals
        // the parameter defaults: the MPC labels a newly inserted plugin with program 1's name.
        //                         Out    In    Atk    Rel  Ratio All  Mix Meter HPF Color
        addProgram("Default",          {   0.0f,   0.0f,   200.0f,   250.0f, 0, 0, 100, 0, 0, 40 });
        addProgram("Vocal Smooth",     {  13.5f,  -8.5f,   300.0f,   400.0f, 0, 0, 100, 0, 1, 35 });   // vocal, 5.0 dB GR
        addProgram("Vocal Upfront",    {  11.0f,  -2.0f,   120.0f,   150.0f, 1, 0, 100, 0, 1, 50 });   // vocal, 8.9 dB GR
        addProgram("Drum Bus Glue",    {   7.5f,  -4.5f,   600.0f,   200.0f, 0, 0, 100, 0, 2, 40 });   // drums, 3.0 dB GR
        addProgram("Drum Smash",       {   1.5f,  14.5f,    60.0f,    60.0f, 0, 1,  45, 0, 1, 70 });   // drums, 15.0 dB GR
        addProgram("Room Nuke",        {   6.0f,  14.0f,    20.0f,    50.0f, 0, 1, 100, 0, 0, 80 });   // drums, 19.9 dB GR
        addProgram("Bass Grip",        {  12.5f,  -6.5f,   500.0f,   300.0f, 1, 0, 100, 0, 0, 55 });   // bass, 6.0 dB GR
        addProgram("Snare Crack",      {   6.0f,   2.0f,   800.0f,    80.0f, 2, 0, 100, 0, 2, 50 });   // snare, 8.0 dB GR
        addProgram("Peak Limit",       {   8.5f,  -6.5f,    20.0f,   250.0f, 3, 0, 100, 0, 0, 20 });   // mix, 2.1 dB GR
        addProgram("Kick Punch",       {  11.5f,  -5.5f,   800.0f,   100.0f, 0, 0, 100, 0, 0, 40 });   // kick, 5.9 dB GR
        addProgram("808 Control",      {  13.0f,  -8.0f,   300.0f,   400.0f, 1, 0, 100, 0, 0, 30 });   // 808, 4.9 dB GR
        addProgram("Hats Tame",        {  11.0f,  -7.0f,    20.0f,    50.0f, 1, 0, 100, 0, 0, 20 });   // hats, 4.2 dB GR
        addProgram("Loop Pump",        {  -3.5f,  11.5f,   400.0f,    50.0f, 3, 0, 100, 0, 1, 45 });   // drums, 7.8 dB GR
        addProgram("NY Drums",         {  -1.0f,  16.5f,    20.0f,   120.0f, 3, 0,  35, 0, 1, 60 });   // drums, 14.1 dB GR
        addProgram("Rap Vocal",        {   9.0f,   1.0f,    60.0f,   100.0f, 2, 0, 100, 0, 1, 45 });   // vocal, 10.0 dB GR
        addProgram("Backing Vox",      {  14.5f,  -8.5f,   250.0f,   500.0f, 0, 0, 100, 0, 2, 25 });   // vocal, 6.1 dB GR
        addProgram("Electric Guitar",  {   9.5f,  -3.5f,   400.0f,   250.0f, 1, 0, 100, 0, 1, 55 });   // guitar, 6.2 dB GR
        addProgram("Acoustic Guitar",  {  11.0f,  -6.0f,   300.0f,   350.0f, 0, 0, 100, 0, 2, 20 });   // guitar, 5.0 dB GR
        addProgram("Piano",            {  12.5f,  -8.5f,   500.0f,   500.0f, 0, 0, 100, 0, 0, 25 });   // piano, 3.9 dB GR
        addProgram("Keys Bite",        {   9.0f,  -4.0f,   300.0f,   200.0f, 1, 0, 100, 0, 0, 45 });   // piano, 5.0 dB GR
        addProgram("Synth Lead",       {  14.0f,  -8.0f,   150.0f,   120.0f, 1, 0, 100, 0, 0, 50 });   // lead, 6.2 dB GR
        addProgram("Pluck Snap",       {   8.5f,  -1.5f,   800.0f,    60.0f, 2, 0, 100, 0, 0, 40 });   // pluck, 7.0 dB GR
        addProgram("Pad Even",         {  13.0f,  -9.0f,   400.0f,   900.0f, 0, 0, 100, 0, 0, 20 });   // pad, 4.1 dB GR
        addProgram("Brass & Strings",  {  13.0f,  -9.0f,   500.0f,   600.0f, 0, 0, 100, 0, 1, 30 });   // pad, 4.1 dB GR
        addProgram("Sample Crunch",    {   4.5f,   8.0f,   100.0f,   150.0f, 0, 1,  70, 0, 1, 90 });   // drums, 11.9 dB GR
        addProgram("Mix Bus",          {  10.5f,  -8.5f,   800.0f,   600.0f, 0, 0, 100, 0, 2, 20 });   // mix, 2.0 dB GR

        worker_ = std::thread([this] { meterLoop(); });
    }

    ~FETComp() override
    {
        stop_.store(true);
        if (worker_.joinable()) worker_.join();
    }

    void prepare(double sampleRate, int maxBlockSize) override
    {
        (void)maxBlockSize;
        sr_ = sampleRate > 1000.0 ? (float)sampleRate : 44100.0f;
        inGain_.setTime(20.0f, sr_);
        outGain_.setTime(20.0f, sr_);
        mix_.setTime(20.0f, sr_);
        dcR_ = 1.0f - 2.0f * 3.14159265f * 8.0f / sr_;
        lastHpf_ = -1;
        reset();
    }

    void reset() override
    {
        envDb_ = 0.0f;
        outPower_ = 0.0f;
        needle_ = meterMode() == 0 ? kZeroVuPos : 0.0f;
        needleOut_.store(needle_);
        for (int c = 0; c < kMaxChannels; ++c) { sc_[c].reset(); dcX_[c] = dcY_[c] = 0.0f; }
        inGain_.snap(dbToGain(get(kInput)));
        outGain_.snap(dbToGain(get(kOutput)));
        mix_.snap(get(kMix) * 0.01f);
    }

    void process(mpc::ProcessContext& ctx) override
    {
        const int n = ctx.numFrames;
        const int numOut = mpc::clamp(ctx.numOutputs, 0, kMaxChannels);
        if (n <= 0 || numOut == 0) return;

        // ---- per-block settings --------------------------------------------------------
        const bool all = getBool(kAllButtons);
        const RatioShape& rs = kRatios[all ? 4 : mpc::clamp(getInt(kRatio), 0, 3)];
        const float halfKnee = rs.knee * 0.5f;
        const float kneeK = rs.slope / (2.0f * rs.knee);

        const float atkUs = mpc::clamp(get(kAttack), 20.0f, 800.0f);
        const float relMs = mpc::clamp(get(kRelease), 50.0f, 1100.0f);
        const float aAtt = 1.0f - std::exp(-1.0f / (atkUs * 1.0e-6f * sr_ * (all ? 1.6f : 1.0f)));
        const float aRel = 1.0f - std::exp(-1.0f / (relMs * 1.0e-3f * sr_));

        const int hpf = mpc::clamp(getInt(kScHpf), 0, 3);
        if (hpf != lastHpf_)
        {
            static const float kHz[4] = { 0.0f, 60.0f, 120.0f, 200.0f };
            const mpc::dsp::BiquadCoeffs k = hpf == 0 ? mpc::dsp::BiquadCoeffs::identity()
                                                      : mpc::dsp::BiquadCoeffs::highpass(sr_, kHz[hpf]);
            for (int c = 0; c < kMaxChannels; ++c) sc_[c].setCoeffs(k);
            lastHpf_ = hpf;
        }

        const float color = mpc::clamp(get(kColor), 0.0f, 100.0f) * 0.01f;
        inGain_.setTarget(dbToGain(get(kInput)));
        outGain_.setTarget(dbToGain(get(kOutput)));
        mix_.setTarget(get(kMix) * 0.01f);

        const bool stereoIn = ctx.inputs != nullptr && ctx.numInputs >= 2 && ctx.inputs[1] != nullptr;
        const bool anyIn = ctx.inputs != nullptr && ctx.numInputs >= 1 && ctx.inputs[0] != nullptr;

        float env = envDb_;
        float blockMaxGr = 0.0f;
        float power = 0.0f;

        for (int i = 0; i < n; ++i)
        {
            // Read this frame's input before writing anything (processing is in place).
            const float dryL = anyIn ? ctx.inputs[0][i] : 0.0f;
            const float dryR = stereoIn ? ctx.inputs[1][i] : dryL;

            const float gIn = inGain_.process();
            const float gOut = outGain_.process();
            const float mix = mix_.process();

            const float xL = dryL * gIn, xR = dryR * gIn;

            // Detector: high-passed, stereo-linked peak, in dB.
            const float sL = std::fabs(sc_[0].process(xL));
            const float sR = std::fabs(sc_[1].process(xR));
            const float peak = sL > sR ? sL : sR;
            const float levelDb = kDbPerLog2 * fastLog2(peak + 1.0e-9f);

            // Gain computer with a soft knee: target gain reduction in dB (>= 0).
            const float over = levelDb - rs.thr;
            float target;
            if (over <= -halfKnee) target = 0.0f;
            else if (over < halfKnee) { const float d = over + halfKnee; target = kneeK * d * d; }
            else target = rs.slope * over;

            // Attack / release on the gain reduction itself.
            env += (target > env ? aAtt : aRel) * (target - env);
            if (env > blockMaxGr) blockMaxGr = env;
            const float g = fastExp2(-env * (1.0f / kDbPerLog2));

            // FET colour: even harmonics that grow with gain reduction, then a gentle
            // odd-order ceiling. DC from the even term is removed afterwards.
            const float grAmt = env < 20.0f ? env : 20.0f;
            const float a = color * (0.05f + 0.015f * grAmt) + (all ? 0.12f : 0.0f) * color;
            float yL = xL * g, yR = xR * g;
            yL = colourStage(yL, a, 0);
            yR = colourStage(yR, a, 1);

            const float wL = yL * gOut, wR = yR * gOut;
            const float oL = dryL + (wL - dryL) * mix;
            const float oR = dryR + (wR - dryR) * mix;
            power += 0.5f * (oL * oL + oR * oR);

            ctx.outputs[0][i] = oL;
            if (numOut > 1) ctx.outputs[1][i] = oR;
        }
        envDb_ = env < 1.0e-6f ? 0.0f : env;

        // ---- meter ballistics (per block) ----------------------------------------------
        const float dt = (float)n / sr_;
        float target;
        if (meterMode() == 0)
        {
            const float shown = all ? blockMaxGr * 1.6f : blockMaxGr;   // all-buttons slam
            target = kZeroVuPos * fastExp2(-shown * (1.0f / kDbPerLog2));
            needle_ += (1.0f - std::exp(-dt / 0.045f)) * (target - needle_);
        }
        else
        {
            const float pw = power / (float)n;
            outPower_ += (1.0f - std::exp(-dt / 0.065f)) * (pw - outPower_);   // VU integration
            target = kZeroVuPos * std::sqrt(outPower_ > 0.0f ? outPower_ : 0.0f) / kOutRefRms;
            needle_ += (1.0f - std::exp(-dt / 0.03f)) * (target - needle_);
        }
        if (!std::isfinite(needle_)) needle_ = 0.0f;
        needle_ = mpc::clamp(needle_, 0.0f, 1.0f);
        needleOut_.store(needle_, std::memory_order_relaxed);
    }

    bool formatValue(int index, float plain, std::string& out) override
    {
        char buf[32];
        switch (index)
        {
            case kOutput: case kInput:
                std::snprintf(buf, sizeof buf, "%+.1f dB", (double)plain); out = buf; return true;
            case kAttack:
                std::snprintf(buf, sizeof buf, "%.0f us", (double)plain); out = buf; return true;
            case kRelease:
                std::snprintf(buf, sizeof buf, "%.0f ms", (double)plain); out = buf; return true;
            case kMix: case kColor:
                std::snprintf(buf, sizeof buf, "%.0f %%", (double)plain); out = buf; return true;
            case kScHpf:
                out = plain < 0.5f ? "Off" : (plain < 1.5f ? "60 Hz" : (plain < 2.5f ? "120 Hz" : "200 Hz"));
                return true;
            case kMeter:
            {
                // The reading on the VU scale (GR mode: -dB of gain reduction).
                const float vu = plain > 0.001f ? 20.0f * std::log10(plain / kZeroVuPos) : -60.0f;
                std::snprintf(buf, sizeof buf, "%+.1f VU", (double)vu); out = buf; return true;
            }
            default: return false;
        }
    }

private:
    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo info;
        info.name         = "FET Compressor";
        info.vendor       = "Segeda";   // must match plugin.json "manufacturer"
        info.uniqueId     = MPC_FOURCC(kUid[0], kUid[1], kUid[2], kUid[3]);   // must match plugin.json "uid"
        info.version      = 1410;
        info.isInstrument = false;
        info.numInputs    = 2;
        info.numOutputs   = 2;
        info.tailSamples  = 1;
        return info;
    }

    static constexpr char kUid[5] = "EGVo";

    static float dbToGain(float dB) { return std::pow(10.0f, dB / 20.0f); }
    int meterMode() const { return getInt(kMeterMode); }

    float colourStage(float y, float a, int c)
    {
        y = y + a * y * y;                                    // asymmetric: 2nd harmonic
        if (y > 1.5f) y = 1.5f; else if (y < -1.5f) y = -1.5f;
        y = y - (4.0f / 27.0f / 2.25f) * y * y * y;           // soft ceiling around +-1
        const float out = y - dcX_[c] + dcR_ * dcY_[c];       // DC blocker (~8 Hz)
        dcX_[c] = y;
        dcY_[c] = std::fabs(out) < 1.0e-15f ? 0.0f : out;
        return out;
    }

    // Worker thread: copies the needle into the display parameter and notifies the host.
    // Never touches audio state; only reads the atomic written at the end of process().
    //
    // It also watches for preset (program) changes. Older SDK frameworks set the new values
    // without telling the host, so the MPC kept showing the old knob positions; reporting each
    // value here makes presets update the screen with any SDK version. The report waits one tick
    // so the program has finished writing its values.
    void meterLoop()
    {
        int lastFrame = -1;
        int seenProgram = currentProgram();
        int reportIn = 0;   // ticks until a detected program change is reported
        while (!stop_.load())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(33));
            const int prog = currentProgram();
            if (prog != seenProgram) { seenProgram = prog; reportIn = 2; }
            if (reportIn > 0 && --reportIn == 0)
            {
                for (int i = 0; i < kMeter; ++i) notifyHostParameterChanged(i);
                requestDisplayUpdate();
            }
            const float p = mpc::clamp(needleOut_.load(std::memory_order_relaxed), 0.0f, 1.0f);
            const int frame = mpc::clamp((int)std::lround(p * (kMeterFrames - 1)), 0, kMeterFrames - 1);
            if (frame == lastFrame) continue;
            lastFrame = frame;
            // The centre of the frame's slot, so any frame-rounding rule on the host lands on it.
            setNormalised(kMeter, ((float)frame + 0.5f) / (float)kMeterFrames);
            notifyHostParameterChanged(kMeter);
        }
    }

    mpc::dsp::Biquad sc_[kMaxChannels];
    mpc::dsp::Smoother inGain_, outGain_, mix_;
    float sr_ = 44100.0f;
    float envDb_ = 0.0f;
    float outPower_ = 0.0f;
    float needle_ = kZeroVuPos;
    float dcX_[kMaxChannels] = {}, dcY_[kMaxChannels] = {};
    float dcR_ = 0.999f;
    int   lastHpf_ = -1;

    std::atomic<float> needleOut_{kZeroVuPos};
    std::atomic<bool>  stop_{false};
    std::thread        worker_;
};

MPC_PLUGIN_ENTRY(FETComp)
