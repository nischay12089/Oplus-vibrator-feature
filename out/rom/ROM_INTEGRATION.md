# O-Haptics on POCO F7 (onyx): built into the partitions

This kit puts OnePlus O-Haptics into your port's **odm** and **my_product** images: touch haptics, soft/default touch
style, intensity sliders, "vibrate in sync with ringtone", every launcher/COUI haptic, and **RichTap HE** (the A–Z
index, adaptive COUI haptics, O-Haptics previews, games).

**Nothing is patched, and it is one file.** The kit replaces Xiaomi's vibrator HAL *binary* with O-Haptics' own HAL,
at Xiaomi's own path, so Xiaomi's **init rc and VINTF manifest stay byte-for-byte stock** - the rc only names that path
(`service vibratorfeature-hal-service`) and the manifest only declares the interface it serves. No second service, no
library, no patched bytes, and the OnePlus framework is untouched. The HAL drives the motor itself through the kernel
force-feedback device, so no Xiaomi code is involved at runtime.
No module and no root at runtime. Everything is applied on the phone to the unpacked partition folders from your
kitchen, before you repack the `.img` files.

It is built for:
* **odm**: the POCO F7 (onyx) HyperOS odm, with the stock `vendor.xiaomi.hardware.vibratorfeature.service`
  (md5 `2e2c998ec557ad8359b8f2a8e506c118`). Projects carrying the binary-patched HAL of an early kit
  (`b350ed94…`, `4aa14de4…`), an earlier build of this HAL, or the two-file layout of kits `0998d304`/`5cf0fe79`,
  are all accepted: `apply.sh` installs Xiaomi's stock rc and manifest and removes what those kits added.
* **my_product**: the OnePlus my_product your port uses (`effect_waveform.xml` md5 `0c868aa6e842311e81d54d3c73e39d33`)

`apply.sh` checks both before changing anything.

## 1. What is in the zip

```
OHaptics_onyx/
├── apply.sh                                               on-phone installer (toybox sh, no Python)
├── ROM_INTEGRATION.md                                     this file
├── odm/bin/hw/vendor.xiaomi.hardware.vibratorfeature.service   the O-Haptics vibrator HAL (§4)
├── odm/firmware/*_RTP.bin                                 176 effect files (OnePlus waveform ids + AOSP TEXTURE_TICK)
├── my_product/etc/vibrator/effect_waveform.xml            effect -> waveform table with dead ids remapped
├── stock/odm/...                                          Xiaomi's stock rc and manifest (installed), and its
│                                                          own HAL binary, which --revert puts back
├── config/odm_fs_config.add, odm_file_contexts.add        entries for the new files
├── config/odm_fs_config.remove, odm_file_contexts.remove  entries to drop (Xiaomi RTP files, old kit files)
└── lists/odm_firmware_remove.txt, odm_firmware_add.txt, library.md5
```

## 2. Apply on the phone

1. In your kitchen, unpack `odm.img` and `my_product.img`.
2. **Delete any previous `OHaptics_onyx` folder first**, then extract `OHaptics_onyx_partitions.zip`, e.g. to
   `/sdcard/OHaptics_onyx`. Android's `unzip` and some file managers *skip* files that already exist, which silently
   mixes old and new files. `apply.sh` prints its kit id (`O-Haptics kit <id>`) and verifies the md5 of everything it
   installs, so a stale extraction shows up as `[FAIL]`.
3. In a root shell or terminal (Termux with `su`, MT Manager terminal, or `adb shell su`):

```sh
# odm and my_product unpacked in the same project:
sh /sdcard/OHaptics_onyx/apply.sh /sdcard/<kitchen>/<project>

# odm and my_product in two different projects:
sh /sdcard/OHaptics_onyx/apply.sh /sdcard/<kitchen>/<odm_project> /sdcard/<kitchen>/<my_product_project>
```

`<project>` is the folder that contains `odm/` (and `my_product/`) plus the kitchen's `odm_fs_config` and
`odm_file_contexts`. The script finds those two files up to two levels deep, e.g. `config/`. With DNA the project
is `/data/DNA/<project>`; DNA ends every `odm_file_contexts` line with a NUL byte, and `apply.sh` keeps that format.

