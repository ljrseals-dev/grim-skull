// room_theremin.cpp
// Room Theremin — C++20, single translation unit, standard library only.
//
// Build:
//   Linux (live audio):
//     g++ -std=c++20 -O2 -pthread -DROOM_THEREMIN_PULSE room_theremin.cpp -o room_theremin -lpulse-simple -lpulse
//   Linux / macOS / Windows (WAV only):
//     g++/clang++/cl -std=c++20 -O2 -pthread room_theremin.cpp -o room_theremin
//
// Run:
//   ./room_theremin                       # interactive theremin
//   ./room_theremin --wav loop.wav        # loop a WAV as source
//   ./room_theremin --render out.wav      # render 20 s and exit
//   ./room_theremin --render out.wav --duration 8
//
// Stdin commands (one per line):
//   W=12 D=16 H=6 A=0.25     room
//   X=6 Y=8                  source position
//   T=440 V=0.66             theremin pitch / level (T=0 silences)
//   R=hall|plate|off         reverb
//   M=1.0 E=1.0 DM=0.25      mix / early / hall damping
//   PD=0.62 PDM=0.3 PP=12    plate decay / damp / predelay ms
//   L=0.7 C=1                master / couple
//   P                        print params
//   Q                        quit

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <numbers>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// ------------------------------------------------------------ config
constexpr double kSR    = 48000.0;
constexpr int    kBlock = 256;
constexpr double kC     = 343.0;
constexpr double kPi    = std::numbers::pi;
constexpr double kLn2   = std::numbers::ln2;

// ------------------------------------------------------------ WAV I/O
namespace wav {
struct Audio {
    int sampleRate = 48000;
    int channels   = 1;
    std::vector<float> interleaved;
};

inline uint32_t rd32(const uint8_t* p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24);
}
inline uint16_t rd16(const uint8_t* p) {
    return uint16_t(p[0] | (p[1] << 8));
}

inline Audio load(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + path);
    f.seekg(0, std::ios::end);
    auto n = f.tellg();
    f.seekg(0);
    std::vector<uint8_t> b(static_cast<size_t>(n));
    f.read(reinterpret_cast<char*>(b.data()), n);
    if (b.size() < 44 || std::memcmp(b.data(), "RIFF", 4) ||
        std::memcmp(b.data() + 8, "WAVE", 4))
        throw std::runtime_error("not a WAV");

    Audio a;
    size_t i = 12;
    uint16_t fmt = 1, bits = 16;
    while (i + 8 <= b.size()) {
        uint32_t id = rd32(&b[i]), sz = rd32(&b[i + 4]);
        if (id == 0x20746d66) { // "fmt "
            fmt          = rd16(&b[i + 8]);
            a.channels   = rd16(&b[i + 10]);
            a.sampleRate = int(rd32(&b[i + 12]));
            bits         = rd16(&b[i + 22]);
        } else if (id == 0x61746164) { // "data"
            size_t start = i + 8, end = std::min(b.size(), start + sz);
            size_t bps   = bits / 8;
            size_t frames = (end - start) / (bps * size_t(a.channels));
            a.interleaved.resize(frames * size_t(a.channels));
            for (size_t k = 0; k < a.interleaved.size(); ++k) {
                const uint8_t* p = &b[start + k * bps];
                if (fmt == 3 && bits == 32) {
                    float v;
                    std::memcpy(&v, p, 4);
                    a.interleaved[k] = v;
                } else if (bits == 16) {
                    int16_t v = int16_t(rd16(p));
                    a.interleaved[k] = v / 32768.f;
                } else if (bits == 24) {
                    int32_t v = (p[0] << 8) | (p[1] << 16) | (int32_t(p[2]) << 24);
                    a.interleaved[k] = float(v >> 8) / 8388608.f;
                } else if (bits == 32) {
                    int32_t v;
                    std::memcpy(&v, p, 4);
                    a.interleaved[k] = float(v) / 2147483648.f;
                } else
                    throw std::runtime_error("unsupported bit depth");
            }
            return a;
        }
        i += 8 + sz + (sz & 1);
    }
    throw std::runtime_error("no data chunk");
}

inline void write(const std::string& path, const Audio& a) {
    std::ofstream f(path, std::ios::binary);
    uint32_t dataBytes = uint32_t(a.interleaved.size() * 2);
    auto w32 = [&](uint32_t v) { f.write(reinterpret_cast<char*>(&v), 4); };
    auto w16 = [&](uint16_t v) { f.write(reinterpret_cast<char*>(&v), 2); };
    f.write("RIFF", 4);
    w32(36 + dataBytes);
    f.write("WAVE", 4);
    f.write("fmt ", 4);
    w32(16);
    w16(1);
    w16(uint16_t(a.channels));
    w32(uint32_t(a.sampleRate));
    w32(uint32_t(a.sampleRate * a.channels * 2));
    w16(uint16_t(a.channels * 2));
    w16(16);
    f.write("data", 4);
    w32(dataBytes);
    for (float v : a.interleaved) {
        int16_t s = int16_t(std::clamp(v, -1.f, 1.f) * 32767.f);
        f.write(reinterpret_cast<char*>(&s), 2);
    }
}
} // namespace wav

