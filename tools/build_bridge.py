#!/usr/bin/env python3
"""
O-Haptics bridge builder for OnePlus/OxygenOS ports on POCO F7 (onyx).

Produces a partition-ready tree (no module) from the OnePlus 13 odm effect library + the device's OPlus configs:
  out/rom/odm/bin/hw/vendor.xiaomi.hardware.vibratorfeature.service   OnePlus vibrator service (tools/oplus_vibrator)
                                 under Xiaomi's name: IVibrator/default + IOplusVibrator (RichTap)
  out/rom/odm/bin/hw/vendor.xiaomi.hardware.vibratorfeature.stock     Xiaomi's HAL, unchanged bytes, renamed
  out/rom/odm/etc/init/vendor.xiaomi.hardware.vibratorfeature.service.rc   Xiaomi's rc, service block replaced by
                                 vibratorfeature-hal-stock (Xiaomi) + vibratorfeature-hal-service (OnePlus)
  out/rom/stock/odm/...           Xiaomi's original HAL, rc and VINTF manifest, for apply.sh --revert
  out/rom/odm/firmware/          effect library keyed by *OPlus waveform id*, replacing Xiaomi's *_RTP.bin files
                                 <id>_op<id>_RTP.bin           default touch style
                                 <1000+id>_opsoft<id>_RTP.bin  soft touch style (only ids with a soft variant)
                                 <id>_rt<name>_RTP.bin         ringtone-synced tracks (see below)
                                 21_op21_RTP.bin               AOSP TEXTURE_TICK (= OnePlus wf 7 scroll tick)
  out/rom/my_product/etc/vibrator/effect_waveform.xml             dead waveform ids remapped
  out/rom/config/, out/rom/lists/, out/rom/apply.sh, out/rom/ROM_INTEGRATION.md
                                 (apply.sh also drops oplus.software.audio.haptic_channel_support and adds
                                 oplus.software.vibrator_luxunvibrator in my_product)
  out/OHaptics_onyx_partitions.zip   the same tree, to unpack and apply on the phone (ROM_INTEGRATION.md)
  out/remap_report.csv           every remapped entry

Both sides use raw signed 8-bit PCM at 24 kHz (OnePlus vibrator_effect.json play_rate_hz=24000,
Xiaomi effect_stream default play rate 0x5dc0), so waveforms are copied byte-for-byte.

"Vibrate in sync with ringtone" (ringtone vibrate type 64): the framework maps the ringtone file name
through <ringtone_effect> to a per-ringtone waveform id. OnePlus ships those haptics as the audio-coupled
haptic channel of the ringtone OGGs (ANDROID_HAPTIC=1). tools/hapticextract decodes that channel on the
phone; here it is band-limited, frequency-shifted from OnePlus' ~125.7 Hz LRA to this motor's f0 (an
analytic-signal shift keeps every envelope and timing sample-exact), and decimated 48k -> 24k.
"""
import argparse
import csv
import json
import hashlib
import math
import os
import re
import shutil
import subprocess
import sys
import zipfile

import numpy as np


HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

RATE = 24000
TRACK_RATE = 48000
HAL_NAME = "vendor.xiaomi.hardware.vibratorfeature.service"
SVC_DIR = os.path.join(ROOT, "tools", "vibratorfeature")
SVC_SRC = os.path.join(SVC_DIR, "vibratorfeature_service.cpp")
# One HAL, in place of Xiaomi's: it drives the motor itself, so Xiaomi's init rc and VINTF manifest stay
# byte-for-byte stock - the rc only names this path and the manifest only declares the interface it serves.
SVC_REL = "odm/bin/hw/" + HAL_NAME                                             # service vibratorfeature-hal-service
XM_RC_REL = "odm/etc/init/vendor.xiaomi.hardware.vibratorfeature.service.rc"   # stock
XM_VINTF_REL = "odm/etc/vintf/manifest/vendor.xiaomi.hardware.vibratorfeature.service.xml"   # stock
OLD_FILES = ["odm/bin/hw/vendor.oplus.hardware.vibrator-service",              # earlier kits: removed
             "odm/etc/init/vendor.oplus.hardware.vibrator-service.rc",
             "odm/bin/hw/vendor.xiaomi.hardware.vibratorfeature.stock",
             "odm/lib64/libohaptics_richtap.so", "odm/etc/vintf/manifest/vibrator-default.xml"]
# The soft touch style plays <id + SOFT_OFFSET>
SOFT_IDS = (0, 1, 2, 3, 4, 6, 7, 46, 47, 54, 105, 106, 107, 109, 309, 310, 315, 316, 318, 362, 363, 364)
SOFT_OFFSET = 1000
NDK = os.path.expanduser(r"~/AppData/Local/Android/Sdk/ndk/26.3.11579264")