Expected output (the script verifies every item after applying and exits with `ERROR` if any is `[FAIL]`):

```
O-Haptics kit aa301c30
removed 195 Xiaomi RTP effects; verifying /sdcard/<kitchen>/<project> (...):
  [ok]   odm/firmware OnePlus effects 176/176 present, 176/176 match this kit
  [ok]   odm/firmware Xiaomi RTP effects left: 0
  [ok]   odm vibratorfeature-hal-service = O-Haptics vibrator HAL (odm/bin/hw/vendor.xiaomi.hardware.vibratorfeature.service)
  [ok]   odm Xiaomi init rc and VINTF manifest are stock
  [ok]   odm has no files left from earlier kits
  [ok]   odm fs_config 179/179, file_contexts 179/179
  [ok]   odm/etc/build.prop ro.odm.mm.vibrator.scaleStreaming=false
  [ok]   my_product effect_waveform.xml md5 6649abae456c949f962b8b0cffa6006d
  [ok]   my_product feature xml without oplus.software.audio.haptic_channel_support
  [ok]   my_product oplus.feature.android.xml with vibrator_luxunvibrator
backup: /sdcard/<kitchen>/<project>/ohaptics_backup
done - repack odm.img and my_product.img
```

Only repack when all ten lines are `[ok]`. You can re-check a project at any time without changing it:

```sh
sh /sdcard/OHaptics_onyx/apply.sh --check /sdcard/<kitchen>/<project> [/sdcard/<kitchen>/<my_product_project>]
```

4. Repack `odm.img` and `my_product.img` and flash **both**. The odm image grows by about **25 MB** (39 MB of effects
   added, 14 MB of Xiaomi effects removed), so let your kitchen resize it.

Running `apply.sh` twice is safe. To undo everything before repacking:

```sh
sh /sdcard/OHaptics_onyx/apply.sh --revert /sdcard/<kitchen>/<project> [/sdcard/<kitchen>/<my_product_project>]
```

## 3. What exactly changes

### odm
| Path | Change |
|---|---|
| `odm/bin/hw/vendor.xiaomi.hardware.vibratorfeature.service` | **replaced** by the O-Haptics vibrator HAL (§4). Keeps that file's existing `0 2000 0755` and `hal_vibrator_default_exec` entries, which are rewritten anyway in case an earlier kit changed them. Xiaomi's own binary travels in `stock/` for `--revert`. |
| `odm/etc/init/vendor.xiaomi.hardware.vibratorfeature.service.rc` | **stock** - it names the path above as `service vibratorfeature-hal-service` with `user system`, `group system input audio`, which is exactly what the HAL needs. Installed from `stock/` in case an earlier kit rewrote it. |
| `odm/etc/vintf/manifest/vendor.xiaomi.hardware.vibratorfeature.service.xml` | **stock** - declares `android.hardware.vibrator` `IVibrator/default`, the interface the HAL serves. |
| `odm/bin/hw/vendor.oplus.hardware.vibrator-service`, `odm/etc/init/vendor.oplus.hardware.vibrator-service.rc`, `odm/bin/hw/vendor.xiaomi.hardware.vibratorfeature.stock`, `odm/lib64/libohaptics_richtap.so`, `odm/etc/vintf/manifest/vibrator-default.xml` | **removed** if an earlier kit installed them. |
| `odm/firmware/<id>_<name>_RTP.bin` (195 files) | **deleted**: Xiaomi's effect library. Their ids would clash with the OnePlus ids in the same folder. Other firmware (camera, touch, …) is not touched. |
| `odm/firmware/*_op*_RTP.bin`, `*_opsoft*_RTP.bin`, `*_rt*_RTP.bin` (176 files) | **added**: 74 OnePlus 13 waveforms, `21_op21_RTP.bin` (AOSP `TEXTURE_TICK`, §6), 22 soft-touch variants, 79 ringtone-synced tracks. `0 0 0644`, `vendor_file`. |
| `odm_fs_config`, `odm_file_contexts` | the old entries removed, 179 new entries appended |
| `odm/etc/build.prop` | `ro.odm.mm.vibrator.scaleStreaming=false` — strength maps 1:1 to amplitude so the intensity sliders span 0-100%. `--xiaomi-strength-curve` restores the `0.5+0.5·g` curve. |