// ------------------------------------------------------------ Params + seqlock
struct Params {
    double W = 12, D = 16, H = 6, alpha = 0.25;
    double sx = 6, sy = 8, sz = 1.5;
    double lx = 6, ly = 2.8, lz = 1.5;
    int    sourceType = 0; // 0 theremin, 1 wav
    double thereminHz = 440, thereminLevel = 0.66, vibratoCents = 12;
    int    reverbType = 0; // 0 hall, 1 plate, 2 off
    double mix = 1.0, earlyLevel = 1.0;
    double hallDamp = 0.25, tailGain = 1.0;
    double plateDecay = 0.62, plateDamp = 0.3, platePredelayMs = 12;
    double level = 0.7;
    bool   couple = true;
    // derived (UI thread)
    double rt60 = 1.9, dc = 1.4;
    int    taps = 0;
};

class ParamBus {
    std::atomic<uint32_t> seq_{0};
    Params p_{};
public:
    void write(const Params& in) {
        uint32_t s = seq_.load(std::memory_order_relaxed);
        seq_.store(s + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        p_ = in;
        std::atomic_thread_fence(std::memory_order_release);
        seq_.store(s + 2, std::memory_order_release);
    }
    Params read() const {
        for (;;) {
            uint32_t s1 = seq_.load(std::memory_order_acquire);
            if (s1 & 1) {
                std::this_thread::yield();
                continue;
            }
            Params out = p_;
            std::atomic_thread_fence(std::memory_order_acquire);
            uint32_t s2 = seq_.load(std::memory_order_relaxed);
            if (s1 == s2) return out;
        }
    }
};

// ------------------------------------------------------------ primitives
class DelayLine {
    std::vector<float> buf_;
    size_t w_ = 0;
public:
    explicit DelayLine(size_t maxLen) : buf_(maxLen + 8, 0.f) {}
    size_t size() const { return buf_.size(); }
    float read(double d) const {
        long i = long(w_) - long(std::floor(d));
        const long n = long(buf_.size());
        while (i < 0) i += n;
        float a = buf_[size_t(i)];
        float b = buf_[size_t((i + 1) % n)];
        double f = d - std::floor(d);
        return float(a + (b - a) * f);
    }
    void write(float x) {
        buf_[w_] = x;
        w_ = (w_ + 1) % buf_.size();
    }
};

class Allpass {
public:
    DelayLine line;
    double len, g, exc;
    Allpass(double len_, double g_, double exc_ = 0)
        : line(size_t(std::ceil(len_ + std::ceil(exc_) + 8))),
          len(len_), g(g_), exc(exc_) {}
    float process(float x, double mod = 0.0) {
        double d = exc > 0 ? len + exc * mod : len;
        float v = float(x + g * line.read(d));
        line.write(v);
        return float(line.read(d) - g * v);
    }
};

// ------------------------------------------------------------ Hall (FDN + image-source ER)
class Hall {
    static constexpr int N = 8;
    std::array<DelayLine, N> lines_ = {
        DelayLine(size_t(0.4 * kSR)), DelayLine(size_t(0.4 * kSR)),
        DelayLine(size_t(0.4 * kSR)), DelayLine(size_t(0.4 * kSR)),
        DelayLine(size_t(0.4 * kSR)), DelayLine(size_t(0.4 * kSR)),
        DelayLine(size_t(0.4 * kSR)), DelayLine(size_t(0.4 * kSR))};
    std::array<double, N> len_{}, lenT_{}, g_{}, gT_{}, lp_{}, da_{};
    double damp_ = 0.25, tailGain_ = 1.0, tailAuto_ = 1.0, erGain_ = 1.0;
    DelayLine er_{size_t(0.6 * kSR)};
    DelayLine pre_{size_t(0.5 * kSR)};
    size_t preSamples_ = 480;
    struct Tap {
        double delay, gl, gr;
    };
    std::vector<Tap> cur_, next_;
    double xf_ = 1.0;
    int lastTaps_ = 0;

