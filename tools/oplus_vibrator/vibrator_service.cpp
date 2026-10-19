// vendor.oplus.hardware.vibrator-service for the POCO F7 (onyx): OnePlus' vibrator service contract on
// Xiaomi's *stock* vendor.xiaomi.hardware.vibratorfeature HAL. Nothing is patched.
//
// OnePlus' framework needs two things from android.hardware.vibrator.IVibrator/default:
//   * the OnePlus calling convention: setAmplitude(1 + strength/2400) before perform(<OnePlus waveform id>),
//     setAmplitude(10000|10001) for the touch style (soft plays <id>+1000 for ids with a soft variant)
//   * vendor.oplus.hardware.oplusvibrator.IOplusVibrator (V2) as the binder extension: AAC RichTap HE
// Xiaomi's HAL has neither, but its own API covers both: IVibratorExt.setAmplitudeExt(g, 1) is the one-shot
// effect strength the next perform() uses, and IVibratorExt.play() renders HE events with AAC's HED engine
// (libaachaptics). This service registers IVibrator/default like OnePlus' vendor.oplus.vibrator does (same
// VINTF entry: android.hardware.vibrator V2), translates the OnePlus calls, forwards everything else
// unchanged, and keeps Xiaomi's HAL as its private backend.
//
// Xiaomi's HAL registers IVibrator/default itself (hard-coded), so this service waits for it, takes its
// binder, then registers in its place; if the Xiaomi HAL restarts, it re-attaches and registers again.
// All work inside Xiaomi's HAL happens on Xiaomi's binder threads (its ExtPlay/perform/off take no locks).

#include <android/binder_ibinder.h>
#include <android/binder_parcel.h>
#include <android/binder_status.h>
#include <android/log.h>
#include <dlfcn.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#define TAG "vendor.oplus.vibrator"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)

namespace {

// ---- interfaces --------------------------------------------------------------------------------------
constexpr char kVibName[] = "android.hardware.vibrator.IVibrator/default";
constexpr char kVibDescriptor[] = "android.hardware.vibrator.IVibrator";
constexpr char kVibHash[] = "ea8742d6993e1a82917da38b9938e537aa7fcb54";       // android.hardware.vibrator V2
constexpr int32_t kVibVersion = 2;
constexpr char kXmExtDescriptor[] = "vendor.hardware.vibratorfeature.IVibratorExt";
constexpr char kOplusDescriptor[] = "vendor.oplus.hardware.oplusvibrator.IOplusVibrator";
constexpr char kOplusHash[] = "b103e6d812aa472b9c5b7deefd7986fa729ce1a8";     // IOplusVibrator V2
constexpr int32_t kOplusVersion = 2;
constexpr char kCbDescriptor[] = "vendor.aac.hardware.richtap.vibrator.IRichtapCallback";
// VibratorFeatureLoader only adds features the XML lacks; report what this service provides.
constexpr char kFeatures[] = "vibrator_lmvibrator=1\nvibrator_qcom_lmvibrator=1\nvibrator_richctap=1\n";
// Xiaomi's HAL normally registers well under a second after class hal starts; system_server's watchdog allows 60 s.
constexpr char kXiaomiHalPath[] = "/odm/bin/hw/vendor.xiaomi.hardware.vibratorfeature.service";
constexpr int kXiaomiWaitMs = 8000;

constexpr transaction_code_t GET_INTERFACE_HASH = 16777214, GET_INTERFACE_VERSION = 16777215;
// android.hardware.vibrator.IVibrator
constexpr transaction_code_t VIB_OFF = 2, VIB_ON = 3, VIB_PERFORM = 4, VIB_SET_AMPLITUDE = 6;
// vendor.hardware.vibratorfeature.IVibratorExt
constexpr transaction_code_t XM_PLAY = 1, XM_SET_AMPLITUDE_EXT = 5;
// vendor.oplus.hardware.oplusvibrator.IOplusVibrator
enum : transaction_code_t {
    INIT = 1, SET_DYNAMIC_SCALE, SET_F0, STOP, SET_AMPLITUDE, PERFORM_HE_PARAM, OFF, ON, PERFORM,
    PERFORM_ENVELOPE, PERFORM_RTP, PERFORM_HE, SET_HAPTIC_PARAM, LM_OFF, LM_ON, START_CALIBRATE,
    GET_CALIBRATE_RESULTS, GET_STATUS, SET_VMAX, SET_MOTOR_OLD_TEST, GET_SUPPORT_FEATURES, WRITE_GUN_TYPE,
    WRITE_GUN_MODE, WRITE_BULLET_NUM, WRITE_HAPTIC_AUDIO, SET_TOUCH_STYLE, GET_TOUCH_STYLE, SET_POWER_ON_SWITCH,
};
constexpr transaction_code_t CB_ON_CALLBACK = 1;   // IRichtapCallback.onCallback(int) - oneway

// OnePlus 13 vibrator_effect.json "soft_style" ids; the soft touch style plays <id + 1000>
constexpr int kSoftIds[] = {0, 1, 2, 3, 4, 6, 7, 46, 47, 54, 105, 106, 107, 109, 309, 310, 315, 316, 318, 362, 363, 364};
constexpr int kSoftOffset = 1000;

// ---- libbinder_ndk vendor APIs (not in the public NDK headers) -------------------------------------
using OnRegister = void (*)(const char* instance, AIBinder* registered, void* cookie);
struct Vndk {
    binder_exception_t (*addService)(AIBinder*, const char*);
    void* (*registerForServiceNotifications)(const char*, OnRegister, void*);
    bool (*setThreadPoolMaxThreadCount)(uint32_t);
    void (*startThreadPool)();
    void (*joinThreadPool)();
    void (*markVintfStability)(AIBinder*);
} gV;

template <typename T>
bool resolve(void* lib, const char* name, T& out) {
    out = reinterpret_cast<T>(dlsym(lib, name));
    if (out == nullptr) LOGE("missing %s in libbinder_ndk", name);
    return out != nullptr;
}

bool loadVndk() {
    void* lib = dlopen("libbinder_ndk.so", RTLD_NOW);
    if (lib == nullptr) return false;
    return resolve(lib, "AServiceManager_addService", gV.addService) &&
           resolve(lib, "AServiceManager_registerForServiceNotifications", gV.registerForServiceNotifications) &&
           resolve(lib, "ABinderProcess_setThreadPoolMaxThreadCount", gV.setThreadPoolMaxThreadCount) &&
           resolve(lib, "ABinderProcess_startThreadPool", gV.startThreadPool) &&
           resolve(lib, "ABinderProcess_joinThreadPool", gV.joinThreadPool) &&
           resolve(lib, "AIBinder_markVintfStability", gV.markVintfStability);
}

int64_t nowMs() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

AIBinder_Class* gVibClass = nullptr;    // serves IVibrator/default, and talks to Xiaomi's IVibrator
AIBinder_Class* gXmExtClass = nullptr;  // client of Xiaomi's IVibratorExt
AIBinder_Class* gOplusClass = nullptr;  // serves IOplusVibrator (our extension)
AIBinder_Class* gCbClass = nullptr;     // client of IRichtapCallback
AIBinder* gSelf = nullptr;              // our IVibrator

// ---- Xiaomi backend (replaced when the Xiaomi HAL restarts) ------------------------------------------
class Backend {
public:
    struct Ref {   // strong references for the duration of one call
        AIBinder* vib = nullptr;
        AIBinder* ext = nullptr;
        ~Ref() {
            if (vib) AIBinder_decStrong(vib);
            if (ext) AIBinder_decStrong(ext);
        }
        explicit operator bool() const { return vib != nullptr; }
    };

