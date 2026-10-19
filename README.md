# O-Haptics bridge — POCO F7 (onyx) running an OxygenOS port

OnePlus O-Haptics (touch haptics, soft/default touch style, intensity, ringtone sync, RichTap HE) on the POCO's Xiaomi
vibrator HAL. It ships as partition content (odm + my_product): nothing is patched, neither the framework nor
Xiaomi's HAL. **Install: see
[ROM_INTEGRATION.md](ROM_INTEGRATION.md)**, and use `out/OHaptics_onyx_partitions.zip` with `apply.sh` on the phone.

## Root cause (logcat + Ghidra + jadx)

`oplus-services.jar` → `PerformOplusPrebakedStep.play()` takes the "qcom linear motor" branch, and `services.jar` →
`VibratorController.performExtPrebaked()` sends this to `IVibrator/default`:

```
setAmplitude(1.0f + strength/2400f)        // OnePlus per-effect gain side channel (e.g. 1.8333)
perform(waveformId, EffectStrength.STRONG) // OnePlus waveform id from effect_waveform.xml
setAmplitude(10000 + touchStyle)           // 10000 default / 10001 soft
```

The OnePlus 13 HAL (`vendor.oplus.hardware.vibrator-service`) understands that, but it only drives Qualcomm
`qcom-hv-haptics` and AAC RichTap hardware. The POCO has an SI SIH688X (`si_haptic`). The Xiaomi HAL on the phone rejected amplitudes
above 1.0 (`setAmplitude failed`), treated OnePlus waveform ids as Xiaomi RTP ids (`no effect for id`) and ignored touch style.
Both vendors use the same waveform format (s8 PCM, 24 kHz), which the SI driver streams via `custom_wave` + `EV_FF`.

## What changes

