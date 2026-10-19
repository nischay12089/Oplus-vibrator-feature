// vendor.xiaomi.hardware.vibratorfeature.service for the POCO F7 (onyx): O-Haptics' own vibrator HAL.
//
// One binary, no libraries, nothing patched. It replaces Xiaomi's HAL file in place, so Xiaomi's init rc
// (service vibratorfeature-hal-service) and its VINTF manifest stay byte-for-byte stock: the rc only names
// this path and the manifest only declares the interface this HAL serves.
//
// It serves what OnePlus' framework expects of android.hardware.vibrator.IVibrator/default:
//   * the OnePlus calling convention: setAmplitude(1 + strength/2400) before perform(<OnePlus waveform id>),
//     setAmplitude(10000|10001) for the touch style (soft plays <id>+1000 where a soft variant exists)
//   * vendor.oplus.hardware.oplusvibrator.IOplusVibrator (V2) as the binder extension: AAC RichTap HE
//
// The motor is driven directly through the kernel's force-feedback device (si_haptic / SI688X):
//   FF_GAIN                                        amplitude
//   FF_CONSTANT + replay.length                    on(): a plain buzz
//   FF_PERIODIC, custom_data[0] = 197, PCM written to the driver's custom_wave node
//                                                  any waveform: the effect library and RichTap patterns
// Effects come from /odm/firmware/<id>_<name>_RTP.bin (raw int8 PCM at 24 kHz, as the kit builds them).
// RichTap HE events are rendered to PCM here, so there is no 16-event limit and no renderer to ask: a
// pattern is one timeline this HAL mixes and streams. Playback is owned by one streaming thread that can be
// preempted at any moment, so a tap during a long pattern is never queued behind it.

#include <android/binder_ibinder.h>
#include <android/binder_parcel.h>
#include <android/binder_status.h>
#include <android/log.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <math.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include <time.h>
#include <unistd.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include <algorithm>
#include <chrono>
#include <climits>
#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#define TAG "vendor.vibratorfeature"
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
constexpr char kOplusDescriptor[] = "vendor.oplus.hardware.oplusvibrator.IOplusVibrator";
constexpr char kOplusHash[] = "b103e6d812aa472b9c5b7deefd7986fa729ce1a8";     // IOplusVibrator V2
constexpr int32_t kOplusVersion = 2;
constexpr char kCbDescriptor[] = "vendor.aac.hardware.richtap.vibrator.IRichtapCallback";
constexpr char kVibCbDescriptor[] = "android.hardware.vibrator.IVibratorCallback";
// VibratorFeatureLoader only adds features the XML lacks; report what this HAL provides.
constexpr char kFeatures[] = "vibrator_lmvibrator=1\nvibrator_qcom_lmvibrator=1\nvibrator_richctap=1\n";
// IVibrator.getCapabilities: ON_CALLBACK | PERFORM_CALLBACK | AMPLITUDE_CONTROL
constexpr int32_t kCapabilities = 1 | 2 | 4;

constexpr transaction_code_t GET_INTERFACE_HASH = 16777214, GET_INTERFACE_VERSION = 16777215;
// android.hardware.vibrator.IVibrator
constexpr transaction_code_t VIB_GET_CAPABILITIES = 1, VIB_OFF = 2, VIB_ON = 3, VIB_PERFORM = 4,
                             VIB_GET_SUPPORTED_EFFECTS = 5, VIB_SET_AMPLITUDE = 6;
// vendor.oplus.hardware.oplusvibrator.IOplusVibrator
enum : transaction_code_t {
    INIT = 1, SET_DYNAMIC_SCALE, SET_F0, STOP, SET_AMPLITUDE, PERFORM_HE_PARAM, OFF, ON, PERFORM,
    PERFORM_ENVELOPE, PERFORM_RTP, PERFORM_HE, SET_HAPTIC_PARAM, LM_OFF, LM_ON, START_CALIBRATE,
    GET_CALIBRATE_RESULTS, GET_STATUS, SET_VMAX, SET_MOTOR_OLD_TEST, GET_SUPPORT_FEATURES, WRITE_GUN_TYPE,
    WRITE_GUN_MODE, WRITE_BULLET_NUM, WRITE_HAPTIC_AUDIO, SET_TOUCH_STYLE, GET_TOUCH_STYLE, SET_POWER_ON_SWITCH,
};
constexpr transaction_code_t CB_ON_CALLBACK = 1;   // IRichtapCallback.onCallback(int) - oneway

// The soft touch style plays <id + 1000>, which is where the kit installs the soft variants of OnePlus 13's
// "soft_style" ids. Whether one exists is answered by the library itself rather than a list repeated here.
constexpr int kSoftOffset = 1000;

// ---- motor: the si_haptic force-feedback device -------------------------------------------------------
constexpr char kInputDir[] = "/dev/input";
constexpr char kMotorName[] = "si_haptic";
constexpr char kSysPathProp[] = "ro.odm.mm.vibrator.si_sys_path";
constexpr char kF0Prop[] = "ro.odm.mm.vibrator.resonant_frequency";
constexpr int kRate = 24000;          // the effect library's PCM rate
constexpr int kPeriod = 512;          // custom_wave period; a request that is not a whole number of periods
                                      // tells the driver it is the last one, so only the final write is short