# --- substitute pools for waveform ids the OnePlus 13 library does not ship -------------
# Chosen by nearest duration (log scale) inside a category pool.
POOL_TOUCH_MID = [315, 318, 122, 54, 56, 364, 363, 47, 46]
POOL_TOUCH_LONG = [118, 108, 43, 45, 44]
POOL_GAME = [61, 100, 101, 62, 63, 102, 66, 65, 67, 103, 68, 104, 64, 69, 118, 43, 45, 44]
POOL_NOTIFICATION = [369, 367, 370, 368, 366, 170, 42, 41, 43, 45, 44]
POOL_RINGTONE = [42, 41, 43, 45, 44]
# waveforms that are a plain strong buzz of a fixed length: synthesized at the motor f0
SYNTH_BUZZ = {57: 400, 58: 1000}
AOSP_TEXTURE_TICK = 21     # IVibrator Effect.TEXTURE_TICK: reaches the HAL as perform(21), untranslated
OP_TEXTURE_WF = 7          # OnePlus "weak granular" tick: recents page scroll, COUI list scroll (307/14)


def parse_args():
    p = argparse.ArgumentParser()
    p.add_argument("--op-vibrator", default=r"C:/Users/Nischay_12089/Desktop/MIO-KITCHEN-4.2.1-win/oneplus13/odm/etc/vibrator")
    p.add_argument("--xml", default=os.path.join(ROOT, "work/device/my_product/vibrator/effect_waveform.xml"))
    p.add_argument("--stock-hal", default=os.path.join(ROOT, "work/device", HAL_NAME))
    p.add_argument("--f0", type=float, default=168.7, help="LRA resonant frequency (sysfs f0 1687 -> 168.7 Hz)")
    p.add_argument("--out", default=os.path.join(ROOT, "out"))
    p.add_argument("--haptic-tracks", default=os.path.join(ROOT, "work/haptic_raw"),
                   help="<ringtone name>.s16 files from tools/hapticextract (48 kHz haptic channel)")
    p.add_argument("--track-src-f0", type=float, default=125.7,
                   help="carrier the OnePlus haptic tracks were authored for (median spectral centroid)")
    p.add_argument("--no-track-retune", action="store_true", help="keep ringtone tracks at their original carrier")
    p.add_argument("--dedicated-track-ids", action="store_true",
                   help="give each decoded ringtone its own waveform, including ROMs whose current XML shares fallback ids")
    p.add_argument("--retune-waveform", type=int, action="append", default=[],
                   help="also retune this UI waveform beyond retune-max-ms, preserving its timing and peak")
    p.add_argument("--no-retune-library", action="store_true",
                   help="keep the OnePlus library at its authored carrier instead of moving each effect's "
                        "energy onto this motor's f0")
    p.add_argument("--retune-max-ms", type=float, default=300.0,
                   help="only retune effects up to this length; longer patterns (ringtone, notification, charge "
                        "animation, game assistant) keep OnePlus' waveform exactly")
    p.add_argument("--min-cycles", type=float, default=2.0,
                   help="re-synthesise UI ticks shorter than this many periods at f0 (0 disables); "
                        "effect 105 (slider step) is only 0.9 cycles as shipped")
    p.add_argument("--ui-gain", type=float, default=1.25,
                   help="amplitude multiplier for short effects / UI ticks (1.0 = as OnePlus authored; the retune "
                        "already raises their real output a lot)")
    p.add_argument("--track-gain", type=float, default=0.6,
                   help="amplitude for the audio-synced ringtone tracks. They are always shifted onto f0 (that is what "
                        "makes sync work), so they need their own, lower level than the library patterns")
    p.add_argument("--long-gain", type=float, default=0.8,
                   help="amplitude multiplier for long patterns (ringtones, alarms, game assistant), which feel much "
                        "stronger than ticks once they sit on resonance")
    p.add_argument("--long-ms", type=float, default=300.0,
                   help="effects longer than this use --long-gain")
    p.add_argument("--texture-tick-peak", type=float, default=64.0,
                   help="peak (of 127, at max intensity) of the scroll/texture tick shared by OnePlus wf %d and "
                        "AOSP TEXTURE_TICK; tap is 93, slider step 42" % OP_TEXTURE_WF)
    p.add_argument("--master-gain", type=float, default=1.0,
                   help="scales the whole library, i.e. how strong the haptics are at maximum intensity "
                        "(0.75 = 75%% of the un-scaled build)")
    p.add_argument("--stock-rtp-list", default=os.path.join(ROOT, "work/device/odm_firmware/rtp_list.txt"),
                   help="names of Xiaomi's stock *_RTP.bin files in /odm/firmware (removed from the image)")
    p.add_argument("--xiaomi-strength-curve", action="store_true",
                   help="use Xiaomi's 0.5+0.5*g curve (ro.odm.mm.vibrator.scaleStreaming=true): louder for events that "
                        "ask for less than full strength, but the intensity slider then never goes below 50%%. "
                        "Default is linear, so Settings' vibration intensity spans 0-100%%")
    return p.parse_args()