    void set(AIBinder* vib, AIBinder* ext) {   // takes ownership
        std::lock_guard<std::mutex> g(mu_);
        if (vib_) AIBinder_decStrong(vib_);
        if (ext_) AIBinder_decStrong(ext_);
        vib_ = vib;
        ext_ = ext;
        if (vib) everAttached_ = true;
        cv_.notify_all();
    }
    void clear() { set(nullptr, nullptr); }

    // IVibrator calls wait briefly while the Xiaomi HAL restarts, so the framework doesn't see failures in that
    // window; one-way RichTap calls don't wait (they would only pile up on the binder threads). Nothing waits if
    // Xiaomi's HAL never came up at all (see main): there is no restart to wait for, only a boot to keep fast.
    void get(Ref& r, int waitMs = 3000) {
        std::unique_lock<std::mutex> l(mu_);
        if (everAttached_) cv_.wait_for(l, std::chrono::milliseconds(waitMs), [this] { return vib_ != nullptr; });
        if (vib_) AIBinder_incStrong(r.vib = vib_);
        if (ext_) AIBinder_incStrong(r.ext = ext_);
    }

    bool waitFirstAttach(int ms) {
        std::unique_lock<std::mutex> l(mu_);
        return cv_.wait_for(l, std::chrono::milliseconds(ms), [this] { return everAttached_; });
    }
    bool everAttached() {
        std::lock_guard<std::mutex> g(mu_);
        return everAttached_;
    }

private:
    std::mutex mu_;
    std::condition_variable cv_;
    AIBinder* vib_ = nullptr;
    AIBinder* ext_ = nullptr;
    bool everAttached_ = false;
};
Backend gBackend;

binder_status_t ok(AParcel* out) {
    AStatus* st = AStatus_newOk();
    binder_status_t r = AParcel_writeStatusHeader(out, st);
    AStatus_delete(st);
    return r;
}

binder_status_t exception(AParcel* out, binder_exception_t ex) {
    AStatus* st = AStatus_fromExceptionCode(ex);
    binder_status_t r = AParcel_writeStatusHeader(out, st);
    AStatus_delete(st);
    return r;
}

// Sends `in` (already written) to `to`; copies the reply into `out` if given.
binder_status_t call(AIBinder* to, transaction_code_t code, AParcel** in, AParcel* out, binder_flags_t flags = 0) {
    AParcel* reply = nullptr;
    binder_status_t st = AIBinder_transact(to, code, in, &reply, flags);
    if (st == STATUS_OK && out != nullptr && reply != nullptr)
        st = AParcel_appendFrom(reply, out, 0, AParcel_getDataSize(reply));
    AParcel_delete(reply);
    return st;
}

// Forwards an IVibrator transaction to Xiaomi's HAL byte for byte (callbacks included).
binder_status_t forward(transaction_code_t code, const AParcel* in, AParcel* out) {
    Backend::Ref b;
    gBackend.get(b);
    if (!b) return exception(out, EX_ILLEGAL_STATE);
    AParcel* xin = nullptr;
    binder_status_t st = AIBinder_prepareTransaction(b.vib, &xin);
    if (st != STATUS_OK) return st;
    int32_t pos = AParcel_getDataPosition(in);
    AParcel_appendFrom(in, xin, pos, AParcel_getDataSize(in) - pos);
    return call(b.vib, code, &xin, out);
}

bool xmSetAmplitudeExt(AIBinder* ext, float g, int32_t type) {
    AParcel* in = nullptr;
    if (ext == nullptr || AIBinder_prepareTransaction(ext, &in) != STATUS_OK) return false;
    AParcel_writeFloat(in, g);
    AParcel_writeInt32(in, type);
    return call(ext, XM_SET_AMPLITUDE_EXT, &in, nullptr) == STATUS_OK;
}

// ---- OnePlus calling convention ------------------------------------------------------------------------
std::mutex gStyleLock;
float gPendingGain = -1.f;   // from setAmplitude(1 + g), consumed by the next perform()
bool gSoftStyle = false;

bool isSoftId(int id) {
    return std::find(std::begin(kSoftIds), std::end(kSoftIds), id) != std::end(kSoftIds);
}

void yieldHe();   // RichTap section

binder_status_t vibSetAmplitude(const AParcel* in, AParcel* out) {
    float a = 0;
    int32_t pos = AParcel_getDataPosition(in);
    AParcel_readFloat(in, &a);
    if (a <= 1.0f) {   // a plain amplitude for on()/waveforms: Xiaomi handles it
        AParcel_setDataPosition(in, pos);
        return forward(VIB_SET_AMPLITUDE, in, out);
    }
    std::lock_guard<std::mutex> g(gStyleLock);
    if (a == 10000.f || a == 10001.f) {
        gSoftStyle = a == 10001.f;               // touch style: default / soft
    } else if (a <= 2.0f) {
        gPendingGain = a - 1.0f;                 // per-effect gain: strength / 2400
    } else {
        gPendingGain = 1.0f;                     // COUI widgets can ask for more than 2400
    }
    return ok(out);
}

binder_status_t vibPerform(const AParcel* in, AParcel* out) {
    int32_t effect = 0;
    int8_t strength = 0;
    AIBinder* cb = nullptr;
    AParcel_readInt32(in, &effect);
    AParcel_readByte(in, &strength);
    AParcel_readStrongBinder(in, &cb);
    float gain;
    bool soft;
    {
        std::lock_guard<std::mutex> g(gStyleLock);
        gain = gPendingGain;
        soft = gSoftStyle;
        gPendingGain = -1.f;
    }
    if (soft && isSoftId(effect)) effect += kSoftOffset;
    yieldHe();
    Backend::Ref b;
    gBackend.get(b);
    binder_status_t st;
    if (!b) {
        st = exception(out, EX_ILLEGAL_STATE);
    } else {
        if (gain >= 0.f) xmSetAmplitudeExt(b.ext, gain, 1);   // Xiaomi's one-shot effect strength
        AParcel* xin = nullptr;
        st = AIBinder_prepareTransaction(b.vib, &xin);
        if (st == STATUS_OK) {
            AParcel_writeInt32(xin, effect);
            AParcel_writeByte(xin, strength);
            AParcel_writeStrongBinder(xin, cb);
            st = call(b.vib, VIB_PERFORM, &xin, out);
        }
    }
    if (cb) AIBinder_decStrong(cb);
    return st;
}

binder_status_t onVibTransact(AIBinder*, transaction_code_t code, const AParcel* in, AParcel* out) {
    switch (code) {
        case VIB_SET_AMPLITUDE:
            return vibSetAmplitude(in, out);
        case VIB_PERFORM:
            return vibPerform(in, out);
        case VIB_ON:
        case VIB_OFF:   // Xiaomi's off() would block until a running HE stream drained
            yieldHe();
            return forward(code, in, out);
        case GET_INTERFACE_VERSION:
            ok(out);
            return AParcel_writeInt32(out, kVibVersion);
        case GET_INTERFACE_HASH:
            ok(out);
            return AParcel_writeString(out, kVibHash, static_cast<int32_t>(strlen(kVibHash)));
        default:
            return forward(code, in, out);
    }
}

// ---- RichTap: completion callbacks ---------------------------------------------------------------------
void richtapCallback(AIBinder* cb) {
    if (!AIBinder_associateClass(cb, gCbClass)) {
        LOGW("callback binder is not %s", kCbDescriptor);
        return;
    }
    AParcel* in = nullptr;
    if (AIBinder_prepareTransaction(cb, &in) != STATUS_OK) return;
    AParcel_writeInt32(in, 0);
    call(cb, CB_ON_CALLBACK, &in, nullptr, FLAG_ONEWAY);
}

class Timer {
public:
    void at(int64_t when, AIBinder* cb) {
        if (cb == nullptr) return;
        AIBinder_incStrong(cb);
        std::lock_guard<std::mutex> g(mu_);
        if (!started_) {
            std::thread([this] { run(); }).detach();
            started_ = true;
        }
        q_.emplace(when, cb);
        cv_.notify_one();
    }
    void flush() {   // stop(): release everyone waiting for a completion now
        std::lock_guard<std::mutex> g(mu_);
        std::multimap<int64_t, AIBinder*> now;
        for (auto& e : q_) now.emplace(0, e.second);
        q_.swap(now);
        cv_.notify_one();
    }

private:
    void run() {
        std::unique_lock<std::mutex> l(mu_);
        for (;;) {
            if (q_.empty()) {
                cv_.wait(l);
                continue;
            }
            int64_t wait = q_.begin()->first - nowMs();
            if (wait > 0) {
                cv_.wait_for(l, std::chrono::milliseconds(wait));
                continue;
            }
            AIBinder* cb = q_.begin()->second;
            q_.erase(q_.begin());
            l.unlock();
            richtapCallback(cb);
            AIBinder_decStrong(cb);
            l.lock();
        }
    }
    std::mutex mu_;
    std::condition_variable cv_;
    std::multimap<int64_t, AIBinder*> q_;
    bool started_ = false;
};
Timer gTimer;

// ---- RichTap: HE -> Xiaomi IVibratorExt.play ------------------------------------------------------------
constexpr int kContinuous = 4096, kTransient = 4097;
constexpr int kMaxEvents = 16;      // Vibrator::ExtPlay: "The num of he event ... is limited to 16"
constexpr int kHedPoints = 4;       // AAC HED keeps exactly 4 points per continuous event
constexpr int kMaxEventMs = 5000;
constexpr int kTransientMs = 48;
constexpr int kPrebakedMs = 30;
constexpr int kStreamSlackMs = 1500;   // Xiaomi keeps streaming after the nominal end of a pattern

struct Ev {
    bool cont = false;
    int t = 0, inten = 0, freq = 50, dur = 0;
    std::vector<int> pt, pi, pf;   // curve: time (rel. to event start), intensity 0..100, freq -100..100
    int end() const { return t + (cont ? dur : kTransientMs); }
};

std::mutex gLock;
int gAmp = 255, gInterval = 0, gFreq = 0;
int64_t gBusyUntil = 0;
int gPid = -1, gSeq = -1, gTotal = 0, gGot = 0;   // HE 2.0 session (packets share pid/seq)
int64_t gStart = 0;

bool xmPlay(const std::vector<int32_t>& d, int loop, int64_t interval) {
    Backend::Ref b;
    gBackend.get(b, 0);
    AParcel* in = nullptr;
    if (!b.ext || AIBinder_prepareTransaction(b.ext, &in) != STATUS_OK) return false;
    AParcel_writeInt32Array(in, d.data(), static_cast<int32_t>(d.size()));
    AParcel_writeInt32(in, loop);
    AParcel_writeInt64(in, interval);
    AParcel_writeInt32(in, 255);
    AParcel* reply = nullptr;
    bool good = false;
    if (AIBinder_transact(b.ext, XM_PLAY, &in, &reply, 0) == STATUS_OK) {
        AStatus* st = nullptr;
        good = AParcel_readStatusHeader(reply, &st) == STATUS_OK && AStatus_isOk(st);
        if (!good) LOGW("IVibratorExt.play rejected %zu ints", d.size());
        AStatus_delete(st);
    }
    AParcel_delete(reply);
    return good;
}

// Stops a running HE stream with Xiaomi's own "hboost quick off" (setAmplitudeExt(_, 2): pauses the
// DynamicEffectThread and stops the FF device). IVibrator.off() is no use here: FFDevice::off() writes an
// end-of-stream event that blocks until the driver has played everything already streamed, so it - and every
// effect queued behind it - waits for the HE pattern to finish.
void xmQuickOff() {
    Backend::Ref b;
    gBackend.get(b, 0);
    if (b.ext) xmSetAmplitudeExt(b.ext, 0.f, 2);
}

void readPoints(Ev& e, const std::vector<int32_t>& he, size_t o, int n) {
    for (int i = 0; i < n; i++) {
        e.pt.push_back(clampi(he[o + 3 * i], 0, e.dur));
        e.pi.push_back(clampi(he[o + 3 * i + 1], 0, 100));
        e.pf.push_back(clampi(he[o + 3 * i + 2], -100, 100));
    }
}

// HE 1.0: up to 16 fixed 55-int blocks (HapticPlayer.getSerializationDataHe_1_0)
void parseHe1(const std::vector<int32_t>& he, std::vector<Ev>& out) {
    for (size_t b = 0; b + 7 <= he.size(); b += 55) {
        int type = he[b];
        if (type != kContinuous && type != kTransient) break;
        Ev e;
        e.t = std::max(0, he[b + 1]);
        e.inten = clampi(he[b + 2], 0, 100);
        e.freq = clampi(he[b + 3], 0, 100);
        if (type == kContinuous) {
            e.cont = true;
            e.dur = clampi(he[b + 4], 0, kMaxEventMs);
            int n = std::min(clampi(he[b + 6], 0, 16), static_cast<int>((he.size() - b - 7) / 3));
            readPoints(e, he, b + 7, n);
        }
        out.push_back(e);
    }
}

// HE 2.0 packet: [2, heVersion, pid, seq, total | n << 16, {index, absTime, nEvents, events...}...]
//   transient  [4097, 5, vibId, relTime, intensity, freq, duration]
//   continuous [4096, 3n+6, vibId, relTime, intensity, freq, duration, n, (time, intensity, freq) x n]
void parseHe2(const std::vector<int32_t>& he, std::vector<Ev>& out) {
    int patterns = static_cast<uint32_t>(he[4]) >> 16;
    size_t o = 5;
    for (int p = 0; p < patterns && o + 3 <= he.size(); p++) {
        int abs = he[o + 1], nEv = he[o + 2];
        o += 3;
        for (int k = 0; k < nEv && o + 7 <= he.size(); k++) {
            int type = he[o], len = he[o + 1];
            Ev e;
            e.t = std::max(0, abs + he[o + 3]);
            e.inten = clampi(he[o + 4], 0, 100);
            e.freq = clampi(he[o + 5], 0, 100);
            if (type == kContinuous && o + 8 <= he.size()) {
                e.cont = true;
                e.dur = clampi(he[o + 6], 0, kMaxEventMs);
                int n = std::min(clampi(he[o + 7], 0, 16), static_cast<int>((he.size() - o - 8) / 3));
                readPoints(e, he, o + 8, n);
            }
            if (type == kContinuous || type == kTransient) out.push_back(e);
            o += std::max(5, len) + 2;
        }
    }
}

// RichTap curves have 2..16 points, AAC HED exactly 4: short curves get points inserted on their
// longest segments (shape unchanged), longer ones are cut into consecutive events of 3 segments each,
// so every original point - and so the envelope and its timing - survives.
void fitCurve(const Ev& e, std::vector<Ev>& out) {
    size_t n = e.pt.size();
    if (n < 2) {   // no curve: flat at the event intensity
        Ev f = e;
        f.pt = {0, e.dur / 3, 2 * e.dur / 3, e.dur};
        f.pi = {100, 100, 100, 100};
        f.pf = {0, 0, 0, 0};
        out.push_back(f);
        return;
    }
    for (size_t s = 0; s + 1 < n; s += kHedPoints - 1) {
        size_t k = std::min(n - 1, s + kHedPoints - 1);
        int dur = e.pt[k] - e.pt[s];
        if (dur <= 0) continue;
        std::vector<std::array<int, 3>> p;
        for (size_t i = s; i <= k; i++) p.push_back({e.pt[i] - e.pt[s], e.pi[i], e.pf[i]});
        while (p.size() < static_cast<size_t>(kHedPoints)) {   // halve the longest segment
            size_t best = 0;
            for (size_t i = 1; i + 1 < p.size(); i++)
                if (p[i + 1][0] - p[i][0] > p[best + 1][0] - p[best][0]) best = i;
            const auto a = p[best], c = p[best + 1];
            p.insert(p.begin() + best + 1, {(a[0] + c[0]) / 2, (a[1] + c[1]) / 2, (a[2] + c[2]) / 2});
        }
        Ev c;
        c.cont = true;
        c.t = e.t + e.pt[s];
        c.inten = e.inten;
        c.freq = e.freq;
        c.dur = dur;
        for (const auto& q : p) {
            c.pt.push_back(q[0]);
            c.pi.push_back(q[1]);
            c.pf.push_back(q[2]);
        }
        out.push_back(c);
    }
}

std::vector<Ev> fitAll(const std::vector<Ev>& in) {
    std::vector<Ev> out;
    for (const Ev& e : in) {
        if (!e.cont) out.push_back(e);
        else if (e.dur > 0) fitCurve(e, out);
    }
    std::stable_sort(out.begin(), out.end(), [](const Ev& a, const Ev& b) { return a.t < b.t; });
    return out;
}

int patternEnd(const std::vector<Ev>& evs) {
    int end = 0;
    for (const Ev& e : evs) end = std::max(end, e.end());
    return end;
}

// RichTap event intensity (0..100, as the app/COUI computed it) -> intensity handed to AAC's HED renderer.
// The identity is OnePlus parity: their AAC HAL receives the same numbers. The launcher's A-Z index sends
// 50..100 for a transient depending on drag speed; continuous events scale their curve by this value.
int heIntensity(int v) {
    return v;   // unchanged: Xiaomi's HED renderer receives exactly what OnePlus' AAC HAL would
}

// IVibratorExt.play data:
//  [len, 1, globalIntensity, globalSharpness, 0 (no ParameterData), nEvents, events...]
//    transient   [0, relTime, intensity, freq, 0, 0]
//    continuous  [1, relTime, intensity, freq, duration, 2,
//                 0, 0, 4, (time, intensity) x4,   1, 0, 4, (time, freq) x4]
std::vector<int32_t> encode(const Ev* ev, size_t n, int shift, int amp, int freq) {
    std::vector<int32_t> d = {0, 1, clampi((amp * 100 + 127) / 255, 0, 100),
                              (freq > 0 && freq <= 100) ? freq : 50, 0, static_cast<int32_t>(n)};
    for (size_t i = 0; i < n; i++) {
        const Ev& e = ev[i];
        int rel = std::max(0, e.t - shift);
        d.insert(d.end(), {e.cont ? 1 : 0, rel, clampi(heIntensity(e.inten), 0, 100), e.freq,
                           e.cont ? e.dur : 0, e.cont ? 2 : 0});
        if (!e.cont) continue;
        for (int c = 0; c < 2; c++) {
            d.insert(d.end(), {c, 0, kHedPoints});
            for (int j = 0; j < kHedPoints; j++) d.insert(d.end(), {e.pt[j], c == 0 ? e.pi[j] : e.pf[j]});
        }
    }
    d[0] = static_cast<int32_t>(d.size());
    return d;
}

// Plays a timeline whose t=0 was `shift` ms ago. With loops > 1 every chunk loops natively in the HAL with
// the same period: its HED spans [0, chunkEnd], so interval = patternEnd + interval - chunkEnd.
int64_t playTimeline(const std::vector<Ev>& evs, int shift, int loops, int interval, int amp, int freq) {
    if (evs.empty() || amp <= 0) return 0;
    int end = patternEnd(evs);
    for (size_t i = 0; i < evs.size(); i += kMaxEvents) {
        size_t n = std::min(evs.size() - i, static_cast<size_t>(kMaxEvents));
        int chunkEnd = 0;
        for (size_t j = i; j < i + n; j++) chunkEnd = std::max(chunkEnd, evs[j].end());
        if (chunkEnd <= shift) continue;   // a late HE 2.0 packet whose events are already over
        int gap = loops > 1 ? end + interval - chunkEnd : 0;
        xmPlay(encode(&evs[i], n, shift, amp, freq), loops, gap);
    }
    if (loops == INT_MAX) return INT64_MAX / 4;
    return static_cast<int64_t>(loops) * (end - shift) + static_cast<int64_t>(loops - 1) * interval;
}

// True while an HE stream may still be playing (gLock held). Xiaomi renders ahead and streams past the
// nominal pattern length, so the window is generous; a quick off when nothing plays costs nothing.
bool heActiveLocked(int64_t now) { return gBusyUntil > 0 && now < gBusyUntil; }

void busy(int64_t start, int64_t len) {
    std::lock_guard<std::mutex> g(gLock);
    int64_t until = len >= INT64_MAX / 8 ? INT64_MAX / 4 : start + 2 * len + kStreamSlackMs;
    gBusyUntil = std::max(gBusyUntil, until);
}

// Ends a running HE pattern; returns whether one was running.
bool endHe() {
    bool active;
    {
        std::lock_guard<std::mutex> g(gLock);
        active = heActiveLocked(nowMs());
        gBusyUntil = 0;
        gPid = gSeq = -1;
    }
    if (active) xmQuickOff();
    gTimer.flush();
    return active;
}

void stopAll() { endHe(); }

// A regular effect (and the framework's off()) supersedes a running HE pattern, as on OnePlus. Otherwise
// Xiaomi queues it behind the HE stream and the call - and the framework's vibration thread - blocks for
// seconds.
void yieldHe() { endHe(); }

void performHe(int looper, int interval, int amplitude, int freq, const std::vector<int32_t>& he, AIBinder* cb) {
    // HapticPlayer.getRealLooper(): -1 loops until stop(), 0 plays once
    int loops = looper == -1 ? INT_MAX : (looper < 0 ? 0 : std::max(1, looper));
    int64_t now = nowMs();
    if (he.empty() || loops == 0) {
        gTimer.at(now, cb);
        return;
    }
    int amp, ival, fr, shift = 0;
    bool multi = false, replaces = true;   // a new pattern replaces a running one; a 2nd HE 2.0 packet extends it
    {
        std::lock_guard<std::mutex> g(gLock);
        gAmp = amp = clampi(amplitude, 0, 255);
        gInterval = ival = std::max(0, interval);
        gFreq = fr = freq;
        if (he[0] == 2 && he.size() >= 5) {
            int pid = he[2], seq = he[3], total = he[4] & 0xffff;
            if (pid != gPid || seq != gSeq || gGot >= gTotal) {
                gPid = pid;
                gSeq = seq;
                gTotal = std::max(1, total);
                gGot = 0;
                gStart = now;
            } else {
                replaces = false;
            }
            gGot += std::max(1, static_cast<int>(static_cast<uint32_t>(he[4]) >> 16));
            shift = static_cast<int>(now - gStart);
            multi = gTotal > 1;
        }
    }
    std::vector<Ev> raw;
    if (he[0] == 2) parseHe2(he, raw);
    else parseHe1(he, raw);
    std::vector<Ev> evs = fitAll(raw);
    if (multi && loops > 1) {
        LOGW("multi-packet HE 2.0 with %d loops: playing once", loops);
        loops = 1;
    }
    if (replaces) {   // Xiaomi would block this play() until the previous stream drained
        bool active;
        {
            std::lock_guard<std::mutex> g(gLock);
            active = heActiveLocked(now);
        }
        if (active) xmQuickOff();
    }
    int64_t len = playTimeline(evs, shift, loops, ival, amp, fr);
    busy(now, len);
    if (loops != INT_MAX) gTimer.at(now + len, cb);
    LOGD("he%d: %zu events -> %zu, loops %d, %lld ms", he[0] == 2 ? 2 : 1, raw.size(), evs.size(), loops,
         static_cast<long long>(len));
}

void playOnce(std::vector<Ev> evs, AIBinder* cb) {
    int amp, fr;
    {
        std::lock_guard<std::mutex> g(gLock);
        amp = gAmp;
        fr = gFreq;
    }
    int64_t now = nowMs();
    int64_t len = playTimeline(fitAll(evs), 0, 1, 0, amp, fr);
    busy(now, len);
    gTimer.at(now + len, cb);
}

// ---- IOplusVibrator --------------------------------------------------------------------------------------
bool allocInts(void* data, int32_t length, int32_t** buffer) {
    auto* v = static_cast<std::vector<int32_t>*>(data);
    if (length < 0) {
        v->clear();
        *buffer = nullptr;
        return true;
    }
    v->resize(length);
    *buffer = v->data();
    return true;
}

AIBinder* readCb(const AParcel* in) {
    AIBinder* cb = nullptr;
    AParcel_readStrongBinder(in, &cb);
    return cb;   // owned: release() when done
}

void release(AIBinder* cb) {
    if (cb) AIBinder_decStrong(cb);
}

binder_status_t onOplusTransact(AIBinder*, transaction_code_t code, const AParcel* in, AParcel* out) {
    int32_t a = 0, b = 0, c = 0, d = 0;
    std::vector<int32_t> arr;
    AIBinder* cb = nullptr;
    switch (code) {
        case PERFORM_HE:   // oneway
            AParcel_readInt32(in, &a);
            AParcel_readInt32(in, &b);
            AParcel_readInt32(in, &c);
            AParcel_readInt32(in, &d);
            AParcel_readInt32Array(in, &arr, allocInts);
            cb = readCb(in);
            performHe(a, b, c, d, arr, cb);
            release(cb);
            return STATUS_OK;
        case STOP:
            cb = readCb(in);
            stopAll();
            gTimer.at(nowMs(), cb);
            release(cb);
            return ok(out);
        case OFF:          // oneway
            cb = readCb(in);
            stopAll();
            gTimer.at(nowMs(), cb);
            release(cb);
            return STATUS_OK;
        case PERFORM_HE_PARAM:
            AParcel_readInt32(in, &a);
            AParcel_readInt32(in, &b);
            AParcel_readInt32(in, &c);
            cb = readCb(in);
            if (a == 0 && b == 0 && c == 0) {
                stopAll();   // HapticPlayer.stop() sends (0, 0, 0)
            } else {
                std::lock_guard<std::mutex> g(gLock);
                gInterval = std::max(0, a);
                gAmp = clampi(b, 0, 255);
                gFreq = c;
            }
            gTimer.at(nowMs(), cb);
            release(cb);
            return ok(out);
        case SET_AMPLITUDE: {   // oneway
            AParcel_readInt32(in, &a);
            cb = readCb(in);
            {
                std::lock_guard<std::mutex> g(gLock);
                gAmp = clampi(a, 0, 255);
            }
            gTimer.at(nowMs(), cb);
            release(cb);
            return STATUS_OK;
        }
        case ON: {   // oneway: a plain buzz of timeoutMs
            AParcel_readInt32(in, &a);
            cb = readCb(in);
            std::vector<Ev> evs;
            for (int t = 0; t < a; t += kMaxEventMs) {
                Ev e;
                e.cont = true;
                e.t = t;
                e.inten = 100;
                e.dur = std::min(kMaxEventMs, a - t);
                evs.push_back(e);   // no curve: fitCurve makes it flat
            }
            playOnce(evs, cb);
            release(cb);
            return STATUS_OK;
        }
        case PERFORM: {   // RichTap ext-prebaked: no AAC effect library here -> a transient at that strength
            int8_t s = 0;
            AParcel_readInt32(in, &a);
            AParcel_readByte(in, &s);
            cb = readCb(in);
            int st = static_cast<uint8_t>(s);
            Ev e;
            e.inten = st > 100 ? (st * 100 + 127) / 255 : st;
            playOnce({e}, cb);
            release(cb);
            ok(out);
            return AParcel_writeInt32(out, kPrebakedMs);
        }
        case PERFORM_ENVELOPE: {   // oneway: 4 x (relativeTime, scale, freq) = one HED continuous event
            bool steep = false;
            AParcel_readInt32Array(in, &arr, allocInts);
            AParcel_readBool(in, &steep);
            cb = readCb(in);
            if (arr.size() >= 12 && arr[9] - arr[0] > 0) {
                Ev e;
                e.cont = true;
                e.inten = 100;
                e.freq = clampi(arr[2], 0, 100);
                e.dur = arr[9] - arr[0];
                for (int i = 0; i < 4; i++) {
                    e.pt.push_back(clampi(arr[3 * i] - arr[0], 0, e.dur));
                    e.pi.push_back(clampi(arr[3 * i + 1], 0, 100));
                    e.pf.push_back(clampi(arr[3 * i + 2] - e.freq, -100, 100));
                }
                playOnce({e}, cb);
            } else {
                gTimer.at(nowMs(), cb);
            }
            release(cb);
            return STATUS_OK;
        }
        case INIT:
        case SET_DYNAMIC_SCALE:
        case SET_F0:
        case SET_HAPTIC_PARAM:
        case PERFORM_RTP:   // oneway, nothing to do on this hardware
            return STATUS_OK;
        case GET_SUPPORT_FEATURES:
            ok(out);
            return AParcel_writeByteArray(out, reinterpret_cast<const int8_t*>(kFeatures),
                                          static_cast<int32_t>(strlen(kFeatures)));
        case GET_CALIBRATE_RESULTS: {
            int32_t none = 0;
            ok(out);
            return AParcel_writeInt32Array(out, &none, 0);
        }
        case GET_STATUS:
            ok(out);
            return AParcel_writeInt32(out, 0);
        case GET_TOUCH_STYLE: {
            std::lock_guard<std::mutex> g(gStyleLock);
            ok(out);
            return AParcel_writeInt32(out, gSoftStyle ? 1 : 0);
        }
        case SET_TOUCH_STYLE: {
            AParcel_readInt32(in, &a);
            std::lock_guard<std::mutex> g(gStyleLock);
            gSoftStyle = a == 1;
            return ok(out);
        }
        case WRITE_HAPTIC_AUDIO:
        case SET_POWER_ON_SWITCH:
        case LM_OFF:
        case LM_ON:
        case START_CALIBRATE:
        case SET_VMAX:
        case SET_MOTOR_OLD_TEST:
        case WRITE_GUN_TYPE:
        case WRITE_GUN_MODE:
        case WRITE_BULLET_NUM:
            return ok(out);
        case GET_INTERFACE_VERSION:
            ok(out);
            return AParcel_writeInt32(out, kOplusVersion);
        case GET_INTERFACE_HASH:
            ok(out);
            return AParcel_writeString(out, kOplusHash, static_cast<int32_t>(strlen(kOplusHash)));
        default:
            return STATUS_UNKNOWN_TRANSACTION;
    }
}

void* onCreate(void* args) { return args; }
void onDestroy(void*) {}
binder_status_t onClientTransact(AIBinder*, transaction_code_t, const AParcel*, AParcel*) {
    return STATUS_UNKNOWN_TRANSACTION;
}

// ---- registration ----------------------------------------------------------------------------------------
AIBinder_DeathRecipient* gDeath = nullptr;

// Takes Xiaomi's IVibrator (+ IVibratorExt extension) as the backend.
bool attach(AIBinder* xm) {
    if (!AIBinder_associateClass(xm, gVibClass)) {
        LOGE("%s is not %s", kVibName, kVibDescriptor);
        AIBinder_decStrong(xm);
        return false;
    }
    AIBinder* ext = nullptr;
    if (AIBinder_getExtension(xm, &ext) != STATUS_OK || ext == nullptr || !AIBinder_associateClass(ext, gXmExtClass)) {
        LOGE("Xiaomi HAL has no %s extension", kXmExtDescriptor);
        if (ext) AIBinder_decStrong(ext);
        AIBinder_decStrong(xm);
        return false;
    }
    AIBinder_linkToDeath(xm, gDeath, nullptr);
    gBackend.set(xm, ext);
    return true;
}

void registerSelf() {
    binder_exception_t ex = gV.addService(gSelf, kVibName);
    if (ex != EX_NONE) LOGE("addService(%s) failed: %d", kVibName, ex);
    else LOGI("%s: OnePlus vibrator service (IVibrator V%d + %s V%d) on Xiaomi vibratorfeature", kVibName,
              kVibVersion, kOplusDescriptor, kOplusVersion);
}

void onXiaomiDied(void*) {
    LOGW("Xiaomi vibratorfeature HAL died, waiting for it to register again");
    gBackend.clear();
}

// servicemanager calls this for every registration of IVibrator/default: Xiaomi's HAL at boot and after
// each restart (it hard-codes that name), and our own. Xiaomi's becomes the backend and we take the name
// back; clients that hold our binder keep using it throughout.
std::mutex gAttachLock;
void onRegistered(const char*, AIBinder* registered, void*) {
    if (registered == nullptr || !AIBinder_isRemote(registered)) return;   // our own registration
    std::lock_guard<std::mutex> g(gAttachLock);
    AIBinder_incStrong(registered);
    if (attach(registered)) registerSelf();
}

}  // namespace