### my_product
| Path | Change |
|---|---|
| `etc/vibrator/effect_waveform.xml` | Replaced. 79 ringtone/notification/alarm entries point at their synced tracks, with duration = track length. 290 entries whose waveforms exist on no device (other ringtones, game/AT&T tracks) are remapped to the closest real effect. |
| `etc/permissions/oplus.feature.android.xml` | `oplus.software.vibrator_luxunvibrator` added (inserted before `</oplus-config>`). OnePlus gates the power-button/lock haptic, the long-press effect (368) and **every COUI widget haptic** — brightness and volume sliders, seekbars, pickers, switches — behind it. `oplus.software.vibrator_richctap` stays: RichTap is served by the OnePlus vibrator service. |
| `etc/permissions/oplus.product.feature_multimedia_unique.xml` | One line removed: `oplus.software.audio.haptic_channel_support`. With it, Telecom, notifications and the Settings preview play the ringtone's haptic channel through the audio HAL, which this phone cannot do (no haptic audio output), so "sync with ringtone" was silent. Without it they drive the vibrator. |

No new files in my_product, so its fs_config/file_contexts don't change.

## 4. The O-Haptics vibrator HAL

Source: `tools/vibratorfeature/vibratorfeature_service.cpp`, built with the NDK into one static PIE binary that
replaces Xiaomi's HAL file. It serves what the OnePlus framework expects of `android.hardware.vibrator.IVibrator/default`:

* the OnePlus calling convention: `setAmplitude(1 + strength/2400)` before `perform(<OnePlus waveform id>)`, and
  `setAmplitude(10000|10001)` for the touch style (soft plays `<id>+1000`)
* `vendor.oplus.hardware.oplusvibrator.IOplusVibrator` (V2) as the binder **extension**: RichTap HE
* `getCapabilities` → `ON_CALLBACK | PERFORM_CALLBACK | AMPLITUDE_CONTROL`, and `getSupportedEffects` listing every
  waveform id found in `/odm/firmware` - the same answers Xiaomi's HAL gave, so the framework behaves identically

### How the motor is driven
The LRA is the `si_haptic` (SI688X) force-feedback device, found by name under `/dev/input`:

| Need | Kernel call |
|---|---|
| level of effects and patterns | **the samples themselves**, with `FF_GAIN` held at full (see below) |
| level of `on()` | `EV_FF` / `FF_GAIN` |
| `on()` - a plain buzz | `FF_CONSTANT` + `replay.length` |
| effects and RichTap patterns | `FF_PERIODIC` with `custom_data[0] = 197`, then raw int8 24 kHz PCM written to the driver's `custom_wave` node |

Effects are the kit's own `/odm/firmware/<id>_<name>_RTP.bin` files, streamed with their samples scaled to
the requested strength.