def load_op13_library(vib_dir):
    cfg = json.load(open(os.path.join(vib_dir, "vibrator_effect.json")))
    # single motor table ("9999" on OP13); take the first one
    table = next(iter(cfg.values()))
    lib = {}
    for style_key, style in (("def_style", "def"), ("soft_style", "soft")):
        for e in table.get(style_key, []):
            fname = os.path.basename(e["effect_file"])
            path = os.path.join(vib_dir, next(iter(cfg.keys())), style, fname)
            pcm = resample_to_24k(open(path, "rb").read(), int(e.get("play_rate_hz", RATE)))
            lib.setdefault(style, {})[int(e["effect_id"])] = pcm
    return lib["def"], lib.get("soft", {})


def resample_to_24k(pcm, rate):
    # Xiaomi's stream path writes samples straight into the SI driver's custom_wave FIFO,
    # so everything must already be at the driver rate. (OP13 ships effect_56 at 8 kHz.)
    if rate == RATE:
        return pcm
    s = [b - 256 if b > 127 else b for b in pcm]
    n_out = int(len(s) * RATE / rate)
    out = bytearray()
    for i in range(n_out):
        x = i * rate / RATE
        j = int(x)
        fr = x - j
        a = s[j]
        b = s[j + 1] if j + 1 < len(s) else s[j]
        out.append(int(round(a + (b - a) * fr)) & 0xFF)
    return bytes(out)


def synth_buzz(ms, f0):
    n = int(RATE * ms / 1000)
    ramp = int(RATE / f0)          # one period attack / release
    out = bytearray()
    for i in range(n):
        env = min(1.0, i / ramp, (n - 1 - i) / ramp)
        v = int(round(127 * env * math.sin(2 * math.pi * f0 * i / RATE)))
        out.append(v & 0xFF)
    return bytes(out)


def pcm_ms(b):
    return len(b) * 1000.0 / RATE


def apply_gain(pcm, gain, allow_waveshape=True):
    """Make every effect in the library stronger than OnePlus authored it.

    The HAL clamps the runtime gain at 1.0 (Xiaomi's getAmplitudeFromEffectStrength), so with the
    intensity slider already at max the only remaining headroom is the sample data itself. OnePlus'
    waveforms peak around 87% of full scale (median; some are much quieter), so there is room - but
    going over +-127 wraps/clips and turns a clean LRA pulse into a buzz.
    """
    if gain == 1.0:
        return pcm
    x = np.frombuffer(pcm, dtype=np.int8).astype(np.float32)
    if gain < 1.0 or not allow_waveshape:
        # Plain scaling. A soft knee is a waveshaper: it manufactures harmonics (169 -> 338, 507 Hz...),
        # which is inaudible on a 12 ms tick but a continuous buzz on a 16 s ringtone track. Long
        # patterns and ringtone tracks therefore never get waveshaped, and must not clip either.
        y = x * gain
    else:
        # Normalised soft knee: slope gain/tanh(gain) near zero (quiet effects get the full boost),
        # full scale still maps to full scale, nothing clips. Plain tanh() would *shrink* mid-level
        # samples (x=100, gain 1.25 -> 96); note the same normalisation would *raise* samples for
        # gain < 1, which is why reductions take the branch above.
        y = 127.0 * np.tanh(gain * x / 127.0) / math.tanh(gain)
    return np.clip(np.round(y), -127, 127).astype(np.int8).tobytes()


def centroid_hz(x, lo=40.0, hi=600.0):
    """energy-weighted centre frequency of an effect (24 kHz int8 samples as float)"""
    n = max(len(x) * 4, 4096)
    X = np.abs(np.fft.rfft(x, n))
    fr = np.fft.rfftfreq(n, 1.0 / RATE)
    m = (fr > lo) & (fr < hi)
    p = X[m] ** 2
    return float((fr[m] * p).sum() / max(p.sum(), 1e-9))


def energy_at(x, f0, width=0.15):
    """share of the 40-600 Hz energy that lands within +-width of f0 (what the LRA can convert)"""
    n = max(len(x) * 4, 4096)
    X = np.abs(np.fft.rfft(x, n)) ** 2
    fr = np.fft.rfftfreq(n, 1.0 / RATE)
    tot = X[(fr > 40) & (fr < 600)].sum()
    return float(X[(fr > f0 * (1 - width)) & (fr < f0 * (1 + width))].sum() / max(tot, 1e-9))