int main() {
    if (!loadVndk()) return 1;
    gV.setThreadPoolMaxThreadCount(4);
    gV.startThreadPool();

    gVibClass = AIBinder_Class_define(kVibDescriptor, onCreate, onDestroy, onVibTransact);
    gXmExtClass = AIBinder_Class_define(kXmExtDescriptor, onCreate, onDestroy, onClientTransact);
    gOplusClass = AIBinder_Class_define(kOplusDescriptor, onCreate, onDestroy, onOplusTransact);
    gCbClass = AIBinder_Class_define(kCbDescriptor, onCreate, onDestroy, onClientTransact);
    gDeath = AIBinder_DeathRecipient_new(onXiaomiDied);

    gSelf = AIBinder_new(gVibClass, nullptr);
    AIBinder* oplus = AIBinder_new(gOplusClass, nullptr);
    gV.markVintfStability(gSelf);   // system_server only uses vendor binders with VINTF stability
    gV.markVintfStability(oplus);
    if (AIBinder_setExtension(gSelf, oplus) != STATUS_OK) return 1;

    // fires right away if Xiaomi's HAL is already registered, else as soon as it registers
    if (gV.registerForServiceNotifications(kVibName, onRegistered, nullptr) == nullptr) {
        LOGE("cannot watch %s", kVibName);
        return 1;
    }

    // Failsafe. Without Xiaomi's HAL (vibratorfeature-hal-stock not running, e.g. its binary missing from the odm
    // image) nobody registers IVibrator/default; system_server waits for it at boot and its watchdog reboots the
    // phone in a loop. Register anyway after a while: the phone boots, just without vibration, and as soon as
    // Xiaomi's HAL does register, onRegistered attaches it and everything works.
    std::thread([] {
        if (gBackend.waitFirstAttach(kXiaomiWaitMs)) return;
        std::lock_guard<std::mutex> g(gAttachLock);
        if (gBackend.everAttached()) return;
        LOGE("Xiaomi's HAL (service vibratorfeature-hal-service, %s) has not registered %s after %d ms: registering "
             "without it so the phone can boot - there is no vibration until it runs", kXiaomiHalPath, kVibName,
             kXiaomiWaitMs);
        registerSelf();
    }).detach();
    gV.joinThreadPool();
    return 0;
}