constexpr int kChunk = 1024;           // bytes per write: small enough that one blocked write is brief
// Writing samples only queues them: the driver still needs to set the chip up over i2c and then play
// them out. Tearing the stream down before that is done throws the queued samples away, so playback is
// given its own length plus this much slack before the effect is erased.
constexpr int kDrainMs = 45;
// The framework calls off() as cleanup right after a short effect, while the motor is often still moving.
// An off() this close to the natural end is ignored, since nothing perceptible can be cancelled in it.
constexpr int kBriefMs = 150;
constexpr int kCustomWaveId = 197;    // the driver's CUSTOME_WAVE_ID: "I am streaming you samples"
constexpr float kDefaultF0 = 168.5f;
constexpr char kFirmwareDir[] = "/odm/firmware";
// The touch style (Crisp/Gentle). The framework announces it with setAmplitude(10000|10001) only when the
// user changes it, so after a restart or boot it has to be read from the setting itself. An earlier build kept
// its own copy instead; that copy drifted from the real setting and played Gentle while the user had Crisp,
// cutting everything to a third - so the setting is now the only record.
constexpr char kStyleKey[] = "touch_style_vibration_intensity";   // 0 = Crisp, 1 = Gentle
// The framework applies the user's intensity slider to ordinary effects, as the strength inside
// setAmplitude(1 + strength/2400) - so those must NOT be scaled again here. RichTap patterns are the
// exception: apps send amplitude 0 ("no override") and nothing scales them, so the setting is read here and
// applied to patterns only. It lives in system_server's own store, which this HAL can read because Xiaomi's
// rc runs it as uid system; re-read only when that file's mtime changes.
// (Note: `settings put` from a shell does not reach the framework's cached copy, so it is useless for
// testing this - only moving the real slider is.)
constexpr char kSettingsPath[] = "/data/system/users/0/settings_system.xml";
constexpr char kIntensityKey[] = "touch_stepless_vibration_intensity";
constexpr int kIntensityMax = 2400;   // the slider's full-scale value
// Level for on() - the plain buzz - which the driver plays from its own RAM wave, so it can only be set
// through FF_GAIN.
constexpr float kOutputScale = 0.75f;
// Effects and RichTap patterns get their strength from their SAMPLES, never from FF_GAIN, because the
// driver's gain stage cannot express it. Measured on this phone (FF_GAIN -> chip level):
//   100% -> 0x80   74% -> 0x40   49% -> 0x1e   37% -> 0x1e   24% -> 0x1e   3% -> 0x1e
// Everything below about half collapses onto one floor, and above it there are only a few coarse steps -
// so the intensity slider, the Gentle style and every trim below that point changed nothing at the motor.
// Xiaomi's own HAL avoided this the same way: it kept the gain at full and scaled the samples.
// Where full strength lands, per kind of playback:
//  * library effects: 1.45 - 45% above the authored waveform, chosen by ear (1.70, 1.55 and 1.39 were tried). Each effect is raised only as
//    far as its own headroom allows (scaleSamples); a short effect already at full scale gets the rest from a
//    soft knee instead (kneeSamples). At 1.0 it would match Xiaomi's HAL (kit 8f415157) exactly
//  * RichTap patterns: 0.5 - the level the O-Haptics video was tuned at (kHeTrim); patterns sustain, and at
//    full drive they were too strong
constexpr float kEffectScale = 1.45f;
// Short effects that are already at full scale - the taps, the keyboard, volume, the slider end-stop - get the
// rest of their strength from a soft knee: the peak stays at full scale and everything below it is lifted, so
// the effect carries more energy. Short ones only: on sustained content a knee adds odd harmonics of f0 that
// buzz the housing, so circle-to-search (600 ms) stops at its clean maximum, about x1.48. The driver clamps
// its own gain at 0x80 whatever is asked (measured up to 0xffff), so there is no headroom left above this
// short of the chip's boost voltage (drv_vboost), which is deliberately left alone.
constexpr int kKneeMaxMs = 60;
// The boost above can still over-drive one family of taps: holding an app plays effect 6 (8 and 109 share its
// waveform), whose three near-full half-cycles push the mass further than any other tap. Lifted x1.45 at
// full intensity it went past the motor's travel and rattled the back panel; at half intensity it never got
// there. So a short effect is boosted only as far as the library's own strongest tap moves the mass when it
// plays as authored (Library::excursionCeiling). Measured with a resonator at f0 of this damping; only
// waveforms are compared with each other, and the ranking was the same from Q 4 to 16.
constexpr float kMotorQ = 8.f;
constexpr float kPatternScale = 0.5f;
// A library tick is over in ~12 ms; a RichTap pattern sustains for hundreds of ms, and sustained movement
// at resonance reads far stronger than the same peak in a tick. Patterns are trimmed against the library
// for that, and long continuous events again by the staging the kit gives long library content
// (build_bridge.py --long-gain 0.8 --long-ms 300). Lower kHeTrim to take patterns down further.
constexpr float kHeTrim = 0.36f;
// The library's own soft waveforms are only ~0.77 of the normal ones, so the soft touch style barely
// changes how hard the motor is driven. Gentle is trimmed on top of that: effects by kGentleEffectScale,
// RichTap patterns by kSoftTrim. A single 0.3 for both was chosen while the driver's gain stage was flooring
// every level, so it barely applied then; with real levels Gentle fell to ~23% of Crisp, far too weak.
// Patterns on Gentle: soft ratio x 0.3 of their Crisp level.
constexpr float kSoftTrim = 0.3f;
// Where a full-strength effect lands on Gentle at maximum intensity - the Gentle counterpart of
// kEffectScale, for the taps, sliders and holding an app. 0.435 (with kSoftTrim 0.3) is exactly the Gentle of kit
// aa301c30, which the user lived with for a week and called perfect; 0.5-0.87 were tried live but never flashed.
// Effects the kit built no soft waveform for get the library's soft ratio on top, so they match the ones that
// have one.
constexpr float kGentleEffectScale = 0.435f;
// A 17 ms tick and a 105 ms buzz at the same amplitude are not felt as equally strong: perceived strength
// keeps growing with duration well past 100 ms. The kit also boosts everything up to 300 ms (--ui-gain 1.25)
// and retunes it onto resonance, where this motor couples hardest - so the slider end-stop, a 105 ms effect,
// arrives louder than the ticks around it. Longer effects are trimmed back toward tick loudness, down to a
// floor so notification patterns stay present. Raise kDurTrimFloor to keep long effects stronger.
constexpr int kTickMs = 20;
// 1.0 turns the trim off entirely: every effect plays at the strength the framework asked for, as Xiaomi's
// HAL did (kit 8f415157). The user chose this; lower it to bring the trim back for long effects.
constexpr float kDurTrimFloor = 1.0f;
// How steeply the trim falls with duration: at 0.35 the 104 ms slider end-stop lands at 0.56 and a 33 ms tap
// keeps 0.84, while circle-to-search and the brightness slider (600 ms, effect 362) sit at the floor.
// Both values were first tuned while the driver's gain stage was flooring every level (see kEffectScale), so
// the trim barely applied then; with real levels the old 0.63 / 0.3 cut those effects to a third.
constexpr float kDurTrimExp = 0.35f;
constexpr int kLongMs = 300;
constexpr float kLongGain = 0.8f;

int64_t nowMs() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

std::string prop(const char* key) {
    char buf[PROP_VALUE_MAX] = {};
    __system_property_get(key, buf);
    return buf;
}

int msOf(size_t samples) { return static_cast<int>(samples * 1000 / kRate); }

// Lifts a waveform's level below its peak by `d` (> 1) while keeping the peak where it is.
float kneeSamples(std::vector<int8_t>& pcm, float d) {
    if (d <= 1.f) return 1.f;
    const float norm = tanhf(d);
    for (int8_t& v : pcm) {
        float u = d * static_cast<float>(v) / 127.f;
        v = static_cast<int8_t>(clampi(static_cast<int>(lrintf(127.f * tanhf(u) / norm)), -127, 127));
    }
    return d;
}

// Sets a streamed waveform's strength (see kEffectScale) and returns the factor actually applied. Raising a
// waveform above its authored level is allowed only up to its own headroom: clipping it would add odd
// harmonics of f0 - around 505 and 840 Hz - which are audible and rattle the housing.
float scaleSamples(std::vector<int8_t>& pcm, float g) {
    if (g < 0.f) g = 0.f;
    if (g > 1.f) {
        int peak = 0;
        for (int8_t v : pcm) peak = std::max(peak, std::abs(static_cast<int>(v)));
        if (peak > 0) g = std::min(g, 127.f / static_cast<float>(peak));
    }
    if (g == 1.f) return g;
    for (int8_t& v : pcm)
        v = static_cast<int8_t>(clampi(static_cast<int>(lrintf(static_cast<float>(v) * g)), -127, 127));
    return g;
}

// Gives a library effect the strength `want` (1 = as authored): linearly up to its own headroom, and for a
// short effect the rest through the soft knee. Returns the knee used (1 = none).
float shapeEffect(std::vector<int8_t>& pcm, float want, float* applied) {
    *applied = scaleSamples(pcm, want);
    if (*applied > 0.f && want > *applied + 1e-3f && msOf(pcm.size()) <= kKneeMaxMs)
        return kneeSamples(pcm, want / *applied);
    return 1.f;
}

// How far a waveform swings the motor's mass: the peak of a resonator at f0 driven by it, including 25 ms of
// ring-out after it ends. The units are arbitrary - it is only compared with Library::excursionCeiling().
float excursionOf(const std::vector<int8_t>& pcm, float f0) {
    const float w = 2.f * 3.14159265f * f0;
    const float dt = 1.f / kRate;
    float v = 0.f, p = 0.f, peak = 0.f;
    for (size_t k = 0; k < pcm.size() + kRate / 40; ++k) {
        float u = k < pcm.size() ? static_cast<float>(pcm[k]) : 0.f;
        v += (u - (w / kMotorQ) * v - w * w * p) * dt;
        p += v * dt;
        peak = std::max(peak, fabsf(p));
    }
    return peak;
}