def freq_shift(x, shift, rate=RATE, lo=20.0, hi=1000.0):
    """single-sideband shift: moves the carrier, keeps envelope and timing sample-exact"""
    if abs(shift) < 0.5:
        return x
    pad = np.zeros(len(x) * 3, dtype=float)
    xx = np.concatenate([x, pad])
    f = np.fft.fftfreq(len(xx), 1.0 / rate)
    h = np.where((f >= lo) & (f <= hi), 2.0, 0.0)
    a = np.fft.ifft(np.fft.fft(xx) * h)
    t = np.arange(len(xx)) / rate
    return np.real(a * np.exp(2j * np.pi * shift * t))[:len(x)]


def synth_tick(peak, cycles, f0):
    """a crisp resonant pulse: fast attack, decay to zero, `cycles` periods at f0"""
    n = max(8, int(round(RATE * cycles / f0)))
    t = np.arange(n) / RATE
    attack = np.clip(t * f0 * 5.0, 0.0, 1.0)            # ~1/5 cycle rise
    decay = np.clip((n - 1 - np.arange(n)) / (n * 0.6), 0.0, 1.0)
    return peak * attack * decay * np.sin(2 * np.pi * f0 * t)


def retune_effect(pcm, f0, min_cycles):
    """Put an effect's energy on the motor's resonance.

    OnePlus authored these for their own LRA: the long effects sit at 125-160 Hz and the short UI ticks
    are fast transients centred at 230-330 Hz. This motor is a narrow bandpass at f0, so most of that
    energy never becomes movement. Shifting the carrier (not resampling) keeps envelope and duration.
    Pulses too short to complete min_cycles periods at f0 are re-synthesised at the same peak level.
    """
    x = np.frombuffer(pcm, dtype=np.int8).astype(float)
    if len(x) < 8:
        return pcm
    peak = float(np.abs(x).max())
    if peak < 1:
        return pcm
    want = RATE * min_cycles / f0
    if min_cycles > 0 and len(x) < want:
        y = synth_tick(peak, min_cycles, f0)
    else:
        y = freq_shift(x, f0 - centroid_hz(x))
        y *= peak / max(np.abs(y).max(), 1e-9)          # the shift changes peak; restore it
        # a few effects carry two components, where centring the centroid moves the dominant one off
        # resonance - keep whichever version actually has more energy at f0
        if energy_at(y, f0) <= energy_at(x, f0):
            return pcm
    return np.clip(np.round(y), -127, 127).astype(np.int8).tobytes()


def convert_haptic_track(path, shift_hz):
    x = np.fromfile(path, dtype="<i2").astype(np.float64)
    n = len(x)
    f = np.fft.fftfreq(n, 1.0 / TRACK_RATE)
    # analytic signal of the 20 Hz .. 1 kHz band (an LRA cannot render anything outside it)
    h = np.where((f >= 20.0) & (f <= 1000.0), 2.0, 0.0)
    a = np.fft.ifft(np.fft.fft(x) * h)
    t = np.arange(n) / TRACK_RATE
    y = np.real(a * np.exp(2j * np.pi * shift_hz * t))[::2]   # band-limited, so plain decimation is alias-free
    return np.clip(np.round(y / 256.0), -127, 127).astype(np.int8).tobytes()


def load_ringtone_tracks(xml_text, track_dir, shift_hz):
    """-> {waveform_id: (pcm, ringtone_name)} for every extracted track listed in <ringtone_effect>"""
    if not os.path.isdir(track_dir):
        return {}
    effect_wf = {m.group(1): int(m.group(2)) for m in
                 re.finditer(r'<effect effect_id="(\d+)" waveform_id="(\d+)"', xml_text)}
    name_wf = {m.group(1): effect_wf[m.group(2)] for m in
               re.finditer(r'<ringtone_effect ringtone_name="([^"]+)" effect_id="(\d+)"', xml_text)}
    tracks = {}
    for fn in sorted(os.listdir(track_dir)):
        name = fn[:-4]
        if not fn.endswith(".s16") or name not in name_wf:
            continue
        wf = name_wf[name]
        if wf in tracks:
            sys.exit("two ringtones share waveform id %d" % wf)
        tracks[wf] = (convert_haptic_track(os.path.join(track_dir, fn), shift_hz), name)
    return tracks


