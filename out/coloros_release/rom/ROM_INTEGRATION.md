# Complete ColorOS O-Haptics partition kit

Kit fb7bc8ea for POCO F7 (onyx), ColorOS PHY110_16.0.10.500(CN01), with the existing working standalone vibrator HAL.

This is one complete installer combining the current fixes. Use this kit instead of applying the earlier ringtone/message/CTS add-ons in sequence.

## Changes

- **Circle to Search is 15% quieter.** Effect 527 now uses dedicated waveform 900, separate from waveform 362 used by brightness and other controls. Its 600 ms timing and resonant carrier are unchanged; peak falls from 86 to 73. Plain linear scaling reduces the ramp and final pulse together, without adding harmonics. Duration in the framework table stays 650 ms.
- **Continuous navigation hold enabled:** adds `oplus.software.vibrator.long_press_home_wave_support` if missing. This is the feature gate that makes SystemUI request the ramp during expansion instead of only a final tap.
- **58 ringtone sync tracks:** each installed ColorOS ringtone uses its own extracted haptic channel and exact playback length instead of a shared generic fallback.
- **46 message/notification sync tracks:** each installed notification tone has its own haptic waveform. Existing 0.60 linear track trim and pulse timing are retained. Notification ids 800â€“845 avoid the HAL's +1000 Gentle-variant namespace.
- **HAL retained byte for byte:** `4cd70c8825f3168889f6a725f9cc2566`, the same binary currently installed on the connected phone. Effect strength 1.45, Gentle tuning, keyboard/taps/sliders and RichTap video tuning are unchanged. Waveform 362 is retained unchanged from the preceding ColorOS fix.
- **One service at runtime:** Xiaomi's stock init rc and VINTF manifest remain byte-for-byte stock. No separate service, new library, jar/APK patch, module or change to `libcts_hook.so`. The stock Xiaomi HAL in the zip is only the rollback copy.

The kit contains 281 firmware waveforms: the existing 176 plus 58 ringtone tracks, 46 notification tracks and the dedicated CTS waveform. Every XML waveform reference resolves.

## Apply on your phone

1. Extract `OHaptics_onyx_ColorOS.zip` into a **fresh** folder, for example `/sdcard/Download/OHaptics_onyx`. Do not merge it with an old extraction.
2. Unpack your current ColorOS `odm.img` and `my_product.img` in your kitchen.
3. Run as root, replacing the project path:

    sh /sdcard/Download/OHaptics_onyx/apply.sh /data/DNA/YOUR_COLOROS_PROJECT

   If odm and my_product are in separate projects, add the my_product project as the second argument.
4. Require **all 11 checks to say `[ok]`**. Then repack and flash **both odm.img and my_product.img**, and reboot. HAL library scanning, feature flags and framework XML tables are cached at startup.
5. Test the Circle to Search expansion/hold, then ringtone sync and a message notification. The relevant app channel must permit sound and vibration and use the sync/follow-tone option.

The installer accepts the original known table and the prior kits from this project. It refuses unknown XML unless explicitly forced. It preserves other feature entries, backs up project files and retains DNA's NUL-before-newline file_contexts format. It does not change partition images on this PC.

Check or undo an unpacked project:

    sh apply.sh --check PROJECT [MY_PRODUCT_PROJECT]
    sh apply.sh --revert PROJECT [MY_PRODUCT_PROJECT]

Rollback restores backed-up project data from `ohaptics_backup`, removes this kit's firmware files, and puts Xiaomi's stock HAL, rc and manifest back. It is a stock-HAL revert, not a downgrade to an earlier O-Haptics HAL. Keep that backup until you have tested the flashed images.

## Validation and limits

All waveform hashes, XML references, namespace separation, preserved HAL/stock rc/manifest hashes and unchanged non-CTS effect entries are checked when assembling the release. The trim preserves sample count and zero crossings, with RMS ratio approximately 0.85; the peak is 73 instead of 86. The on-phone installer is tested on isolated project copies, including repeat application, checks and rollback.

Physical housing noise and real audio-to-motor timing still need confirmation after flashing. This kit covers the 58 built-in ringtone files and 46 built-in notification files extracted from this ColorOS ROM. Custom audio without an embedded/mapped haptic track and application-provided vibration patterns are not converted. A released hold cancels the ramp; it does not vibrate indefinitely after search opens.

## Reassemble the release

Run `python tools/package_release.py` from the repository. It assembles the committed working kit in `out/rom` and the extracted ColorOS add-ons when available; otherwise it uses the published `out/coloros_release/rom` inputs. The working HAL is deliberately not recompiled. Outputs are `out/coloros_release/OHaptics_onyx_ColorOS.zip`, `SHA256SUMS`, and the complete `rom/` folder. `release_manifest.json` records the kit id, hashes, waveform counts and CTS trim.

The older raw extraction builders require the locally pulled `work/coloros` inputs. Device APKs, logs, raw audio and decompilations remain outside Git.