**Why the samples carry the level, not `FF_GAIN`.** The driver's gain stage cannot express it. Measured
(`FF_GAIN` → chip level): 100% → `0x80`, 74% → `0x40`, and **49%, 37%, 24% and 3% all → `0x1e`**. Everything
below about half collapses onto one floor, so the intensity slider, the Gentle style and every trim below that
point did nothing at the motor - the slider felt stuck. Xiaomi's own HAL avoided this the same way, keeping the
gain at full and scaling the samples. Where full strength lands is set per kind of playback:
`kEffectScale` (1.45) - raised as far as each effect's headroom allows, with a soft knee for short effects
already at full scale (1.0 would match Xiaomi's HAL exactly) - and `kPatternScale` (0.5), the level the O-Haptics video was tuned at, since
patterns sustain and at full drive they read too strong. RichTap patterns are
rendered to PCM here, so there is no 16-event limit and no renderer to depend on.

Two things about that node decide whether anything is felt at all, and both cost a debugging round:
* **Writing samples only queues them.** The driver still needs i2c setup and then real time to play them, so the
  stream is held open for the playback length plus a margin. Ending it immediately throws the samples away - which
  silenced every effect short enough to be written in one go, while long patterns still worked.
* **The framework calls `off()` about 35 ms after a short `perform()`**, as routine cleanup while the motor is still
  moving. An `off()` that close to an effect's natural end is ignored; longer and looping patterns still stop at once.

Writes never hold a lock, and stopping erases the effect, which releases a write blocked waiting for buffer room. A tap
during a 3 s pattern is served in **1-3 ms**.

### How patterns are rendered
Events are summed as **envelopes** and one continuous carrier is applied at the end. Every event sits on the motor's
resonance, so giving each its own phase let overlapping events cancel and beat, and put a step in the waveform at every
event start - audible noise that rattles the back panel. Clipping the sum did the same, by adding odd harmonics of f0
(~505 and ~840 Hz). The timeline is therefore scaled **down** as a whole instead of clipping.

Levels are measured from the installed library rather than hardcoded: its short effects peak at 95 of 127 (the
headroom), and its soft waveforms are 0.77 of the normal ones (the soft ratio). Four dials shape the feel:

| Dial | Default | Governs |
|---|---|---|
| `kHeTrim` | 0.36 | RichTap pattern strength, against library loudness |
| `kGentleEffectScale` | 0.435 | where a full-strength effect lands on the soft ("Gentle") touch style at maximum intensity - the Gentle counterpart of `kEffectScale`, for taps, sliders and holding an app. 0.435 with `kSoftTrim` 0.3 is exactly the Gentle of kit `aa301c30`, used for a week and kept by choice (0.5-0.87 were tried live). Effects without a soft waveform also get the library's soft ratio (0.77), so they match the ones that have one |
| `kSoftTrim` | 0.3 | how much gentler RichTap patterns are on Gentle (on top of the soft ratio): x0.115 against x0.5 on Crisp |
| `kDurTrimExp` | 0.35 | how steeply longer effects are trimmed; a 104 ms slider end-stop lands at 0.56, a 17 ms tick at 1.00 |
| `kDurTrimFloor` | 1.0 | the limit for long effects; **1.0 turns the duration trim off**, so every effect plays at the strength the framework asked for (as kit `8f415157` did) - lower it to trim circle-to-search, the brightness slider (effect 362) and notifications again |
| `kEffectScale` | 1.45 | where a full-strength effect lands at maximum intensity, chosen by ear (1.70, 1.55 and 1.39 were tried): raised as far as each effect's own headroom allows, never clipped. 1.0 = exactly as kit `8f415157` |
| `kKneeMaxMs` | 60 | short effects already at full scale (taps 2 and 4, keyboard, volume, the slider end-stop) get the rest of `kEffectScale` from a soft knee - peak unchanged, everything below it lifted. Longer effects never get one: on sustained content it adds the harmonics that buzz the housing, so circle-to-search would stop at its clean maximum, about x1.48 (at 1.45 it sits just below that) |
| `kMotorQ` | 8 | damping of the motor model behind the **excursion limit**: a boosted short effect may swing the mass no further than the library's strongest tap (≤ 20 ms) does as authored. Holding an app (effect 6; 8 and 109 share it) drives three near-full half-cycles; at x1.45 it went past the motor's travel and rattled the back panel at maximum intensity. It now plays as authored there; the keyboard and taps 2/4 keep the full boost. Only the ranking of effects matters, and it was the same from Q 4 to 16 |
| `kPatternScale` | 0.5 | the same for RichTap patterns |

The duration values were first tuned while the driver's gain stage was flooring every level, so the trim barely
applied then. Once levels became real, the old 0.63 / 0.3 cut circle-to-search to a third; they were relaxed to
the values above.

Sustain, not peak, is what reads as strength: a pattern holding resonance for hundreds of ms feels far stronger than a
12 ms tick at the same peak, which is why duration is trimmed at all.

**There is no more headroom above this in software.** The driver clamps its own gain at `0x80` whatever is asked
(measured from `0x7fff` up to `0xffff`), and the effects at full scale cannot be raised further without clipping.
The one remaining lever is the chip's boost voltage (`drv_vboost`, 92 on this phone), a hardware setting this kit
deliberately leaves alone.

### The intensity slider
The framework **does** apply the user's intensity slider to ordinary effects - it is inside the strength of
`setAmplitude(1 + strength/2400)`: `1.2917` at the slider's minimum, `2.0000` at its maximum. So effects are not
scaled by it again here; doing so squared it. RichTap patterns are the exception: apps send amplitude 0 ("no
override") and nothing scales them, so for patterns only, the HAL reads `touch_stepless_vibration_intensity`
itself. It lives in `system_server`'s store, `/data/system/users/0/settings_system.xml`, readable because
Xiaomi's rc runs the HAL as uid `system`; that file is Android binary XML, and the value is the attribute
directly after the key. It is parsed only when its mtime changes, at most once a second, and full strength is
assumed if it cannot be read.

Do not test intensity with `settings put` or `cmd vibrator_manager`: neither reaches the OnePlus service's
cached copy, so both make the slider look broken. Only moving the real slider is a valid test.

### The touch style follows the setting
The framework announces Crisp/Gentle with `setAmplitude(10000|10001)` **only when the user changes it**, so after
a restart or boot the HAL reads `touch_style_vibration_intensity` from the same settings store as the intensity.
An earlier build kept its own copy on `/mnt/vendor/persist`; that copy drifted from the real setting and played
Gentle while the user had Crisp, cutting everything to a third. The setting is now the only record: the
announcement applies at once, and whenever the stored value changes, it wins.

Gentle also applies to effects the kit built no `<id>+1000` variant for (the recents scroll, some slider ticks), by
scaling them with the measured soft ratio.

### Diagnostics
The HAL appends every transaction to `/data/vendor/ohaptics/trace.log` **if that directory exists**, and is silent
otherwise - `logd` drops this HAL's own lines once haptics get busy, which hides exactly the calls worth seeing.
This needs a **permissive** ROM: on an enforcing one the policy does not let `hal_vibrator_default` write in
`vendor_data_file`, so the trace stays empty (dmesg shows the `write`/`append` denials). The HAL itself is fine on
enforcing - it can read the settings it follows; if a ROM's policy ever stops that, logcat says once
`cannot stat|open /data/system/users/0/settings_system.xml …`, and intensity and Crisp/Gentle then fall back to what
the framework announces. To turn the trace on:

```sh
adb shell su -c "mkdir -p /data/vendor/ohaptics && chmod 777 /data/vendor/ohaptics"
adb shell su -c "setprop ctl.restart vibratorfeature-hal-service"
```

### SELinux
Your ROM runs permissive, so nothing else is needed. On an enforcing build the service (domain
`hal_vibrator_default`, like Xiaomi's HAL) additionally needs to find the vibrator service and call Xiaomi's HAL:

```
(allow hal_vibrator_default hal_vibrator_service (service_manager (find)))
(allow hal_vibrator_default hal_vibrator_default (binder (call transfer)))
```

Add them to `odm/etc/selinux/odm_sepolicy.cil`. odm also ships a `precompiled_sepolicy`, which init uses as long as
the system-side hashes match, so it must be rebuilt (or removed, letting init compile the policy at boot).

## 5. Strength and intensity

Most of the perceived weakness was **not** amplitude: it was frequency. Your motor resonates at 168.7 Hz, but OnePlus
authored the library for their own LRA - the long effects sit at 125-160 Hz and the short UI ticks are fast transients
centred at 230-330 Hz. An LRA is a narrow bandpass, so that energy never becomes movement: only **8.3%** of the median
effect's energy was near 168.7 Hz.

| What | Default | Why |
|---|---|---|
| **Retune onto resonance** | effects ≤ 300 ms only (`--retune-max-ms 300`, `--no-retune-library` disables) | Each short effect's carrier is shifted so its energy centre lands on f0, with an analytic-signal shift that leaves envelope and duration untouched. Median energy at f0 **8.3% -> 55%**. Effects that would come out worse keep their original waveform. |
| **Re-synthesise sub-cycle ticks** | `--min-cycles 2.0` | One cycle at 168.7 Hz takes 5.9 ms, but effect 105 (slider step) ships as 5.2 ms, so the motor cannot complete a swing. Seven such ticks become crisp 2-cycle pulses at f0. |
| **Short-effect gain** | `--ui-gain 1.25` | Taps, keyboard and slider ticks (normalised soft knee, so nothing clips). |
| **Long patterns** | `--long-gain 0.8`, `--long-ms 300` | Notification patterns, the charge animation and the game-assistant launch: not retuned, **linearly** scaled. A soft knee on long content is a waveshaper - it adds 338/507 Hz harmonics that you hear as a buzz - so only ticks get one. Energy above 300 Hz equals OnePlus' originals. |
| **Ringtone sync tracks** | `--track-gain 0.6` | Linearly scaled, keeping only their +43 Hz sync shift. |
| **Scroll/texture tick** | `--texture-tick-peak 64` | OnePlus' "weak granular" tick (waveform 7: recents page scroll, COUI list scroll) is a peak-27, 8 ms 300 Hz transient that this LRA cannot render; it becomes a 2-cycle f0 tick, shared with AOSP `TEXTURE_TICK` (§6). |
| **Master scale** | `--master-gain 1.0` | Scales the whole library at once. |
| **Intensity curve** | linear (`--xiaomi-strength-curve` for the old one) | `scaleStreaming=false`, so strength maps 1:1 to amplitude and Settings' *Vibration intensity* spans the full range. |

With the linear curve the intensity sliders behave like this (Settings -> Sound & vibration -> Vibration intensity;
`touch_stepless_vibration_intensity` and `ring_stepless_vibration_intensity`, 0-2400):

| Setting | Amplitude now | With Xiaomi's curve (before) |
|---|---|---|
| 2400 (max) | 1.00 | 1.00 |
| 1800 | 0.75 | 0.88 |
| 1200 (mid) | 0.50 | 0.75 |
| 600 | 0.25 | 0.62 |

Current levels at max intensity (peak sample value of 127): tap 93, slider step 42, keyboard 116, scroll tick 64,
ringtone track (Feeling) 70, charge animation 48. RichTap HE plays at the intensity the app asks for (as on OnePlus).

## 6. Launcher audit

Every haptic call in `OplusLauncher.apk` (96 call sites) was traced to the waveform it ends up playing:

| Path | Status |
|---|---|
| `VibrationUtils.vibrate(type)` → effect 0/1/68/69/73/367 (long-press, drag, folders, recents gesture, stacks, app lock) | all map to existing waveforms |
| `performHapticFeedback(0…308)` → OnePlus provider → effects 0/1/2/3/68/69/70/152/154/368 | all map |
| A–Z index, adaptive COUI widgets → RichTap HE | played by the OnePlus vibrator service (§4) |
| swipe along the nav bar to switch apps → AOSP `TEXTURE_TICK` (21) | was `no effect for id 21`; now a library waveform |
| recents page scroll (horizontal layout) / COUI list scroll → effect 68 → waveform 7 | was too faint to feel; now a 64-peak f0 tick |
| **stack** recents scroll | no haptic **by OnePlus design**: `StackRecentsViewDelegate.needVibrateWhenScroll()` returns `false` (the horizontal layout returns `true`). A real OnePlus 13 doesn't vibrate there either. |

## 7. Verify after flashing

```sh
adb shell getprop init.svc.vibratorfeature-hal-service                             # running
adb shell dumpsys --pid android.hardware.vibrator.IVibrator/default                # = getprop init.svc_debug_pid.vibratorfeature-hal-service
adb shell md5sum /odm/bin/hw/vendor.xiaomi.hardware.vibratorfeature.service        # the kit's SVC_MD5, not 2e2c998e
adb logcat -s vendor.vibratorfeature
```
* At boot: `android.hardware.vibrator.IVibrator/default: O-Haptics vibrator HAL (IVibrator V2 +
  vendor.oplus.hardware.oplusvibrator.IOplusVibrator V2) on the si_haptic motor`, preceded by
  `motor: /dev/input/eventN …`, `library headroom: 95 of 127`, `library soft ratio: 0.77`, `excursion ceiling: 0.000389 at f0 …` (0 would mean the limit is off) and
  `touch style from settings: default (crisp)` (or `soft (gentle)`).
* `dumpsys vibrator_manager` must report `capabilities = [ON_CALLBACK, PERFORM_CALLBACK, AMPLITUDE_CONTROL]` and a
  `supportedEffects` list including `TEXTURE_TICK`; recent vibrations should read `finished`, not `cancelled`.
* **Touch haptics, soft style:** with Gentle on, `perform effect=1` must resolve to `id=1001` (see §4 diagnostics).
* **RichTap:** drag the launcher's A–Z index; each letter renders a pattern.
* **Power button / lock, brightness and volume sliders:** these need `vibrator_luxunvibrator`. If a slider is silent,
  check `adb logcat | grep -i luxun` for `isLuxunVibratorSupport:false`.
* **Ringtone sync** (Settings → ringtone vibration → sync with ringtone, or an incoming call):
  `LinearmotorVibrator … ringtoneVibrateType=64 … ringtoneFilePath=…/OnePlus_new_feeling.ogg`, then
  `perform effect 98` (Feeling). Telecom must **not** log `startVibrateWhenRingPlay, no need invoke vibrate`.
* `adb shell cmd vibrator_manager synced prebaked 21` → the AOSP `TEXTURE_TICK` waveform plays.
* `getprop ro.odm.mm.vibrator.scaleStreaming` → `false`
* Note `cmd vibrator_manager` ignores the intensity slider entirely, so never judge intensity with it - it reported the
  same gain at slider 2400, 1200 and 400. Only real UI interaction shows the truth.

## 8. If the phone bootloops

`system_server` waits for `IVibrator/default` at boot (`VibratorManagerService` → `nativeGetVibratorIds`) and its
watchdog restarts it in a loop if nothing ever registers that name. This HAL registers it itself, from Xiaomi's own
stock rc, and registers even when the motor cannot be opened - so a broken motor means no vibration, never a bootloop.

The two bootloops during development both came from hand-merged odm images in which **no** vibrator HAL binary was
left at `/odm/bin/hw/vendor.xiaomi.hardware.vibratorfeature.service`: one where a service binary was copied to a
renamed path and the real HAL dropped, and one DNA build where only some of the kit's files were copied in. The rule
that avoids both: install with `apply.sh`, and repack only when all ten lines are `[ok]`. If the phone does loop,
`adb` still works during it - restore that one file and it boots.

## 9. If ringtone sync doesn't work after flashing

On the phone, run `adb shell grep -c haptic_channel_support /my_product/etc/permissions/oplus.product.feature_multimedia_unique.xml`.
If it prints `1`, the flag is still in the flashed my_product. Re-run the current `apply.sh` on the same project
(`--check` shows that line as `[FAIL]`). It fixes only what's missing. Then repack and flash **my_product.img**.

## 10. Known limits

* **RichTap extras without an equivalent on this HAL:** `performRtp` (raw RTP streams) is ignored, RichTap
  ext-prebaked ids play as a transient at the requested strength, and `performHeParam` updates (live interval /
  amplitude / frequency changes) apply to the next pattern rather than one already playing. Multi-packet HE 2.0
  patterns (longer than 10 sub-patterns) play once even if they ask for loops.
* **Custom ringtones** (your own mp3/ogg) have no haptic track; only the 79 built-in OnePlus ringtones,
  notifications and alarms sync.
* The kit is tied to the HAL and `effect_waveform.xml` md5s above. After a base update, rebuild it
  (`python tools/build_bridge.py` in the project) from the new files.