def dedicated_track_ids(xml_text, track_dir):
    """Keep framework effect ids/names, but never reuse a generic fallback for synced audio."""
    names = {fn[:-4] for fn in os.listdir(track_dir) if fn.endswith(".s16")}
    taken = {int(v) for v in re.findall(r'waveform_id="(\d+)"', xml_text)}
    effects = {}
    next_id = 2000
    for name, effect in re.findall(r'<ringtone_effect ringtone_name="([^"]+)" effect_id="(\d+)"', xml_text):
        if name not in names:
            continue
        if effect in effects:
            raise ValueError("multiple decoded ringtones share effect id " + effect)
        while next_id in taken:
            next_id += 1
        effects[effect] = next_id
        taken.add(next_id)
        next_id += 1

    def replace(m):
        tag = m.group(0)
        attrs = dict(re.findall(r'(\w+)="([^"]*)"', tag))
        if attrs.get("effect_id") in effects:
            tag = re.sub(r'waveform_id="\d+"', 'waveform_id="%d"' % effects[attrs["effect_id"]], tag)
        return tag
    return re.sub(r"<effect [^>]*/>", replace, xml_text)


def nearest(pool, lib, dur):
    dur = max(dur, 1)
    return min((w for w in pool if w in lib), key=lambda w: abs(math.log(max(pcm_ms(lib[w]), 1) / dur)))


def choose_substitute(attrs, lib):
    desc = attrs["desc"].lower()
    usage = attrs["usage"].upper()
    dur = int(attrs["duration"])
    if usage == "TOUCH" or usage == "UNKNOWN":
        tier = re.search(r"(normal|double|triple|quad|penta)_kill|game_(start|victory|failed)", desc)
        if tier:
            key = tier.group(1) or tier.group(2)
            # OnePlus' own afgame kill-streak set (100..104) + sgame highlights
            return dict(normal=100, double=101, triple=102, quad=103, penta=104,
                        start=69, victory=68, failed=62)[key], "game-tier"
        if re.search(r"game", desc):
            return nearest(POOL_GAME, lib, dur), "game"
        if dur <= 60:
            if re.search(r"big|strong|heavy|half_lap", desc):
                return 6, "touch-short-strong"
            if re.search(r"small|weak|light", desc):
                return 7, "touch-short-weak"
            return 2, "touch-short"
        if dur <= 1200:
            return nearest(POOL_TOUCH_MID, lib, dur), "touch-mid"
        return nearest(POOL_TOUCH_LONG, lib, dur), "touch-long"
    if usage == "NOTIFICATION":
        return nearest(POOL_NOTIFICATION, lib, dur), "notification"
    return nearest(POOL_RINGTONE, lib, dur), "ringtone/alarm (repeats)"


def remap_xml(xml_text, lib, track_wfs=()):
    report = []

    def fix(m):
        tag = m.group(0)
        attrs = dict(re.findall(r'(\w+)="([^"]*)"', tag))
        wf = int(attrs["waveform_id"])
        if wf in track_wfs:
            # play window == track length, so a looping ringtone and its haptics restart together
            return re.sub(r'duration="\d+"', 'duration="%d"' % int(round(pcm_ms(lib[wf]))), tag)
        if wf in lib:
            return tag
        new_wf, why = choose_substitute(attrs, lib)
        pcm = pcm_ms(lib[new_wf])
        old_dur = int(attrs["duration"])
        if why.startswith("touch-short"):
            new_dur = max(old_dur, int(round(pcm)) + 20)
        else:
            new_dur = int(round(pcm)) + 55
        report.append(dict(effect_id=attrs["effect_id"], desc=attrs["desc"], usage=attrs["usage"],
                           old_waveform=wf, old_duration=old_dur, new_waveform=new_wf,
                           new_duration=new_dur, rule=why))
        tag = re.sub(r'waveform_id="\d+"', 'waveform_id="%d"' % new_wf, tag)
        tag = re.sub(r'duration="\d+"', 'duration="%d"' % new_dur, tag)
        return tag

    fixed = re.sub(r"<effect [^>]*/>", fix, xml_text)
    return fixed, report


def build_service(out_path):
    """NDK build of the O-Haptics vibrator HAL: arm64 PIE, API 33, static libc++."""
    src = open(SVC_SRC, encoding="utf-8").read()
    if "kSoftOffset = %d;" % SOFT_OFFSET not in src:
        sys.exit("kSoftOffset in %s differs from SOFT_OFFSET" % SVC_SRC)
    tc = os.path.join(NDK, "toolchains", "llvm", "prebuilt", "windows-x86_64", "bin")
    cxx = os.path.join(tc, "clang++.exe")
    if not os.path.exists(cxx):
        sys.exit("Android NDK not found at %s (needed for %s)" % (NDK, SVC_SRC))
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    subprocess.check_call([cxx, "--target=aarch64-linux-android33", "-std=c++17", "-O2", "-fPIE", "-pie",
                           "-Wall", "-Wextra", "-Wno-unused-parameter", "-static-libstdc++",
                           "-Wl,--gc-sections", "-Wl,-z,max-page-size=16384", "-Wl,--no-undefined", "-s",
                           "-o", out_path, SVC_SRC, "-lbinder_ndk", "-llog", "-ldl"])