// An integer setting from system_server's store. The file is Android binary XML ("ABX\0"), not text: each
// attribute is a 0x2f token, a two-byte interned name index, a two-byte length, then the characters, and the
// attribute directly after a setting's name holds its value:
//   "touch_stepless_vibration_intensity"  2f 00 05 00 04  "2400"
int abxInt(const std::string& xml, const char* key, int fallback) {
    size_t at = xml.find(key);
    if (at == std::string::npos) return fallback;
    size_t from = at + strlen(key);
    size_t left = xml.size() - from;
    const unsigned char* p = reinterpret_cast<const unsigned char*>(xml.data()) + from;
    if (left < 6 || p[0] != 0x2f) return fallback;
    size_t len = (static_cast<size_t>(p[3]) << 8) | p[4];
    if (len < 1 || len > 8 || left < 5 + len) return fallback;
    std::string digits(reinterpret_cast<const char*>(p + 5), len);
    if (digits.find_first_not_of("0123456789") != std::string::npos) return fallback;
    return atoi(digits.c_str());
}

struct Settings {
    int intensity = -1;   // touch_stepless_vibration_intensity, 0..2400; -1 = unknown
    int style = -1;       // touch_style_vibration_intensity; -1 = unknown
};

// The settings this HAL follows. Cheap to call: the file is parsed only when it has changed (nanosecond
// mtime, so two changes in one second are not missed), and at most once a second.
Settings readSettings() {
    static std::mutex mu;
    static int64_t checked = 0;
    static timespec seen = {0, 0};
    static Settings snap;
    std::lock_guard<std::mutex> l(mu);
    int64_t now = nowMs();
    if (checked != 0 && now - checked < 1000) return snap;
    checked = now;
    // Said once: on an enforcing ROM whose policy does not let this HAL read system_server's settings, the
    // slider no longer reaches RichTap patterns and Crisp/Gentle is only learnt when the user changes it.
    static bool warned = false;
    auto unreadable = [](const char* what) {
        if (warned) return;
        warned = true;
        LOGW("cannot %s %s: %s - intensity and touch style fall back to what the framework announces", what,
             kSettingsPath, strerror(errno));
    };
    struct stat st;
    if (stat(kSettingsPath, &st) != 0) {
        unreadable("stat");
        return snap;
    }
    if (st.st_mtim.tv_sec == seen.tv_sec && st.st_mtim.tv_nsec == seen.tv_nsec) return snap;
    seen = st.st_mtim;
    FILE* f = fopen(kSettingsPath, "re");
    if (f == nullptr) {
        unreadable("open");
        return snap;
    }
    std::string xml;
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) xml.append(buf, n);
    fclose(f);
    snap.intensity = abxInt(xml, kIntensityKey, snap.intensity);
    snap.style = abxInt(xml, kStyleKey, snap.style);
    return snap;
}

// The user's vibration intensity, 0..1; full strength if it cannot be read, so haptics never go silent.
float userIntensity() {
    int v = readSettings().intensity;
    return (v >= 0 && v <= kIntensityMax) ? static_cast<float>(v) / kIntensityMax : 1.f;
}


void traceLog(const char* fmt, ...) __attribute__((format(printf, 1, 2)));   // diagnostics, defined below

// Drives the motor. One streaming thread owns playback: every new request or stop bumps a generation
// counter, so whatever is playing is abandoned at the next period instead of being waited for.
class Motor {
public:
    bool start() {
        if (!openDevice()) return false;
        sysfs_ = prop(kSysPathProp);
        if (sysfs_.empty()) sysfs_ = "/sys/bus/i2c/drivers/sih_haptic_688X/5-006b";
        wave_ = sysfs_ + "/custom_wave";
        f0_ = readF0();
        std::thread([this] { streamLoop(); }).detach();
        LOGI("motor: %s, custom_wave %s, f0 %.1f Hz", dev_.c_str(), wave_.c_str(), static_cast<double>(f0_));
        return true;
    }

    bool ready() const { return fd_ >= 0; }
    float f0() const { return f0_; }
    bool streaming() const { return streamId_.load() >= 0; }

    void setGain(float g) {
        if (fd_ < 0) return;
        int32_t v = clampi(static_cast<int>(lrintf(g * kOutputScale * 0x7fff)), 0, 0x7fff);
        std::lock_guard<std::mutex> l(evMu_);
        if (v == gain_) return;
        gain_ = v;
        sendEvent(FF_GAIN, v);
    }

    // on(): a plain buzz of `ms`, the driver's own continuous mode.
    void buzz(int ms) {
        traceLog("    motor.buzz ms=%d", ms);
        stop(true);
        playEnd_.store(nowMs() + ms + kDrainMs);
        looping_.store(false);
        if (fd_ < 0 || ms <= 0) return;
        std::lock_guard<std::mutex> l(evMu_);
        ff_effect e = {};
        e.type = FF_CONSTANT;
        e.replay.length = static_cast<uint16_t>(std::min(ms, 0xffff));
        e.u.constant.level = 0x7fff;
        int id = upload(e);
        if (id < 0) return;
        sendEvent(static_cast<uint16_t>(id), 1);
        buzzId_ = id;
    }

    // Hands a rendered timeline to the streaming thread. loops < 0 repeats until stop(). The samples already
    // carry their strength, so the driver gain goes to full: anything less is quantised (see kEffectScale).
    void play(std::vector<int8_t> pcm, int loops, int gapMs) {
        traceLog("    motor.play bytes=%zu loops=%d gap=%d fd=%d", pcm.size(), loops, gapMs, fd_);
        if (fd_ < 0 || pcm.empty()) return;
        {
            std::lock_guard<std::mutex> l(evMu_);
            if (gain_ != 0x7fff) {
                gain_ = 0x7fff;
                sendEvent(FF_GAIN, 0x7fff);
            }
        }
        int64_t len = msOf(pcm.size());
        looping_.store(loops < 0);
        playEnd_.store(loops < 0 ? INT64_MAX / 4
                                 : nowMs() + loops * len + std::max(0, loops - 1) * gapMs + kDrainMs);
        {
            std::lock_guard<std::mutex> l(mu_);
            pcm_ = std::move(pcm);
            loops_ = loops;
            gap_ = std::max(0, gapMs);
            gen_++;
            work_ = true;
        }
        abortPlayback();   // whatever is streaming gives up at once; a new pattern never queues behind it
        cv_.notify_all();
    }

    // Stops immediately: nothing here waits for the driver to drain what it already streamed. `force` is
    // for taking the motor over (a new buzz); without it, an effect that is about to end on its own is left
    // alone, because the framework's routine off() lands while short effects are still playing.
    void stop(bool force = false) {
        int64_t left = playEnd_.load() - nowMs();
        if (!force && !looping_.load() && left > 0 && left <= kBriefMs) {
            traceLog("    motor.stop ignored, %lldms of a brief effect left", static_cast<long long>(left));
            return;
        }
        traceLog("    motor.stop");
        playEnd_.store(0);
        {
            std::lock_guard<std::mutex> l(mu_);
            gen_++;
            work_ = false;
        }
        {
            std::lock_guard<std::mutex> l(evMu_);
            if (buzzId_ >= 0) {
                sendEvent(static_cast<uint16_t>(buzzId_), 0);
                erase(buzzId_);
                buzzId_ = -1;
            }
        }
        abortPlayback();
    }

private:
    // Ends the stream the driver is playing. Erasing the effect makes it drop its ring buffer, which also
    // releases a write that is blocked waiting for room, so the streaming thread notices at once.
    void abortPlayback() {
        int id = streamId_.exchange(-1);
        if (id < 0) return;
        std::lock_guard<std::mutex> l(evMu_);
        sendEvent(static_cast<uint16_t>(id), 0);
        erase(id);
    }