    static bool isPrime(uint64_t n) {
        if (n < 2) return false;
        for (uint64_t i = 2; i * i <= n; ++i)
            if (n % i == 0) return false;
        return true;
    }
    static double nearPrime(double n) {
        n = std::max(7.0, std::round(n));
        for (int k = 0; k < 200; ++k) {
            if (isPrime(uint64_t(n + k))) return n + k;
            if (n - k > 2 && isPrime(uint64_t(n - k))) return n - k;
        }
        return n;
    }
    static std::vector<Tap> earlyTaps(const Params& p, int maxTaps) {
        const double beta = std::sqrt(std::max(0.0, 1.0 - p.alpha));
        const double s[3] = {p.sx, p.sy, p.sz};
        const double l[3] = {p.lx, p.ly, p.lz};
        const double dims[3] = {p.W, p.D, p.H};
        std::vector<Tap> taps;
        const int rng[5] = {-2, -1, 0, 1, 2};
        for (int q0 = 0; q0 < 2; ++q0)
            for (int q1 = 0; q1 < 2; ++q1)
                for (int q2 = 0; q2 < 2; ++q2)
                    for (int n0 : rng)
                        for (int n1 : rng)
                            for (int n2 : rng) {
                                const int q[3] = {q0, q1, q2};
                                const int n[3] = {n0, n1, n2};
                                int refl = 0;
                                for (int a = 0; a < 3; ++a)
                                    refl += std::abs(n[a] - q[a]) + std::abs(n[a]);
                                if (refl == 0 || refl > 3) continue;
                                double img[3];
                                for (int a = 0; a < 3; ++a)
                                    img[a] = (1 - 2 * q[a]) * s[a] + 2.0 * n[a] * dims[a];
                                double dx = img[0] - l[0], dy = img[1] - l[1],
                                       dz = img[2] - l[2];
                                double dist = std::sqrt(dx * dx + dy * dy + dz * dz);
                                if (dist < 0.3) continue;
                                double amp = std::pow(beta, double(refl)) / dist;
                                double lat = std::clamp(dx / std::max(dist, 1e-3), -1.0, 1.0);
                                double ang = (lat + 1.0) * kPi / 4.0;
                                taps.push_back(
                                    {dist / kC * kSR, amp * std::cos(ang), amp * std::sin(ang)});
                            }
        std::sort(taps.begin(), taps.end(), [](const Tap& a, const Tap& b) {
            return a.gl * a.gl + a.gr * a.gr > b.gl * b.gl + b.gr * b.gr;
        });
        if (int(taps.size()) > maxTaps) taps.resize(size_t(maxTaps));
        return taps;
    }
    void recomputeDamp() {
        // sample-rate-aware version of the original 1200 constant
        const double scale = kSR * 0.025;
        for (int i = 0; i < N; ++i)
            da_[i] = std::clamp(damp_ * lenT_[i] / scale, 0.0, 0.95);
    }

public:
    Hall() {
        Params p;
        apply(p, true);
        len_ = lenT_;
        g_ = gT_;
        cur_ = next_;
        next_.clear();
        xf_ = 1.0;
    }
    void setDamp(double d) {
        damp_ = std::clamp(d, 0.0, 0.9);
        recomputeDamp();
    }
    void setTail(double g) { tailGain_ = g; }
    void setEarly(double g) { erGain_ = g; }
    int lastTapCount() const { return lastTaps_; }

    void apply(const Params& p, bool snap = false) {
        const double V = p.W * p.D * p.H;
        const double S = 2.0 * (p.W * p.D + p.W * p.H + p.D * p.H);
        const double alpha = std::clamp(p.alpha, 0.02, 0.97);
        const double rt60 =
            std::clamp(-0.161 * V / (S * std::log(1.0 - alpha)), 0.15, 12.0);
        const double mfp = 4.0 * V / S;
        const double base =
            std::min(0.45 * double(lines_[0].size()), (mfp / kC) * kSR);
        const double f[8] = {0.55, 0.68, 0.80, 0.93, 1.05, 1.19, 1.33, 1.50};
        for (int i = 0; i < N; ++i) {
            lenT_[i] = nearPrime(base * f[i]);
            gT_[i] = std::pow(10.0, -3.0 * lenT_[i] / (kSR * rt60));
        }
        recomputeDamp();
        const double Aeq = S * -std::log(1.0 - alpha);
        double avgLen = 0;
        for (double L : lenT_) avgLen += L;
        avgLen /= N;
        const double eFdn = 0.86 * 2.0 * 0.1225 * (8.0 * 0.1225) *
                            (rt60 * kSR) / (13.8 * avgLen);
        tailAuto_ = std::sqrt((16.0 * kPi) / Aeq / eFdn);
        const double dDirect =
            std::sqrt((p.sx - p.lx) * (p.sx - p.lx) + (p.sy - p.ly) * (p.sy - p.ly) +
                      (p.sz - p.lz) * (p.sz - p.lz));
        preSamples_ = size_t(std::clamp(dDirect / kC * kSR * 0.6, 1.0,
                                        double(pre_.size()) - 16));
        next_ = earlyTaps(p, 40);
        lastTaps_ = int(next_.size());
        xf_ = 0.0;
        if (snap) {
            len_ = lenT_;
            g_ = gT_;
            cur_ = next_;
            next_.clear();
            xf_ = 1.0;
        }
    }