def write(path, data, mode="wb"):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, mode) as f:
        f.write(data)


SCALE_PROP_KEY = "ro.odm.mm.vibrator.scaleStreaming"
# a stock Xiaomi effect file: <id>_<name>_[<n>KHz_][S|P_]RTP.bin (one ships as ..._Rtp.bin)
STOCK_RTP = re.compile(r"^\d+_[^_]+_(.*_)?RTP\.bin$", re.IGNORECASE)


APPLY_SH = open(os.path.join(HERE, "apply_template.sh"), encoding="utf-8").read()


def main():
    a = parse_args()
    out = a.out
    for sub in ("ohap", "xml", "module", "hal", "rom"):
        shutil.rmtree(os.path.join(out, sub), ignore_errors=True)
    for stale in ("OHapticsBridge-onyx.zip",):
        if os.path.exists(os.path.join(out, stale)):
            os.remove(os.path.join(out, stale))
    rom = os.path.join(out, "rom")

    # ---- 1. effect library -> odm/firmware ----------------------------------------------
    lib_def, lib_soft = load_op13_library(a.op_vibrator)
    for wf, ms in SYNTH_BUZZ.items():
        if wf not in lib_def:
            lib_def[wf] = synth_buzz(ms, a.f0)
    if AOSP_TEXTURE_TICK in lib_def:
        sys.exit("OP13 ships a waveform %d; it would collide with AOSP TEXTURE_TICK" % AOSP_TEXTURE_TICK)
    texture_soft_ratio = (np.abs(np.frombuffer(lib_soft[OP_TEXTURE_WF], dtype=np.int8)).max()
                          / np.abs(np.frombuffer(lib_def[OP_TEXTURE_WF], dtype=np.int8)).max()
                          if OP_TEXTURE_WF in lib_soft else None)
    if set(lib_soft) != set(SOFT_IDS):
        sys.exit("soft ids in vibrator_effect.json %s differ from the HAL patch bitmap %s"
                 % (sorted(lib_soft), sorted(SOFT_IDS)))
    if not a.no_retune_library:
        before = sorted(centroid_hz(np.frombuffer(p, dtype=np.int8).astype(float))
                        for p in lib_def.values() if len(p) > 8)
        resynth = 0
        for d in (lib_def, lib_soft):
            for wf in list(d):
                if pcm_ms(d[wf]) > a.retune_max_ms and wf not in a.retune_waveform:
                    continue                      # long patterns stay as OnePlus authored them
                resynth += len(d[wf]) < RATE * a.min_cycles / a.f0
                d[wf] = retune_effect(d[wf], a.f0, a.min_cycles)
        after = sorted(centroid_hz(np.frombuffer(p, dtype=np.int8).astype(float))
                       for p in lib_def.values() if len(p) > 8)
        print("retuned effects <=%.0f ms onto f0=%.1f Hz: median centroid %.0f -> %.0f Hz, %d sub-%.1f-cycle ticks "
              "re-synthesised; longer patterns left as authored"
              % (a.retune_max_ms, a.f0, before[len(before) // 2], after[len(after) // 2], resynth, a.min_cycles))

    xml_text = open(a.xml, encoding="utf-8").read()
    if a.dedicated_track_ids:
        xml_text = dedicated_track_ids(xml_text, a.haptic_tracks)
    shift = 0.0 if a.no_track_retune else a.f0 - a.track_src_f0
    tracks = load_ringtone_tracks(xml_text, a.haptic_tracks, shift)
    clash = sorted(set(tracks) & (set(lib_def) | {AOSP_TEXTURE_TICK}))
    if clash:
        sys.exit("ringtone tracks collide with OP13 waveform ids %s" % clash)

    raw = {}                       # file name -> (pcm, waveform id)
    for wf, pcm in sorted(lib_def.items()):
        raw["%d_op%d_RTP.bin" % (wf, wf)] = (pcm, wf)
    for wf, pcm in sorted(lib_soft.items()):
        raw["%d_opsoft%d_RTP.bin" % (wf + SOFT_OFFSET, wf)] = (pcm, wf)
    for wf, (pcm, name) in sorted(tracks.items()):
        raw["%d_rt%s_RTP.bin" % (wf, re.sub(r"[^A-Za-z0-9]", "", name))] = (pcm, wf)
        lib_def[wf] = pcm

    short_gain = a.master_gain * a.ui_gain
    long_gain = a.master_gain * a.long_gain
    track_gain = a.master_gain * a.track_gain
    track_names = {n for n, (pcm, wf) in raw.items() if wf in tracks}

    def gain_for(name, pcm):
        if name in track_names:
            return track_gain
        return long_gain if pcm_ms(pcm) > a.long_ms else short_gain

    # only the short UI ticks may be waveshaped; anything long stays spectrally clean (no added buzz)
    files = {name: apply_gain(pcm, gain_for(name, pcm),
                              allow_waveshape=(name not in track_names and pcm_ms(pcm) <= a.long_ms))
             for name, (pcm, wf) in raw.items()}
    n_long = sum(1 for n, (pcm, wf) in raw.items() if n not in track_names and pcm_ms(pcm) > a.long_ms)
    print("gain: %.2fx on %d short effects, %.2fx on %d patterns >%.0f ms, %.2fx on %d ringtone tracks (master %.2fx)"
          % (short_gain, len(files) - n_long - len(track_names), long_gain, n_long,
             a.long_ms, track_gain, len(track_names), a.master_gain))
    # Texture ticks. TEXTURE_TICK is the one stock AOSP effect OnePlus' framework neither converts nor
    # ships, so perform(21) found no file and was dropped (launcher quick-switch along the nav bar,
    # bubbles, third-party scroll ticks). OnePlus' own texture tick, the weak granular wf 7, is a 27-peak
    # 8 ms transient at 300 Hz: crisp on their wideband motor, below feel on this narrowband LRA (peak 39
    # after the retune). Both get one f0 tick at --texture-tick-peak; the soft variant keeps OnePlus' ratio.
    if not a.no_retune_library:
        tick = synth_tick(a.texture_tick_peak, a.min_cycles, a.f0)
        as_pcm = lambda y: np.clip(np.round(y), -127, 127).astype(np.int8).tobytes()
        files["%d_op%d_RTP.bin" % (OP_TEXTURE_WF, OP_TEXTURE_WF)] = as_pcm(tick)
        files["%d_op%d_RTP.bin" % (AOSP_TEXTURE_TICK, AOSP_TEXTURE_TICK)] = as_pcm(tick)
        if texture_soft_ratio is not None:
            soft_tick = as_pcm(tick * min(1.0, texture_soft_ratio))
            files["%d_opsoft%d_RTP.bin" % (OP_TEXTURE_WF + SOFT_OFFSET, OP_TEXTURE_WF)] = soft_tick
        print("texture tick: wf %d + AOSP TEXTURE_TICK %d at peak %.0f (%.1f ms at f0), soft x%.2f"
              % (OP_TEXTURE_WF, AOSP_TEXTURE_TICK, a.texture_tick_peak, pcm_ms(as_pcm(tick)),
                 min(1.0, texture_soft_ratio or 1.0)))
    for name, pcm in files.items():
        write(os.path.join(rom, "odm", "firmware", name), pcm)
    print("effect library: %d OP13 + %d soft + %d ringtone tracks (carrier shift %+.1f Hz, %.1f MB) -> odm/firmware"
          % (len(lib_def) - len(tracks), len(lib_soft), len(tracks), shift,
             sum(len(p) for p in files.values()) / 1e6))

    stock_rtp = sorted(l.strip() for l in open(a.stock_rtp_list, encoding="utf-8") if l.strip())
    if not all(STOCK_RTP.match(n) for n in stock_rtp):
        sys.exit("unexpected name in %s" % a.stock_rtp_list)
    if set(stock_rtp) & set(files):
        sys.exit("library file names collide with stock Xiaomi names")
    write(os.path.join(rom, "odm_firmware_remove.txt"), ("\n".join(stock_rtp) + "\n").encode())

    # ---- 2. my_product: effect_waveform.xml remap + haptic_channel_support removed ---------------
    fixed, report = remap_xml(xml_text, lib_def, set(tracks))
    used = set(int(x) for x in re.findall(r'waveform_id="(\d+)"', fixed))
    missing = sorted(used - set(lib_def))
    if missing:
        sys.exit("BUG: still unresolved waveform ids %s" % missing)
    write(os.path.join(rom, "my_product", "etc", "vibrator", "effect_waveform.xml"), fixed.encode("utf-8"))
    with open(os.path.join(out, "remap_report.csv"), "w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=["effect_id", "desc", "usage", "old_waveform", "old_duration",
                                        "new_waveform", "new_duration", "rule"])
        w.writeheader()
        w.writerows(report)
    print("effect_waveform.xml: %d entries remapped, all %d referenced waveform ids resolve"
          % (len(report), len(used)))

    # ---- 3. the O-Haptics vibrator HAL, in place of Xiaomi's (nothing patched) -----------------
    out_path = lambda rel: os.path.join(rom, *rel.split("/"))
    svc_out = out_path(SVC_REL)
    build_service(svc_out)
    if "O-Haptics vibrator HAL".encode() in open(a.stock_hal, "rb").read():
        sys.exit("%s is already the O-Haptics HAL, not Xiaomi's" % a.stock_hal)
    # Xiaomi's own HAL, rc and manifest travel in stock/: the rc and manifest are installed from there (an
    # earlier kit may have rewritten them), and the HAL binary is what apply.sh --revert puts back.
    stock = {}
    for rel in (SVC_REL, XM_RC_REL, XM_VINTF_REL):
        src = a.stock_hal if rel == SVC_REL else os.path.join(ROOT, "work", "device", os.path.basename(rel))
        dst = os.path.join(rom, "stock", *rel.split("/"))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copy(src, dst)
        stock[rel] = hashlib.md5(open(dst, "rb").read()).hexdigest()
    print("O-Haptics vibrator HAL %s (%d bytes); Xiaomi's init rc and VINTF manifest stay stock"
          % (SVC_REL, os.path.getsize(svc_out)))

    # ---- 4. on-phone integration kit ------------------------------------------------------------
    # The HAL keeps the entries Xiaomi's file already has; they are rewritten anyway, together with the rc and
    # manifest entries, because earlier kits and hand merges changed or removed them.
    own = [(SVC_REL, "0 2000 0755", "hal_vibrator_default_exec"), (XM_RC_REL, "0 0 0644", "vendor_configs_file"),
           (XM_VINTF_REL, "0 0 0644", "vendor_configs_file")]
    fs = (["odm/firmware/%s 0 0 0644" % n for n in sorted(files)]
          + ["%s %s" % (rel, mode) for rel, mode, _ in own])
    ctx = (["/odm/firmware/%s u:object_r:vendor_file:s0" % re.escape(n) for n in sorted(files)]
           + ["/%s u:object_r:%s:s0" % (re.escape(rel), label) for rel, _, label in own])
    gone = [rel for rel, _, _ in own] + OLD_FILES   # stripped before appending (idempotent)
    write(os.path.join(rom, "config", "odm_fs_config.add"), ("\n".join(fs) + "\n").encode())
    write(os.path.join(rom, "config", "odm_file_contexts.add"), ("\n".join(ctx) + "\n").encode())
    # grep -F patterns matching the whole first field (path + separator) of every line we own or remove
    names = stock_rtp + sorted(files)
    write(os.path.join(rom, "config", "odm_fs_config.remove"),
          ("".join("odm/firmware/%s \n" % n for n in names) + "".join("%s \n" % rel for rel in gone)).encode())
    write(os.path.join(rom, "config", "odm_file_contexts.remove"),
          ("".join("/odm/firmware/%s \n/odm/firmware/%s \n" % (re.escape(n), n) for n in names)
           + "".join("/%s \n/%s \n" % (re.escape(rel), rel) for rel in gone)).encode())
    os.makedirs(os.path.join(rom, "lists"), exist_ok=True)
    os.replace(os.path.join(rom, "odm_firmware_remove.txt"), os.path.join(rom, "lists", "odm_firmware_remove.txt"))
    write(os.path.join(rom, "lists", "odm_firmware_add.txt"), ("\n".join(sorted(files)) + "\n").encode())

    md5 = lambda path: hashlib.md5(open(path, "rb").read()).hexdigest()
    lib_md5 = "".join("%s  %s\n" % (hashlib.md5(pcm).hexdigest(), n) for n, pcm in sorted(files.items()))
    write(os.path.join(rom, "lists", "library.md5"), lib_md5.encode())
    kit_id = hashlib.md5((lib_md5 + md5(svc_out) + "".join(sorted(stock.values()))).encode()).hexdigest()[:8]
    apply_sh = (APPLY_SH.replace("@KIT_ID@", kit_id)
                .replace("@STOCK_MD5@", stock[SVC_REL])
                .replace("@SVC_MD5@", md5(svc_out))
                .replace("@XM_RC_MD5@", stock[XM_RC_REL])
                .replace("@XM_VINTF_MD5@", stock[XM_VINTF_REL])
                .replace("@EW_BASE_MD5@", md5(a.xml))
                .replace("@EW_NEW_MD5@", md5(os.path.join(rom, "my_product", "etc", "vibrator", "effect_waveform.xml")))
                .replace("@SCALE_PROP@", "%s=%s" % (SCALE_PROP_KEY, "true" if a.xiaomi_strength_curve else "false")))
    write(os.path.join(rom, "apply.sh"), apply_sh.encode())
    shutil.copy(os.path.join(ROOT, "ROM_INTEGRATION.md"), rom)

    zpath = os.path.join(out, "OHaptics_onyx_partitions.zip")
    with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED) as z:
        for base, _, fnames in os.walk(rom):
            for n in sorted(fnames):
                full = os.path.join(base, n)
                z.write(full, "OHaptics_onyx/" + os.path.relpath(full, rom).replace(os.sep, "/"))
    print("kit:", zpath, "(%.1f MB) id %s" % (os.path.getsize(zpath) / 1e6, kit_id))

if __name__ == "__main__":
    main()