    bool openDevice() {
        DIR* d = opendir(kInputDir);
        if (d == nullptr) return false;
        while (dirent* e = readdir(d)) {
            if (strncmp(e->d_name, "event", 5) != 0) continue;
            std::string path = std::string(kInputDir) + "/" + e->d_name;
            int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
            if (fd < 0) continue;
            char name[128] = {};
            unsigned long bits[4] = {};
            bool match = ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) >= 0 && strcmp(name, kMotorName) == 0 &&
                         ioctl(fd, EVIOCGBIT(EV_FF, sizeof(bits)), bits) >= 0 &&
                         (bits[FF_CUSTOM / (8 * sizeof(long))] & (1UL << (FF_CUSTOM % (8 * sizeof(long)))));
            if (match) {
                fd_ = fd;
                dev_ = path;
                break;
            }
            close(fd);
        }
        closedir(d);
        if (fd_ < 0) LOGE("no %s force-feedback device under %s", kMotorName, kInputDir);
        return fd_ >= 0;
    }

    float readF0() {
        FILE* f = fopen((sysfs_ + "/f0").c_str(), "re");
        int raw = 0;
        if (f != nullptr) {
            if (fscanf(f, "%d", &raw) != 1) raw = 0;
            fclose(f);
        }
        if (raw > 1000 && raw < 5000) return raw / 10.0f;   // sysfs reports tenths of a Hz: 1685 -> 168.5
        float p = static_cast<float>(atof(prop(kF0Prop).c_str()));
        return p > 50.f ? p : kDefaultF0;
    }

    bool sendEvent(uint16_t code, int32_t value) {   // evMu_ held
        input_event e = {};
        e.type = EV_FF;
        e.code = code;
        e.value = value;
        if (write(fd_, &e, sizeof(e)) == static_cast<ssize_t>(sizeof(e))) return true;
        LOGW("EV_FF code %u value %d: %s", code, value, strerror(errno));
        return false;
    }

    int upload(ff_effect& e) {   // evMu_ held
        e.id = -1;
        if (ioctl(fd_, EVIOCSFF, &e) >= 0) return e.id;
        LOGW("EVIOCSFF: %s", strerror(errno));
        return -1;
    }

    void erase(int id) {   // evMu_ held
        if (id >= 0) ioctl(fd_, EVIOCRMFF, id);
    }

    // Puts the driver in custom-wave mode and starts it; samples follow on the custom_wave node.
    bool beginStream() {   // streaming thread only
        ff_effect e = {};
        e.type = FF_PERIODIC;
        e.u.periodic.waveform = FF_CUSTOM;
        e.u.periodic.magnitude = 0x7fff;
        int16_t data[3] = {kCustomWaveId, 0, 0};
        e.u.periodic.custom_len = sizeof(data);
        e.u.periodic.custom_data = data;
        std::lock_guard<std::mutex> l(evMu_);
        int id = upload(e);
        if (id < 0) return false;
        waveFd_ = ::open(wave_.c_str(), O_WRONLY | O_CLOEXEC);
        if (waveFd_ < 0) {
            LOGW("open %s: %s", wave_.c_str(), strerror(errno));
            erase(id);
            return false;
        }
        if (!sendEvent(static_cast<uint16_t>(id), 1)) {
            close(waveFd_);
            waveFd_ = -1;
            erase(id);
            return false;
        }
        streamId_ = id;
        return true;
    }

    // Only the streaming thread closes the custom_wave fd: another thread closing it under a write in
    // flight could land on a reused descriptor. Interrupting is abortPlayback's job instead.
    void endStream() {   // streaming thread only
        if (waveFd_ >= 0) {
            close(waveFd_);
            waveFd_ = -1;
        }
        abortPlayback();
    }

    // Streams `pcm` once. Returns false if a newer request (or a stop) superseded this one.
    // The writes are deliberately outside every mutex: the driver holds a write until its ring buffer has
    // room, i.e. until the samples already queued have played, so holding a lock here would make a tap
    // wait for the whole pattern - the stall this HAL exists to avoid.
    bool streamOnce(const std::vector<int8_t>& pcm, uint64_t gen) {
        if (!beginStream()) return true;   // nothing started, so nothing to abandon
        size_t off = 0, whole = pcm.size() - pcm.size() % kPeriod;
        while (off < whole) {
            if (superseded(gen)) return false;
            if (waveFd_ < 0) return true;
            size_t n = std::min(whole - off, static_cast<size_t>(kChunk));
            ssize_t w = write(waveFd_, pcm.data() + off, n);
            if (w > 0) {
                off += static_cast<size_t>(w);
            } else if (w == 0 || errno == EINTR || errno == EAGAIN || errno == ENOSPC) {
                std::this_thread::sleep_for(std::chrono::milliseconds(4));
            } else {
                return !superseded(gen);   // erased from under us: a stop, or a real error
            }
        }
        // The last request is short on purpose: that is the end-of-stream marker. If the timeline divided
        // evenly into periods, one silent sample is appended so there is still a short request to send.
        if (superseded(gen)) return false;
        int8_t tail[kPeriod] = {};
        size_t rest = pcm.size() - whole;
        if (rest > 0) memcpy(tail, pcm.data() + whole, rest);
        else rest = 1;
        if (waveFd_ >= 0 && write(waveFd_, tail, rest) < 0 && errno != EINTR)
            LOGW("custom_wave tail %zu: %s", rest, strerror(errno));
        return true;
    }

    // Sleeps until `until`, giving up early if a newer request superseded this one.
    bool waitUntil(int64_t until, uint64_t gen) {
        while (nowMs() < until) {
            if (superseded(gen)) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return !superseded(gen);
    }

    bool superseded(uint64_t gen) {
        std::lock_guard<std::mutex> l(mu_);
        return gen_ != gen;
    }

    void streamLoop() {
        for (;;) {
            std::vector<int8_t> pcm;
            int loops, gap;
            uint64_t gen;
            {
                std::unique_lock<std::mutex> l(mu_);
                cv_.wait(l, [this] { return work_ || quit_; });
                if (quit_) return;
                work_ = false;
                pcm = pcm_;
                loops = loops_;
                gap = gap_;
                gen = gen_;
            }
            endStream();   // a new pattern supersedes whatever was playing, exactly as it does on OnePlus
            for (int i = 0; loops < 0 || i < loops; i++) {
                // Timed from when the pass started, not from when the last write returned: the driver
                // accepts samples ahead of the motor.
                int64_t t0 = nowMs();
                if (!streamOnce(pcm, gen)) break;
                // Writing the samples only queued them. Wait out the playback before ending the stream,
                // or the erase in endStream() discards whatever has not reached the motor yet - which
                // silenced every effect short enough to be written in one go.
                if (!waitUntil(t0 + msOf(pcm.size()) + kDrainMs, gen)) break;
                if (loops >= 0 && i + 1 >= loops) break;
                if (gap > 0 && !waitUntil(nowMs() + gap, gen)) break;
                endStream();   // each repeat is its own stream, so the gap is real silence
            }
            endStream();
        }
    }

    int fd_ = -1, waveFd_ = -1, buzzId_ = -1;
    std::atomic<int> streamId_{-1};   // the effect the streaming thread is playing; -1 when idle
    int32_t gain_ = -1;
    std::atomic<int64_t> playEnd_{0};   // when the current playback is expected to be over
    std::atomic<bool> looping_{false};
    std::string dev_, sysfs_, wave_;
    float f0_ = kDefaultF0;
    std::mutex evMu_;   // serialises the quick ioctls and events on the FF fd; never held across a write
    std::mutex mu_;
    std::condition_variable cv_;
    bool work_ = false, quit_ = false;
    uint64_t gen_ = 0;
    std::vector<int8_t> pcm_;
    int loops_ = 1, gap_ = 0;
};
Motor gMotor;