    void process(float inL, float inR, float& outL, float& outR) {
        const float x = 0.5f * (inL + inR);
        er_.write(x);
        pre_.write(x);
        const float xin = pre_.read(double(preSamples_));
        float eL = 0, eR = 0;
        for (auto& t : cur_) {
            float v = er_.read(t.delay);
            eL += v * float(t.gl);
            eR += v * float(t.gr);
        }
        if (!next_.empty()) {
            float nL = 0, nR = 0;
            for (auto& t : next_) {
                float v = er_.read(t.delay);
                nL += v * float(t.gl);
                nR += v * float(t.gr);
            }
            eL = float(eL * (1.0 - xf_) + nL * xf_);
            eR = float(eR * (1.0 - xf_) + nR * xf_);
            xf_ += 1.0 / (0.08 * kSR);
            if (xf_ >= 1.0) {
                cur_ = next_;
                next_.clear();
                xf_ = 1.0;
            }
        }
        double y[N];
        for (int k = 0; k < N; ++k) {
            len_[k] += std::clamp(lenT_[k] - len_[k], -0.02, 0.02);
            g_[k] += (gT_[k] - g_[k]) * 0.0005;
            double o = lines_[k].read(len_[k]);
            lp_[k] = (1.0 - da_[k]) * o + da_[k] * lp_[k];
            y[k] = lp_[k] * g_[k];
        }
        for (int h = 1; h < N; h <<= 1)
            for (int j = 0; j < N; j += h << 1)
                for (int k = j; k < j + h; ++k) {
                    double a = y[k], b = y[k + h];
                    y[k] = a + b;
                    y[k + h] = a - b;
                }
        const double norm = 1.0 / std::sqrt(double(N));
        static const int signL[N] = {1, 1, -1, -1, 1, 1, -1, -1};
        static const int signR[N] = {1, -1, 1, -1, 1, -1, 1, -1};
        static const int inSign[N] = {1, -1, 1, 1, -1, 1, -1, -1};
        double tL = 0, tR = 0;
        for (int k = 0; k < N; ++k) {
            double m = y[k] * norm;
            lines_[k].write(float(m + xin * inSign[k] * 0.35));
            tL += signL[k] * m;
            tR += signR[k] * m;
        }
        const double ta = 0.35 * tailAuto_ * tailGain_;
        outL += float(eL * erGain_ + tL * ta);
        outR += float(eR * erGain_ + tR * ta);
    }
};

// ------------------------------------------------------------ Plate (Dattorro)
class Plate {
    DelayLine pre_{size_t(0.25 * kSR)};
    size_t preSamples_ = 0;
    double bw_ = 0, bwCoef_ = 0.9995;
    std::array<Allpass, 4> diff_{
        Allpass(142, 0.75), Allpass(107, 0.75), Allpass(379, 0.625), Allpass(277, 0.625)};
    struct Tank {
        Allpass mod;
        DelayLine d1;
        Allpass ap2;
        DelayLine d2;
        double d1len = 0, d2len = 0, lp = 0, fb = 0;
        Tank()
            : mod(2, -0.7), d1(8), ap2(2, 0.5), d2(8) {}
        Tank(double modLen, double ap2Len, double d1Len, double d2Len, double g2, double exc)
            : mod(modLen, -0.7, exc),
              d1(size_t(d1Len + 8)),
              ap2(ap2Len, g2),
              d2(size_t(d2Len + 8)),
              d1len(d1Len),
              d2len(d2Len) {}
    };
    Tank tL_, tR_;
    double decay_ = 0.62, damp_ = 0.3, phase_ = 0;
    double lfoInc_ = 2 * kPi * 0.8 / kSR;