| Layer | Change |
|---|---|
| O-Haptics vibrator HAL `odm/bin/hw/vendor.xiaomi.hardware.vibratorfeature.service` (init `vibratorfeature-hal-service`) | `tools/vibratorfeature/vibratorfeature_service.cpp` (NDK, no patching). Replaces Xiaomi's HAL binary and drives the si_haptic motor itself: serves `IVibrator/default` with OnePlus' calling convention plus `IOplusVibrator` (RichTap) as its extension. |
| Xiaomi's init rc and VINTF manifest | **stock, byte for byte** - the rc only names the HAL's path and the manifest only declares the interface it serves, so neither needs changing. |
| `odm/firmware` | Xiaomi's 195 `*_RTP.bin` removed. 176 files added: 72 OnePlus 13 waveforms (`effect_56` resampled 8k→24k), 400 ms / 1 s buzzes synthesized at f0 168.7 Hz, AOSP `TEXTURE_TICK` (21), 22 soft variants, 79 ringtone-synced tracks. |
| `odm/etc/build.prop` | `ro.odm.mm.vibrator.scaleStreaming=false`: strength maps 1:1 to amplitude, so the intensity sliders span 0-100%. `--xiaomi-strength-curve` restores Xiaomi's `0.5+0.5·g` curve. |
| `my_product/etc/vibrator/effect_waveform.xml` | Ringtone entries point at their synced tracks, with duration = track length. The 290 entries whose waveforms don't exist anywhere are remapped by category and nearest duration (`out/remap_report.csv`). |
| `my_product/etc/permissions/oplus.product.feature_multimedia_unique.xml` | `oplus.software.audio.haptic_channel_support` removed (see below). |
| `my_product/etc/permissions/oplus.feature.android.xml` | `oplus.software.vibrator_luxunvibrator` added. OnePlus gates the power-button/lock haptic, long-press effect 368 and all COUI widget haptics (brightness/volume sliders, seekbars, pickers, switches) behind it — `VibrateUtils.getLinearMotorVibrator()` returns null without it, and SystemUI logs `isLuxunVibratorSupport:false`. |
| Library gain | `--ui-gain` (default 1.25) boosts short effects with a normalised soft knee `127·tanh(g·x/127)/tanh(g)`. Effects over 300 ms (`--long-gain 0.8`) and ringtone tracks (`--track-gain 0.6`) are scaled linearly: a soft knee is a waveshaper and adds audible harmonics to long content. |
| Texture tick | OnePlus waveform 7 (recents/list scroll) and AOSP `TEXTURE_TICK` (21, the launcher's nav-bar quick switch) share a 2-cycle f0 tick at `--texture-tick-peak 64`. |

## Ringtone sync

With "sync with ringtone" (vibrate type 64), the caller passes the ringtone path and
`VibratorManagerServiceExtImpl.transferEffectToWaveform()` maps file name → `<ringtone_effect>` → waveform id. OnePlus ships
those haptics inside the ringtones: `/data/oplus/multimedia/ringtones/*.ogg` are 48 kHz stereo with `ANDROID_HAPTIC=1`.

1. `tools/hapticextract/HapticExtract.java` (dex, run with `app_process` as shell) decodes each OGG with the phone's
   `MediaCodec` and keeps only the haptic channel.
2. `build_bridge.py` band-limits each track to 20 Hz–1 kHz and shifts its carrier from OnePlus' LRA (~125.7 Hz, the median over all 79)
   to 168.7 Hz with an analytic-signal shift (envelope and timing unchanged, correlation 0.99). It then decimates to 24 kHz s8.

With `haptic_channel_support`, Telecom (`AsyncRingtonePlayer` → "startVibrateWhenRingPlay, no need invoke vibrate"),
notifications (`OplusSoundVibrateManager`) and the Settings preview (`SysFeatureUtils.supportRingingWithHaptic()`) leave
haptics to the audio HAL. The POCO has no haptic audio output (`isHapticPlaybackSupported()` = false), so nothing vibrated.
Without the flag, all three call `LinearmotorVibrator(type 64, ringtone path)`.

To refresh the tracks after a ROM update:
```bash
adb push tools/hapticextract/out/classes.dex /data/local/tmp/hapticextract.dex
adb shell 'CLASSPATH=/data/local/tmp/hapticextract.dex app_process /system/bin HapticExtract /data/local/tmp/hx /data/oplus/multimedia/ringtones/*.ogg'
adb pull /data/local/tmp/hx/. work/haptic_raw/
python tools/build_bridge.py
```

## Rebuild the kit

```bash
python tools/build_bridge.py
```
Inputs: the OnePlus 13 odm effect library (read-only reference under MIO-Kitchen `oneplus13`), and the files pulled from the phone
into `work/device/` (stock HAL, `effect_waveform.xml`, Xiaomi RTP list) and `work/haptic_raw/`; the service is built
with the Android NDK (r26).
Output: `out/rom/` plus `out/OHaptics_onyx_partitions.zip`.

## RichTap HE

The framework sends RichTap to the HAL extension as `IOplusVibrator`; Xiaomi's extension was `IVibratorExt`, so all HE
was rejected. `IVibratorExt.play(int[] data, int loop, long interval, int amp)` (transaction 1) is an HE player:
`[len, 1, globalIntensity, globalSharpness, 0, nEvents, events...]`, transient `[0, t, intensity, freq, 0, 0]`,
continuous `[1, t, intensity, freq, duration, 2, 0,0,4,(t,i)x4, 1,0,4,(t,f)x4]`, at most 16 events per call, rendered
by AAC's HED engine with native loop/interval. `tools/hebridge/XmExtTest.java` calls it directly for experiments
(root `app_process`). The OnePlus vibrator service (`tools/oplus_vibrator`) decodes HE 1.0 (16 x 55-int blocks) and HE 2.0 packets
(`HapticPlayer.generateSerializationDataHe_2_0`) and its calls into Xiaomi's lock-free code run on Xiaomi's binder threads. While Xiaomi streams HE, its `off()` (blocking
end-of-stream `write()`) and new `play()`/`perform()` calls wait for the stream; the service stops a running pattern with
Xiaomi's quick off (`setAmplitudeExt(_, 2)`) before taps, the framework's `off()` and new patterns.

## Not covered

* RichTap `performRtp`, ext-prebaked effect ids (played as a transient) and live `performHeParam` updates of a pattern
  that is already playing (see ROM_INTEGRATION.md §10).
* `my_product/etc/vibrator/effect2waveform.xml` is not read by either jar, so it was left untouched.