// ---- the effect library: /odm/firmware/<id>_<name>_RTP.bin, raw int8 PCM at 24 kHz -------------------
class Library {
public:
    void scan() {
        DIR* d = opendir(kFirmwareDir);
        if (d == nullptr) {
            LOGE("cannot read %s", kFirmwareDir);
            return;
        }
        while (dirent* e = readdir(d)) {
            int id = -1;
            char rest[256] = {};
            // "<id>_<name>_RTP.bin", the naming Xiaomi's HAL scans for and the kit writes
            if (sscanf(e->d_name, "%d_%255s", &id, rest) != 2 || id < 0) continue;
            const char* dot = strstr(rest, "_RTP.bin");
            if (dot == nullptr || dot[8] != '\0') continue;
            files_[id] = std::string(kFirmwareDir) + "/" + e->d_name;
        }
        closedir(d);
        LOGI("effect library: %zu waveforms in %s", files_.size(), kFirmwareDir);
    }

    bool has(int id) const { return files_.count(id) != 0; }

    std::vector<int8_t> load(int id) const {
        std::vector<int8_t> pcm;
        auto it = files_.find(id);
        if (it == files_.end()) return pcm;
        FILE* f = fopen(it->second.c_str(), "re");
        if (f == nullptr) return pcm;
        char buf[8192];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) pcm.insert(pcm.end(), buf, buf + n);
        fclose(f);
        return pcm;
    }

    // How loud the library's own short effects are, as a fraction of full scale. The kit gain-stages them
    // when it builds them (a tap peaks around 93 of 127), while a RichTap pattern is synthesised here and
    // would otherwise peak at full scale - which is why patterns felt hotter than everything else. Measured
    // from the installed files, so it follows the library instead of being a number chosen here.
    float headroom() const { return headroom_; }
    // How much gentler the library's soft waveforms are than the ones they replace. Effects with no soft
    // waveform of their own are scaled by this on the soft touch style, so Gentle is gentle everywhere
    // instead of only where the kit happened to build a variant.
    float softRatio() const { return softRatio_; }
    // The furthest a boosted short effect may swing the motor's mass (excursionOf); 0 = no limit.
    float excursionCeiling() const { return excursionCeiling_; }

    // Sets excursionCeiling() from the library's own waveforms, as they were authored, driving a motor at f0.
    void measureExcursion(float f0) {
        // Taps only (<= kTickMs): the kit drives its strongest tap as hard as this motor is meant to go, so
        // that is the limit. Longer effects are left out on purpose - effect 11 (28 ms) swings ~30% further,
        // and a ceiling that high would never catch the tap that rattled. Soft variants are quieter copies.
        for (const auto& e : files_) {
            struct stat st;   // one byte per sample, so the size says the length without reading ringtones
            if (e.first >= kSoftOffset || stat(e.second.c_str(), &st) != 0) continue;
            if (st.st_size <= 0 || msOf(static_cast<size_t>(st.st_size)) > kTickMs) continue;
            excursionCeiling_ = std::max(excursionCeiling_, excursionOf(load(e.first), f0));
        }
        LOGI("excursion ceiling: %.6f at f0 %.1f Hz", static_cast<double>(excursionCeiling_),
             static_cast<double>(f0));
    }

    int peakOf(int id) const {
        int peak = 0;
        for (int8_t v : load(id)) peak = std::max(peak, std::abs(static_cast<int>(v)));
        return peak;
    }

    void measureHeadroom() {
        std::vector<int> peaks;
        for (int id : {0, 1, 2, 3, 4, 6, 7}) {
            int peak = peakOf(id);
            if (peak > 0) peaks.push_back(peak);
        }
        if (!peaks.empty()) {
            std::sort(peaks.begin(), peaks.end());
            headroom_ = clampi(peaks[peaks.size() / 2], 40, 127) / 127.f;
            LOGI("library headroom: %d of 127 (%.2f) from %zu short effects", peaks[peaks.size() / 2],
                 static_cast<double>(headroom_), peaks.size());
        }
        std::vector<float> ratios;
        for (const auto& e : files_) {
            if (e.first >= kSoftOffset || !has(e.first + kSoftOffset)) continue;
            int base = peakOf(e.first), soft = peakOf(e.first + kSoftOffset);
            if (base > 0 && soft > 0) ratios.push_back(static_cast<float>(soft) / base);
        }
        if (!ratios.empty()) {
            std::sort(ratios.begin(), ratios.end());
            float r = ratios[ratios.size() / 2];
            softRatio_ = r < 0.2f ? 0.2f : (r > 1.f ? 1.f : r);
            LOGI("library soft ratio: %.2f from %zu soft/normal pairs", static_cast<double>(softRatio_),
                 ratios.size());
        }
    }

    // What getSupportedEffects reports: every waveform the ROM can ask for by id.
    std::vector<int32_t> ids() const {
        std::vector<int32_t> v;
        for (const auto& e : files_)
            if (e.first < kSoftOffset) v.push_back(e.first);
        return v;
    }

private:
    std::map<int, std::string> files_;
    float headroom_ = 1.f;
    float softRatio_ = 1.f;
    float excursionCeiling_ = 0.f;
};
Library gLibrary;

AIBinder_Class* gVibClass = nullptr;    // serves IVibrator/default
AIBinder_Class* gOplusClass = nullptr;  // serves IOplusVibrator (our extension)
AIBinder_Class* gCbClass = nullptr;     // client of IRichtapCallback
AIBinder_Class* gVibCbClass = nullptr;  // client of IVibratorCallback (on() and perform() completion)
AIBinder* gSelf = nullptr;              // our IVibrator

// ---- libbinder_ndk vendor APIs (not in the public NDK headers) -------------------------------------
struct Vndk {
    binder_exception_t (*addService)(AIBinder*, const char*);
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
           resolve(lib, "ABinderProcess_setThreadPoolMaxThreadCount", gV.setThreadPoolMaxThreadCount) &&
           resolve(lib, "ABinderProcess_startThreadPool", gV.startThreadPool) &&
           resolve(lib, "ABinderProcess_joinThreadPool", gV.joinThreadPool) &&
           resolve(lib, "AIBinder_markVintfStability", gV.markVintfStability);
}

// Temporary diagnostics. logd drops this HAL's lines once haptics get busy, which hides exactly the calls
// worth seeing, so every call is appended to a file instead. Disabled unless the directory exists:
//   mkdir -p /data/vendor/ohaptics && chmod 777 /data/vendor/ohaptics
constexpr char kTracePath[] = "/data/vendor/ohaptics/trace.log";
void traceLog(const char* fmt, ...) {
    static int fd = ::open(kTracePath, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0666);
    if (fd < 0) return;
    char buf[320];
    int head = snprintf(buf, sizeof(buf), "%lld ", static_cast<long long>(nowMs()));
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + head, sizeof(buf) - head - 2, fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    size_t len = static_cast<size_t>(head + n);
    buf[len++] = '\n';
    static std::mutex mu;
    std::lock_guard<std::mutex> l(mu);
    if (write(fd, buf, len) < 0) return;
}

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