    static double scaled(double n) {
        return std::max(2.0, std::round(n * (kSR / 29761.0)));
    }
    void rebuild() {
        const double s = kSR / 29761.0;
        const double exc = 16.0 * s;
        const double g2 = std::clamp(decay_ + 0.15, 0.25, 0.5);
        tL_ = Tank(scaled(672), scaled(1800), scaled(4453), scaled(3720), g2, exc);
        tR_ = Tank(scaled(908), scaled(2656), scaled(4217), scaled(3163), g2, exc);
        // scale the four input allpasses
        diff_ = {Allpass(scaled(142), 0.75), Allpass(scaled(107), 0.75),
                 Allpass(scaled(379), 0.625), Allpass(scaled(277), 0.625)};
    }

public:
    Plate() { rebuild(); }
    void setDecay(double d) {
        decay_ = std::clamp(d, 0.1, 0.97);
        rebuild();
    }
    void setDamp(double d) { damp_ = std::clamp(d, 0.0, 0.95); }
    void setPredelayMs(double ms) {
        preSamples_ =
            size_t(std::clamp(ms / 1000.0 * kSR, 0.0, double(pre_.size()) - 16));
    }
    void process(float inL, float inR, float& outL, float& outR) {
        float x = 0.5f * (inL + inR);
        pre_.write(x);
        x = pre_.read(double(std::max<size_t>(1, preSamples_)));
        bw_ = bwCoef_ * x + (1.0 - bwCoef_) * bw_;
        float y = float(bw_);
        for (auto& ap : diff_) y = ap.process(y);
        phase_ += lfoInc_;
        const double sl = std::sin(phase_), sr = std::cos(phase_);
        auto step = [&](Tank& t, double mod, Tank& other) {
            float v = float(y + other.fb * decay_);
            v = t.mod.process(v, mod);
            double o1 = t.d1.read(t.d1len);
            t.d1.write(v);
            t.lp = (1.0 - damp_) * o1 + damp_ * t.lp;
            v = float(t.lp * decay_);
            v = t.ap2.process(v);
            double o2 = t.d2.read(t.d2len);
            t.d2.write(v);
            t.fb = o2;
        };
        step(tL_, sl, tR_);
        step(tR_, sr, tL_);
        auto L = [&](double n) { return scaled(n); };
        double oL = tR_.d1.read(L(266)) + tR_.d1.read(L(2974)) -
                    tR_.ap2.line.read(L(1913)) + tR_.d2.read(L(1996)) -
                    tL_.d1.read(L(1990)) - tL_.ap2.line.read(L(187)) -
                    tL_.d2.read(L(1066));
        double oR = tL_.d1.read(L(353)) + tL_.d1.read(L(3627)) -
                    tL_.ap2.line.read(L(1228)) + tL_.d2.read(L(2673)) -
                    tR_.d1.read(L(2111)) - tR_.ap2.line.read(L(335)) -
                    tR_.d2.read(L(121));
        outL += float(0.6 * oL);
        outR += float(0.6 * oR);
    }
};

// ------------------------------------------------------------ Theremin
class Theremin {
    double phaseA_ = 0, phaseB_ = 0, phaseVib_ = 0;
    double hz_ = 440, level_ = 0, vibCents_ = 12;
public:
    void setHz(double hz) { hz_ = std::clamp(hz, 20.0, 4000.0); }
    void setLevel(double l) { level_ = std::clamp(l, 0.0, 1.0); }
    void setVibrato(double cents) { vibCents_ = std::clamp(cents, 0.0, 50.0); }
    float tick() {
        const double vib = vibCents_ * (kLn2 / 1200.0);
        const double f = hz_ * std::exp(std::sin(phaseVib_) * vib);
        phaseA_ += 2 * kPi * f / kSR;
        phaseB_ += 2 * kPi * f * std::exp((9.0 / 1200.0) * kLn2) / kSR;
        if (phaseA_ > 2 * kPi) phaseA_ -= 2 * kPi;
        if (phaseB_ > 2 * kPi) phaseB_ -= 2 * kPi;
        phaseVib_ += 2 * kPi * 5.35 / kSR;
        if (phaseVib_ > 2 * kPi) phaseVib_ -= 2 * kPi;
        const double sawA = 2.0 * (phaseA_ / (2 * kPi)) - 1.0;
        const double sawB = 2.0 * (phaseB_ / (2 * kPi)) - 1.0;
        return float((0.16 * sawA + 0.13 * sawB) * level_);
    }
};

// ------------------------------------------------------------ WAV looper
class WavSource {
    wav::Audio a_;
    size_t pos_ = 0;
public:
    void load(const wav::Audio& a) {
        a_ = a;
        pos_ = 0;
    }
    bool loaded() const { return !a_.interleaved.empty(); }
    float tick() {
        if (a_.interleaved.empty()) return 0.f;
        const size_t frames = a_.interleaved.size() / size_t(a_.channels);
        float v = 0;
        for (int c = 0; c < a_.channels; ++c)
            v += a_.interleaved[pos_ * size_t(a_.channels) + size_t(c)];
        v /= float(a_.channels);
        pos_ = (pos_ + 1) % frames;
        return v;
    }
};

// ------------------------------------------------------------ simple binaural panner (ILD + ITD)
class Panner {
    std::array<float, 128> buf_{};
    size_t w_ = 0;
    double gainL_ = 0.7, gainR_ = 0.7, itdL_ = 0, itdR_ = 0;
public:
    void set(double azRad, double dist) {
        const double a = std::clamp(azRad, -kPi / 2, kPi / 2);
        const double pan = std::sin(a);
        const double g = 1.0 / std::max(0.5, dist);
        gainL_ = g * std::sqrt(0.5 * (1.0 - pan));
        gainR_ = g * std::sqrt(0.5 * (1.0 + pan));
        const double itdSec = 0.0875 * std::sin(a) / kC; // Woodworth
        itdL_ = itdSec * kSR;
        itdR_ = -itdL_;
    }
    void process(float in, float& outL, float& outR) {
        buf_[w_] = in;
        auto tap = [&](double d) -> float {
            long i = long(w_) - long(std::floor(d));
            while (i < 0) i += 128;
            return buf_[size_t(i % 128)];
        };
        outL = tap(std::max(0.0, itdL_)) * float(gainL_);
        outR = tap(std::max(0.0, itdR_)) * float(gainR_);
        w_ = (w_ + 1) % 128;
    }
};

// ------------------------------------------------------------ Engine
class Engine {
    ParamBus& bus_;
    Hall hall_;
    Plate plate_;
    Theremin theremin_;
    WavSource wavSrc_;
    Panner panner_;
    struct Biquad {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
        void lowpass(double fc, double q) {
            fc = std::clamp(fc, 20.0, kSR * 0.45);
            double w = 2 * kPi * fc / kSR;
            double cs = std::cos(w), sn = std::sin(w), al = sn / (2 * q);
            double a0 = 1 + al;
            b0 = (1 - cs) / 2 / a0;
            b1 = (1 - cs) / a0;
            b2 = b0;
            a1 = -2 * cs / a0;
            a2 = (1 - al) / a0;
        }
        float process(float x) {
            double y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return float(y);
        }
    } air_, vf_;
    DelayLine wallDelay_{size_t(0.2 * kSR)};
    double wallGain_ = 0, wallTime_ = 0.001;
    std::atomic<float> peak_{0};
    Params last_{};

    // strongest first-order reflection (for the dry wall tap)
    static void strongestReflection(const Params& p, double& delay, double& rel) {
        const double beta = std::sqrt(1.0 - std::clamp(p.alpha, 0.02, 0.97));
        const double s[3] = {p.sx, p.sy, p.sz};
        const double l[3] = {p.lx, p.ly, p.lz};
        const double dims[3] = {p.W, p.D, p.H};
        double dd = std::max(
            0.3, std::sqrt((s[0] - l[0]) * (s[0] - l[0]) + (s[1] - l[1]) * (s[1] - l[1]) +
                           (s[2] - l[2]) * (s[2] - l[2])));
        double bestRel = 0, bestDelay = 0.001;
        for (int ax = 0; ax < 3; ++ax)
            for (int side = 0; side < 2; ++side) {
                double img[3] = {s[0], s[1], s[2]};
                img[ax] = side == 0 ? -s[ax] : 2 * dims[ax] - s[ax];
                double di = std::sqrt((img[0] - l[0]) * (img[0] - l[0]) +
                                      (img[1] - l[1]) * (img[1] - l[1]) +
                                      (img[2] - l[2]) * (img[2] - l[2]));
                double r = beta * dd / di;
                if (r > bestRel) {
                    bestRel = r;
                    bestDelay = (di - dd) / kC;
                }
            }
        delay = std::clamp(bestDelay, 0.0003, 0.19);
        rel = bestRel;
    }

public:
    explicit Engine(ParamBus& bus) : bus_(bus) { last_ = bus.read(); }
    void loadWav(const std::string& path) {
        wavSrc_.load(wav::load(path));
        last_.sourceType = 1;
    }
    float peak() const { return peak_.load(); }
    int lastTaps() const { return hall_.lastTapCount(); }

    void syncParams(const Params& p) {
        theremin_.setHz(p.thereminHz);
        theremin_.setLevel(p.sourceType == 0 ? p.thereminLevel : 0.0);
        theremin_.setVibrato(p.vibratoCents);
        hall_.setDamp(p.hallDamp);
        hall_.setTail(p.reverbType == 0 ? p.mix : 0.0);
        hall_.setEarly(p.earlyLevel);
        plate_.setDecay(p.plateDecay);
        plate_.setDamp(p.plateDamp);
        plate_.setPredelayMs(p.platePredelayMs);

        // geometry change → recompute FDN + early taps (UI thread only)
        if (p.W != last_.W || p.D != last_.D || p.H != last_.H ||
            p.alpha != last_.alpha || p.sx != last_.sx || p.sy != last_.sy ||
            p.sz != last_.sz || p.lx != last_.lx || p.ly != last_.ly ||
            p.lz != last_.lz) {
            hall_.apply(p, false);
        }

        // distance & azimuth for panner + air + wall
        const double dx = p.sx - p.lx;
        const double dy = p.sy - p.ly;
        const double dist = std::hypot(dx, dy);
        const double az = std::atan2(dx, dy); // 0 = front (+y)
        panner_.set(az, dist);

        // air absorption (always on)
        const double airFc = std::clamp(16000.0 / (1.0 + 0.09 * dist), 2800.0, 16000.0);
        air_.lowpass(airFc, 0.5);

        // source softening for theremin filter
        const double soften = p.couple ? 1.0 / (1.0 + 0.05 * dist) : 1.0;
        const double open = 0.68 + 0.32 * std::sqrt(std::max(0.0, p.thereminLevel));
        const double vfFc =
            std::clamp(p.thereminHz * 4.2 * open * soften, 380.0, 8600.0);
        vf_.lowpass(vfFc, 0.75);

        // wall tap
        double wrDelay = 0.001, wrRel = 0;
        strongestReflection(p, wrDelay, wrRel);
        wallTime_ = wrDelay;
        wallGain_ = p.couple ? 0.55 * wrRel : 0.0;

        last_ = p;
    }