bool softStyleActive();   // the soft (Gentle) touch style; defined with the OnePlus convention below

// ---- RichTap: completion callbacks ---------------------------------------------------------------------
// Both interfaces declare a single oneway method at transaction 1 (onCallback(int) / onComplete()), but
// they are different interfaces: the class has to match the binder or the transaction is refused.
// IVibrator's own callback is much the commoner of the two, so it is tried first: libbinder logs an error
// for every class that does not match, and the order keeps that out of the log on the hot path.
void fireCallback(AIBinder* cb) {
    bool vib = AIBinder_associateClass(cb, gVibCbClass);
    bool richtap = !vib && AIBinder_associateClass(cb, gCbClass);
    if (!vib && !richtap) {
        LOGW("completion callback is neither %s nor %s", kCbDescriptor, kVibCbDescriptor);
        return;
    }
    AParcel* in = nullptr;
    if (AIBinder_prepareTransaction(cb, &in) != STATUS_OK) return;
    if (richtap) AParcel_writeInt32(in, 0);   // IRichtapCallback.onCallback takes a status, onComplete none
    AParcel* reply = nullptr;
    AIBinder_transact(cb, CB_ON_CALLBACK, &in, &reply, FLAG_ONEWAY);
    AParcel_delete(reply);
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
            fireCallback(cb);
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

// ---- RichTap HE -> one PCM timeline ------------------------------------------------------------------
constexpr int kContinuous = 4096, kTransient = 4097;
constexpr int kMaxEventMs = 5000;
constexpr int kTransientMs = 48;
constexpr int kMaxTimelineMs = 30000;

struct Ev {
    bool cont = false;
    int t = 0, inten = 0, freq = 50, dur = 0;
    std::vector<int> pt, pi, pf;   // curve: time (rel. to event start), intensity 0..100, freq -100..100
    int end() const { return t + (cont ? dur : kTransientMs); }
};

std::mutex gLock;
int gAmp = 255, gInterval = 0, gFreq = 0;
int gPid = -1, gSeq = -1, gTotal = 0, gGot = 0;   // HE 2.0 session (packets share pid/seq)
std::vector<Ev> gPending;                        // events of a multi-packet HE 2.0 pattern
int64_t gStart = 0;

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

int patternEnd(const std::vector<Ev>& evs) {
    int end = 0;
    for (const Ev& e : evs) end = std::max(end, e.end());
    return end;
}

// RichTap event intensity (0..100, as the app/COUI computed it) -> peak sample. The scale is OnePlus
// parity: their AAC HAL receives the same numbers and plays them at full scale.
float heLevel(int inten) { return clampi(inten, 0, 100) / 100.f; }

// A continuous event's curve, sampled at `ms` into the event: intensity 0..1 interpolated between the
// control points (flat at the event intensity when the pattern carries no curve).
float curveAt(const Ev& e, float ms) {
    if (e.pt.size() < 2) return 1.f;
    if (ms <= e.pt.front()) return e.pi.front() / 100.f;
    for (size_t i = 0; i + 1 < e.pt.size(); i++) {
        if (ms > e.pt[i + 1]) continue;
        float span = static_cast<float>(e.pt[i + 1] - e.pt[i]);
        float k = span > 0 ? (ms - e.pt[i]) / span : 0.f;
        return (e.pi[i] + k * (e.pi[i + 1] - e.pi[i])) / 100.f;
    }
    return e.pi.back() / 100.f;
}

// Every RichTap event carries a frequency 0..100 - its "sharpness" - and continuous events also carry a
// curve of frequency offsets (Ev::pf). This motor is a narrow bandpass around its resonance
// (gMotor.f0(), 168.5 Hz on this phone): a carrier at f0 moves the mass hardest, and carrying an event
// away from f0 trades output for character. 50 is RichTap's neutral value.
// Returns the frequency, in Hz, to synthesise an event of RichTap frequency `freq` at.
float carrierHz(int freq) {
    // Carry everything on resonance. This is the same choice the effect library was retuned under: off
    // resonance the coil fights the spring, and on this LRA only ~8% of an off-f0 effect's energy became
    // movement. Sharpness still comes through, from the envelopes in render() rather than the carrier.
    // To trade output for more character, spread freq (50 = neutral) around f0 here.
    (void)freq;
    return gMotor.f0();
}

// Renders events into one int8 timeline. A transient is a short decaying burst; a continuous event follows
// its intensity curve. Events are summed as ENVELOPES and one continuous carrier is applied at the end:
// every event sits on this motor's resonance, so giving each its own phase let overlapping events cancel
// and beat, and put a step in the waveform wherever one began. One carrier can do neither.
std::vector<int8_t> render(const std::vector<Ev>& evs, int shiftMs) {
    int end = std::min(patternEnd(evs) - shiftMs, kMaxTimelineMs);
    if (end <= 0 || evs.empty()) return {};
    std::vector<float> env(static_cast<size_t>(kRate) * end / 1000 + 1, 0.f);
    auto samples = [](int ms) { return static_cast<size_t>(static_cast<int64_t>(kRate) * ms / 1000); };
    for (const Ev& e : evs) {
        int startMs = e.t - shiftMs;
        int lenMs = e.cont ? e.dur : kTransientMs;
        if (lenMs <= 0 || startMs + lenMs <= 0) continue;
        float level = heLevel(e.inten);
        if (e.cont && lenMs > kLongMs) level *= kLongGain;    // as the kit stages long library content
        // An event that began before this timeline starts (a late HE 2.0 packet) is played from where it
        // already is: `skip` samples in, so its envelope keeps its original timing.
        size_t skip = startMs < 0 ? samples(-startMs) : 0;
        size_t base = startMs > 0 ? samples(startMs) : 0;
        size_t n = samples(lenMs);
        for (size_t i = skip; i < n; i++) {
            size_t at = base + (i - skip);
            if (at >= env.size()) break;
            float ms = i * 1000.f / kRate;
            float a;
            if (e.cont) {
                a = curveAt(e, ms);
                a *= std::min(1.f, ms / 4.f);                             // 4 ms attack, no step
                a *= std::max(0.f, std::min(1.f, (lenMs - ms) / 6.f));    // 6 ms release
            } else {
                a = expf(-4.5f * ms / lenMs) * std::min(1.f, ms / 1.5f);  // a crisp decaying tap
            }
            env[at] += level * a;
        }
    }
    // Envelopes add where events overlap. Driving that into the clip point is distortion - odd harmonics
    // of f0, around 505 and 840 Hz, which are audible and rattle the housing - as well as the hardest the
    // motor can be driven. So the timeline is scaled down as a whole to the level the library plays at,
    // trimmed for how much longer a pattern sustains. Only ever downwards.
    float peak = 0.f;
    for (float v : env) peak = std::max(peak, v);
    float target = gLibrary.headroom() * kHeTrim;
    float norm = (peak > target && peak > 0.f) ? target / peak : 1.f;
    const float w = 2.f * static_cast<float>(M_PI) * carrierHz(50) / kRate;
    std::vector<int8_t> pcm(env.size());
    for (size_t i = 0; i < pcm.size(); i++) {
        float v = env[i] * norm * 127.f * sinf(w * static_cast<float>(i));
        pcm[i] = static_cast<int8_t>(clampi(static_cast<int>(lrintf(v)), -127, 127));
    }
    traceLog("    render %zu events %dms peak %.2f -> %.2f", evs.size(), end, static_cast<double>(peak),
             static_cast<double>(peak * norm));
    return pcm;
}

// Plays a timeline whose t=0 was `shift` ms ago; loops < 0 repeats until stop().
int64_t playTimeline(const std::vector<Ev>& evs, int shift, int loops, int interval, int amp) {
    if (evs.empty() || amp <= 0) return 0;
    std::vector<int8_t> pcm = render(evs, shift);
    if (pcm.empty()) return 0;
    float scale = clampi(amp, 0, 255) / 255.f * userIntensity();
    bool softNow = softStyleActive();
    // Patterns have no soft variant to switch to, so the style has to be applied as level here, or Gentle
    // does nothing for the COUI sliders and the A-Z index.
    if (softNow) scale *= gLibrary.softRatio() * kSoftTrim;
    traceLog("    he gain=%.2f (amp %d x intensity %.2f soft=%d) -> samples x%.3f", static_cast<double>(scale),
             amp, static_cast<double>(userIntensity()), softNow ? 1 : 0,
             static_cast<double>(scale * kPatternScale));
    scaleSamples(pcm, scale * kPatternScale);
    gMotor.play(pcm, loops, interval);
    int64_t len = msOf(pcm.size());
    if (loops < 0) return INT64_MAX / 4;
    return static_cast<int64_t>(loops) * len + static_cast<int64_t>(loops - 1) * interval;
}

void stopAll() {
    {
        std::lock_guard<std::mutex> g(gLock);
        gPid = gSeq = -1;
        gPending.clear();
    }
    gMotor.stop();
    gTimer.flush();
}

void performHe(int looper, int interval, int amplitude, int freq, const std::vector<int32_t>& he, AIBinder* cb) {
    // HapticPlayer.getRealLooper(): -1 loops until stop(), 0 plays once
    int loops = looper == -1 ? -1 : (looper < 0 ? 0 : std::max(1, looper));
    int64_t now = nowMs();
    if (he.empty() || loops == 0) {
        gTimer.at(now, cb);
        return;
    }
    std::vector<Ev> raw;
    if (he[0] == 2) parseHe2(he, raw);
    else parseHe1(he, raw);

    int amp, ival, shift = 0;
    std::vector<Ev> evs;
    {
        std::lock_guard<std::mutex> g(gLock);
        // RichTap sends 0 for "no override": play the pattern at its own event intensities. Reading it as
        // an amplitude of zero silences the whole pattern, which is why some gestures had no haptics.
        gAmp = amp = amplitude > 0 ? clampi(amplitude, 0, 255) : 255;
        gInterval = ival = std::max(0, interval);
        gFreq = freq;
        if (he[0] == 2 && he.size() >= 5) {
            int pid = he[2], seq = he[3], total = he[4] & 0xffff;
            if (pid != gPid || seq != gSeq || gGot >= gTotal) {   // a new pattern
                gPid = pid;
                gSeq = seq;
                gTotal = std::max(1, total);
                gGot = 0;
                gStart = now;
                gPending.clear();
            }
            gGot += std::max(1, static_cast<int>(static_cast<uint32_t>(he[4]) >> 16));
            shift = static_cast<int>(now - gStart);
            // Later packets of one pattern extend it: the whole timeline is re-rendered from gStart, so
            // events already played are skipped and the ones still ahead keep their original timing.
            gPending.insert(gPending.end(), raw.begin(), raw.end());
            evs = gPending;
            if (gTotal > 1 && loops != 1) loops = 1;
        } else {
            gPid = gSeq = -1;
            gPending.clear();
            evs = raw;
        }
    }
    std::stable_sort(evs.begin(), evs.end(), [](const Ev& a, const Ev& b) { return a.t < b.t; });
    int64_t len = playTimeline(evs, shift, loops, ival, amp);
    if (loops >= 0) gTimer.at(now + len, cb);
    traceLog("  he%d events=%zu->%zu loops=%d amp=%d(asked %d) len=%lldms", he[0] == 2 ? 2 : 1,
             raw.size(), evs.size(), loops, amp, amplitude, static_cast<long long>(len));
    LOGD("he%d: %zu events (%zu total), loops %d, amp %d (asked %d), %lld ms", he[0] == 2 ? 2 : 1, raw.size(),
         evs.size(), loops, amp, amplitude, static_cast<long long>(len));
}

void playOnce(std::vector<Ev> evs, AIBinder* cb) {
    int amp;
    {
        std::lock_guard<std::mutex> g(gLock);
        amp = gAmp;
    }
    int64_t now = nowMs();
    std::stable_sort(evs.begin(), evs.end(), [](const Ev& a, const Ev& b) { return a.t < b.t; });
    int64_t len = playTimeline(evs, 0, 1, 0, amp);
    gTimer.at(now + len, cb);
}

// ---- OnePlus calling convention on IVibrator ----------------------------------------------------------
float durationTrim(int ms) {
    if (ms <= kTickMs) return 1.f;
    float t = powf(static_cast<float>(kTickMs) / static_cast<float>(ms), kDurTrimExp);
    return t < kDurTrimFloor ? kDurTrimFloor : t;
}

std::mutex gStyleLock;
float gPendingGain = -1.f;   // from setAmplitude(1 + g), consumed by the next perform()
float gAmplitude = 1.f;      // a plain amplitude, for on()
bool gSoftStyle = false;
int gStyleSeen = -1;      // the stored setting last adopted

// The touch style in force. setAmplitude(10000|10001) updates it the moment the user changes it; whenever the
// stored setting itself changes (and at start-up), that wins - so this can never drift from what the user set.
bool softStyleActive() {
    int stored = readSettings().style;   // outside gStyleLock: readSettings has its own
    std::lock_guard<std::mutex> g(gStyleLock);
    if (stored >= 0 && stored != gStyleSeen) {
        gStyleSeen = stored;
        gSoftStyle = stored == 1;
    }
    return gSoftStyle;
}

binder_status_t vibSetAmplitude(const AParcel* in, AParcel* out) {
    float a = 0;
    AParcel_readFloat(in, &a);
    traceLog("  setAmplitude %.4f", static_cast<double>(a));
    std::lock_guard<std::mutex> g(gStyleLock);
    if (a <= 1.0f) {
        gAmplitude = a < 0.f ? 0.f : a;           // a plain amplitude, for on() and waveforms
        // Applied live for on()/waveforms, but never under a stream: that would quantise it (kEffectScale).
        if (!gMotor.streaming()) gMotor.setGain(gAmplitude);
    } else if (a == 10000.f || a == 10001.f) {
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
    bool soft = softStyleActive();
    float gain;
    {
        std::lock_guard<std::mutex> g(gStyleLock);
        gain = gPendingGain;
        gPendingGain = -1.f;
        if (gain < 0.f) gain = gAmplitude;
    }
    int id = effect;
    bool playedSoft = soft && gLibrary.has(id + kSoftOffset);
    if (playedSoft) id += kSoftOffset;
    // Gentle has to apply to everything the UI plays, not just the ids the kit built a soft waveform for -
    // the recents scroll and some slider ticks have none, which is why they stayed strong on Gentle.
    else if (soft) gain *= gLibrary.softRatio();
    std::vector<int8_t> pcm = gLibrary.load(id);
    traceLog("  perform effect=%d -> id=%d strength=%d gain=%.2f soft=%d hasSoft=%d bytes=%zu", effect, id,
             static_cast<int>(strength), static_cast<double>(gain), soft ? 1 : 0,
             playedSoft ? 1 : 0, pcm.size());
    traceLog("    durationTrim(%d ms) = %.2f", msOf(pcm.size()),
             static_cast<double>(durationTrim(msOf(pcm.size()))));
    if (pcm.empty()) {
        LOGE("no waveform for effect %d", id);
        if (cb) AIBinder_decStrong(cb);
        return exception(out, EX_UNSUPPORTED_OPERATION);
    }
    int ms = msOf(pcm.size());
    gain *= durationTrim(ms);   // a long effect at tick amplitude feels much stronger than the tick
    // No intensity factor here: the framework already applied the user's slider to this effect's strength,
    // and it arrived as setAmplitude(1 + strength/2400). Multiplying by it again squares it.
    float want = gain * (soft ? kGentleEffectScale : kEffectScale);
    const std::vector<int8_t> authored = pcm;
    float applied = 0.f;
    float knee = shapeEffect(pcm, want, &applied);
    // Above its authored level a short effect may not swing the mass past the library's strongest tap (see
    // kMotorQ). The largest boost that stays inside is found by bisection; an effect that is already past it
    // as authored still plays as authored, as Xiaomi's HAL played it.
    float ceiling = gLibrary.excursionCeiling();
    float limited = want;
    if (want > 1.f && ms <= kKneeMaxMs && ceiling > 0.f && excursionOf(pcm, gMotor.f0()) > ceiling) {
        float lo = 1.f, hi = want;
        for (int i = 0; i < 8; ++i) {
            float mid = 0.5f * (lo + hi);
            std::vector<int8_t> trial = authored;
            float a = 0.f;
            shapeEffect(trial, mid, &a);
            if (excursionOf(trial, gMotor.f0()) > ceiling)
                hi = mid;
            else
                lo = mid;
        }
        limited = lo;
        pcm = authored;
        knee = shapeEffect(pcm, limited, &applied);
    }
    traceLog("    knee x%.2f (%d ms), boost x%.3f of x%.3f asked", static_cast<double>(knee), ms,
             static_cast<double>(limited), static_cast<double>(want));
    traceLog("    final gain=%.2f (durationTrim %.2f) -> samples x%.3f (asked x%.3f)", static_cast<double>(gain),
             static_cast<double>(durationTrim(ms)), static_cast<double>(applied), static_cast<double>(want));
    gMotor.play(pcm, 1, 0);
    gTimer.at(nowMs() + ms, cb);
    if (cb) AIBinder_decStrong(cb);
    ok(out);
    return AParcel_writeInt32(out, ms);
}

binder_status_t onVibTransact(AIBinder*, transaction_code_t code, const AParcel* in, AParcel* out) {
    traceLog("IVibrator tx=%u", code);
    switch (code) {
        case VIB_GET_CAPABILITIES:
            ok(out);
            return AParcel_writeInt32(out, kCapabilities);
        case VIB_OFF:
            stopAll();
            return ok(out);
        case VIB_ON: {
            int32_t ms = 0;
            AIBinder* cb = nullptr;
            AParcel_readInt32(in, &ms);
            AParcel_readStrongBinder(in, &cb);
            {
                std::lock_guard<std::mutex> g(gStyleLock);
                gMotor.setGain(gAmplitude);
            }
            gMotor.buzz(ms);
            gTimer.at(nowMs() + ms, cb);
            if (cb) AIBinder_decStrong(cb);
            return ok(out);
        }
        case VIB_PERFORM:
            return vibPerform(in, out);
        case VIB_GET_SUPPORTED_EFFECTS: {
            std::vector<int32_t> ids = gLibrary.ids();
            ok(out);
            return AParcel_writeInt32Array(out, ids.data(), static_cast<int32_t>(ids.size()));
        }
        case VIB_SET_AMPLITUDE:
            return vibSetAmplitude(in, out);
        case GET_INTERFACE_VERSION:
            ok(out);
            return AParcel_writeInt32(out, kVibVersion);
        case GET_INTERFACE_HASH:
            ok(out);
            return AParcel_writeString(out, kVibHash, static_cast<int32_t>(strlen(kVibHash)));
        default:
            // external control, compositions, always-on, PWLE and the frequency queries: this motor has
            // none of them, and the framework falls back to effects and amplitude, as it did on Xiaomi's HAL.
            return exception(out, EX_UNSUPPORTED_OPERATION);
    }
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
    traceLog("IOplusVibrator tx=%u", code);
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
            {
                // All three zero is the framework's "no override" (RichtapPatternHeParameter with
                // mInterval/mAmplitude/mFreq = 0), sent alongside a vibration - not a stop. Stopping is
                // STOP and OFF below, which the player uses for that.
                std::lock_guard<std::mutex> g(gLock);
                gInterval = std::max(0, a);
                gAmp = b > 0 ? clampi(b, 0, 255) : 255;
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
                gAmp = clampi(a, 0, 255);   // applied to the samples of the next pattern
            }
            gTimer.at(nowMs(), cb);
            release(cb);
            return STATUS_OK;
        }
        case ON: {   // oneway: a plain buzz of timeoutMs
            AParcel_readInt32(in, &a);
            cb = readCb(in);
            int amp;
            {
                std::lock_guard<std::mutex> g(gLock);
                amp = gAmp;
            }
            gMotor.setGain(clampi(amp, 0, 255) / 255.f);
            gMotor.buzz(a);
            gTimer.at(nowMs() + a, cb);
            release(cb);
            return STATUS_OK;
        }
        case PERFORM: {   // RichTap ext-prebaked: a transient at that strength
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
            return AParcel_writeInt32(out, kTransientMs);
        }
        case PERFORM_ENVELOPE: {   // oneway: 4 x (relativeTime, scale, freq) = one continuous event
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

}  // namespace

int main() {
    if (!loadVndk()) return 1;
    gV.setThreadPoolMaxThreadCount(4);
    gV.startThreadPool();

    gVibClass = AIBinder_Class_define(kVibDescriptor, onCreate, onDestroy, onVibTransact);
    gOplusClass = AIBinder_Class_define(kOplusDescriptor, onCreate, onDestroy, onOplusTransact);
    gCbClass = AIBinder_Class_define(kCbDescriptor, onCreate, onDestroy, onClientTransact);
    gVibCbClass = AIBinder_Class_define(kVibCbDescriptor, onCreate, onDestroy, onClientTransact);

    gLibrary.scan();
    gLibrary.measureHeadroom();
    LOGI("touch style from settings: %s", softStyleActive() ? "soft (gentle)" : "default (crisp)");
    if (!gMotor.start()) LOGE("no motor: registering anyway so the phone boots, but nothing will vibrate");
    gLibrary.measureExcursion(gMotor.f0());   // after start(): f0 is the motor's calibrated one

    gSelf = AIBinder_new(gVibClass, nullptr);
    AIBinder* oplus = AIBinder_new(gOplusClass, nullptr);
    gV.markVintfStability(gSelf);   // system_server only uses vendor binders with VINTF stability
    gV.markVintfStability(oplus);
    if (AIBinder_setExtension(gSelf, oplus) != STATUS_OK) return 1;

    binder_exception_t ex = gV.addService(gSelf, kVibName);
    if (ex != EX_NONE) {
        LOGE("addService(%s) failed: %d", kVibName, ex);
        return 1;
    }
    // The literal below is how apply.sh recognises this binary at Xiaomi's path; keep it in step with
    // SVC_MARKER in tools/apply_template.sh.
    LOGI("%s: O-Haptics vibrator HAL (IVibrator V%d + %s V%d) on the si_haptic motor", kVibName, kVibVersion,
         kOplusDescriptor, kOplusVersion);
    gV.joinThreadPool();
    return 0;
}