    void processBlock(float* outL, float* outR, int n) {
        Params p = bus_.read();
        // cheap per-block updates that do not allocate
        if (p.thereminHz != last_.thereminHz || p.thereminLevel != last_.thereminLevel ||
            p.vibratoCents != last_.vibratoCents || p.sourceType != last_.sourceType ||
            p.mix != last_.mix || p.earlyLevel != last_.earlyLevel ||
            p.hallDamp != last_.hallDamp || p.plateDecay != last_.plateDecay ||
            p.plateDamp != last_.plateDamp || p.platePredelayMs != last_.platePredelayMs ||
            p.reverbType != last_.reverbType || p.couple != last_.couple ||
            p.sx != last_.sx || p.sy != last_.sy || p.W != last_.W || p.D != last_.D ||
            p.H != last_.H || p.alpha != last_.alpha) {
            // full sync is cheap enough; heavy work already done on UI thread
            syncParams(p);
        }

        float peak = 0;
        for (int i = 0; i < n; ++i) {
            float src = 0;
            if (p.sourceType == 0)
                src = theremin_.tick();
            else
                src = wavSrc_.tick();

            // soft filter on theremin
            if (p.sourceType == 0) src = vf_.process(src);

            // dry bus + wall tap
            float dry = src;
            wallDelay_.write(src);
            dry += float(wallGain_) * wallDelay_.read(wallTime_ * kSR);

            // air absorption
            dry = air_.process(dry);

            // binaural
            float panL = 0, panR = 0;
            panner_.process(dry, panL, panR);

            // reverb sends
            float wetL = 0, wetR = 0;
            if (p.reverbType == 0)
                hall_.process(src, src, wetL, wetR);
            else if (p.reverbType == 1)
                plate_.process(src, src, wetL, wetR);

            // plate loudness match (hand-tuned table)
            if (p.reverbType == 1) {
                const double decay = p.plateDecay;
                double e = 9.0;
                const double pts[][2] = {{0.1, 1.2}, {0.4, 1.93}, {0.62, 2.45},
                                         {0.85, 4.06}, {0.97, 9.0}};
                for (int k = 1; k < 5; ++k)
                    if (decay <= pts[k][0]) {
                        const double t =
                            (decay - pts[k - 1][0]) / (pts[k][0] - pts[k - 1][0]);
                        e = pts[k - 1][1] + t * (pts[k][1] - pts[k - 1][1]);
                        break;
                    }
                const double scale = 0.3 * std::sqrt(2.45 / e);
                wetL *= float(scale * p.mix);
                wetR *= float(scale * p.mix);
            }

            float L = panL + wetL;
            float R = panR + wetR;
            const float master = float(p.level * p.level);
            L *= master;
            R *= master;

            // soft clip / compressor-ish
            auto soft = [](float x) {
                const float th = 0.89f; // ~-1 dB
                if (std::abs(x) < th) return x;
                return std::copysign(th + (1.f - th) * std::tanh((std::abs(x) - th) / (1.f - th)), x);
            };
            L = soft(L);
            R = soft(R);

            outL[i] = L;
            outR[i] = R;
            peak = std::max(peak, std::max(std::abs(L), std::abs(R)));
        }
        peak_.store(peak);
    }
};

// ------------------------------------------------------------ derived quantities (UI)
static void updateDerived(Params& p) {
    p.lx = p.W / 2.0;
    p.ly = std::clamp(p.D * 0.18, 0.8, 3.0);
    p.lz = std::min(1.5, p.H * 0.55);
    p.sz = p.lz;
    p.sx = std::clamp(p.sx, 0.3, p.W - 0.3);
    p.sy = std::clamp(p.sy, std::max(0.3, p.ly), p.D - 0.3);

    const double V = p.W * p.D * p.H;
    const double S = 2.0 * (p.W * p.D + p.W * p.H + p.D * p.H);
    const double al = std::clamp(p.alpha, 0.02, 0.97);
    p.rt60 = std::clamp(-0.161 * V / (S * std::log(1.0 - al)), 0.15, 12.0);
    p.dc = 0.057 * std::sqrt(V / p.rt60);
}

static void printParams(const Params& p, int taps) {
    std::printf(
        "Room %.1f x %.1f x %.1f m  alpha=%.2f  V=%.0f m3  RT60=%.2f s  dc=%.2f m  taps=%d\n"
        "Source (%.2f, %.2f) m  dist=%.2f m  az=%.0f deg  couple=%d\n"
        "Theremin %.1f Hz  level=%.2f  vib=%.0f c\n"
        "Reverb %s  mix=%.2f  early=%.2f  hallDamp=%.2f\n"
        "Plate decay=%.2f  damp=%.2f  pre=%.0f ms\n"
        "Master %.2f\n",
        p.W, p.D, p.H, p.alpha, p.W * p.D * p.H, p.rt60, p.dc, taps,
        p.sx, p.sy,
        std::hypot(p.sx - p.lx, p.sy - p.ly),
        std::atan2(p.sx - p.lx, p.sy - p.ly) * 180.0 / kPi, int(p.couple),
        p.thereminHz, p.thereminLevel, p.vibratoCents,
        p.reverbType == 0 ? "hall" : p.reverbType == 1 ? "plate" : "off", p.mix,
        p.earlyLevel, p.hallDamp, p.plateDecay, p.plateDamp, p.platePredelayMs,
        p.level);
}

// ------------------------------------------------------------ audio sink (Pulse or WAV)
class AudioSink {
public:
    virtual ~AudioSink() = default;
    virtual bool start(Engine& eng) = 0;
    virtual void stop() = 0;
};

#ifdef ROOM_THEREMIN_PULSE
#include <pulse/simple.h>
#include <pulse/error.h>
class PulseSink : public AudioSink {
    pa_simple* pa_ = nullptr;
    std::thread th_;
    std::atomic<bool> run_{false};
    Engine* eng_ = nullptr;
public:
    bool start(Engine& eng) override {
        eng_ = &eng;
        pa_sample_spec ss{PA_SAMPLE_FLOAT32LE, uint32_t(kSR), 2};
        int err = 0;
        pa_ = pa_simple_new(nullptr, "RoomTheremin", PA_STREAM_PLAYBACK, nullptr,
                            "Room Theremin", &ss, nullptr, nullptr, &err);
        if (!pa_) {
            std::fprintf(stderr, "PulseAudio open failed: %s\n", pa_strerror(err));
            return false;
        }
        run_ = true;
        th_ = std::thread([this] {
            std::vector<float> buf(size_t(kBlock * 2));
            while (run_) {
                eng_->processBlock(buf.data(), buf.data() + kBlock, kBlock);
                // interleave
                std::vector<float> inter(size_t(kBlock * 2));
                for (int i = 0; i < kBlock; ++i) {
                    inter[size_t(i * 2)] = buf[size_t(i)];
                    inter[size_t(i * 2 + 1)] = buf[size_t(i + kBlock)];
                }
                int e = 0;
                if (pa_simple_write(pa_, inter.data(), inter.size() * sizeof(float), &e) < 0) {
                    std::fprintf(stderr, "pa_simple_write: %s\n", pa_strerror(e));
                    break;
                }
            }
        });
        return true;
    }
    void stop() override {
        run_ = false;
        if (th_.joinable()) th_.join();
        if (pa_) {
            pa_simple_free(pa_);
            pa_ = nullptr;
        }
    }
};
#endif

class WavSink : public AudioSink {
    std::string path_;
    double duration_;
public:
    WavSink(std::string path, double dur) : path_(std::move(path)), duration_(dur) {}
    bool start(Engine& eng) override {
        const int frames = int(duration_ * kSR);
        wav::Audio a;
        a.sampleRate = int(kSR);
        a.channels = 2;
        a.interleaved.resize(size_t(frames * 2));
        std::vector<float> L(static_cast<size_t>(kBlock));
        std::vector<float> R(static_cast<size_t>(kBlock));
        int written = 0;
        while (written < frames) {
            int n = std::min(kBlock, frames - written);
            eng.processBlock(L.data(), R.data(), n);
            for (int i = 0; i < n; ++i) {
                a.interleaved[size_t((written + i) * 2)] = L[size_t(i)];
                a.interleaved[size_t((written + i) * 2 + 1)] = R[size_t(i)];
            }
            written += n;
        }
        wav::write(path_, a);
        std::printf("Wrote %s (%.1f s, %.0f Hz stereo)\n", path_.c_str(), duration_, kSR);
        return true;
    }
    void stop() override {}
};

// ------------------------------------------------------------ main
int main(int argc, char** argv) {
    std::string wavPath, renderPath;
    double renderDur = 20.0;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--wav" && i + 1 < argc) wavPath = argv[++i];
        else if (a == "--render" && i + 1 < argc) renderPath = argv[++i];
        else if (a == "--duration" && i + 1 < argc) renderDur = std::atof(argv[++i]);
        else if (a == "--help") {
            std::puts(
                "room_theremin [--wav file] [--render out.wav] [--duration sec]");
            return 0;
        }
    }

    ParamBus bus;
    Params p;
    updateDerived(p);
    bus.write(p);

    Engine eng(bus);
    if (!wavPath.empty()) {
        try {
            eng.loadWav(wavPath);
            p.sourceType = 1;
            bus.write(p);
            std::printf("Loaded WAV: %s\n", wavPath.c_str());
        } catch (const std::exception& e) {
            std::fprintf(stderr, "WAV load failed: %s\n", e.what());
            return 1;
        }
    }

    // initial geometry push
    eng.syncParams(p);

    std::unique_ptr<AudioSink> sink;
    if (!renderPath.empty()) {
        sink = std::make_unique<WavSink>(renderPath, renderDur);
        sink->start(eng);
        return 0;
    }

#ifdef ROOM_THEREMIN_PULSE
    sink = std::make_unique<PulseSink>();
    if (!sink->start(eng)) {
        std::puts("Falling back to offline mode — type commands, then Q to quit "
                  "(no live audio).");
        sink.reset();
    } else {
        std::puts("PulseAudio running. Type commands (P to print, Q to quit).");
    }
#else
    std::puts("Built without PulseAudio. Interactive parameter changes work; "
              "audio only via --render.");
#endif

    // stdin control loop
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        if (line == "Q" || line == "q") break;
        if (line == "P" || line == "p") {
            printParams(p, eng.lastTaps());
            continue;
        }
        // parse KEY=value pairs
        std::istringstream iss(line);
        std::string tok;
        bool changed = false;
        while (iss >> tok) {
            auto eq = tok.find('=');
            if (eq == std::string::npos) continue;
            std::string key = tok.substr(0, eq);
            std::string val = tok.substr(eq + 1);
            double v = std::atof(val.c_str());
            if (key == "W") {
                p.W = std::clamp(v, 2.5, 40.0);
                changed = true;
            } else if (key == "D") {
                p.D = std::clamp(v, 3.0, 60.0);
                changed = true;
            } else if (key == "H") {
                p.H = std::clamp(v, 2.2, 25.0);
                changed = true;
            } else if (key == "A") {
                p.alpha = std::clamp(v, 0.03, 0.9);
                changed = true;
            } else if (key == "X") {
                p.sx = v;
                changed = true;
            } else if (key == "Y") {
                p.sy = v;
                changed = true;
            } else if (key == "T") {
                p.thereminHz = v;
                changed = true;
            } else if (key == "V") {
                p.thereminLevel = std::clamp(v, 0.0, 1.0);
                changed = true;
            } else if (key == "R") {
                if (val == "hall") p.reverbType = 0;
                else if (val == "plate") p.reverbType = 1;
                else p.reverbType = 2;
                changed = true;
            } else if (key == "M") {
                p.mix = std::clamp(v, 0.0, 2.0);
                changed = true;
            } else if (key == "E") {
                p.earlyLevel = std::clamp(v, 0.0, 2.0);
                changed = true;
            } else if (key == "DM") {
                p.hallDamp = std::clamp(v, 0.0, 0.9);
                changed = true;
            } else if (key == "PD") {
                p.plateDecay = std::clamp(v, 0.1, 0.95);
                changed = true;
            } else if (key == "PDM") {
                p.plateDamp = std::clamp(v, 0.0, 0.95);
                changed = true;
            } else if (key == "PP") {
                p.platePredelayMs = std::clamp(v, 0.0, 120.0);
                changed = true;
            } else if (key == "L") {
                p.level = std::clamp(v, 0.0, 1.0);
                changed = true;
            } else if (key == "C") {
                p.couple = (v != 0.0);
                changed = true;
            }
        }
        if (changed) {
            updateDerived(p);
            bus.write(p);
            eng.syncParams(p); // heavy work on UI thread
            printParams(p, eng.lastTaps());
        }
    }

    if (sink) sink->stop();
    return 0;
}
