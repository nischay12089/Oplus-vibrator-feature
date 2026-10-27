# O-Haptics on the K13 Turbo Pro base

Kit 65a399db for POCO F7 (onyx), ColorOS PLE110_16.0.10.600(CN01).

The connected phone already had the complete patched XML but only the old 176-waveform ODM library. Waveforms 800–845 (notifications), 900 (Circle to Search) and 2000–2057 (ringtones) were absent. This complete kit restores the matching 281-file library, retains the exact working HAL 4cd70c8825f3168889f6a725f9cc2566 and Xiaomi's stock rc/manifest, and preserves the approved touch/keyboard/slider/RichTap strengths and 15% CTS reduction.

K13's tones are ordinary 44.1 kHz audio under /my_product/res/audio, not the previous base's embedded-haptic files under /data/oplus/multimedia. They must not be decoded by assuming their right audio channel is a motor channel. Existing authored haptic tracks are retained at the same tempo and strength, with their total lengths trimmed or silence-padded to the installed K13 tone durations. No audio channel is streamed as motor data. For example, notification_004 is 400 ms here, rather than the previous base's 920 ms. Native K13 factory haptic channels are not present; melody-specific feel and real message/call timing still require on-device confirmation.

## Persistent installation

1. Extract OHaptics_onyx_K13_Turbo_Pro.zip to a fresh folder on the phone.
2. Unpack the CURRENT K13-base odm.img and my_product.img in your phone kitchen.
3. Run as root, with your real project path:

    sh /sdcard/Download/OHaptics_onyx/apply.sh /data/DNA/YOUR_K13_PROJECT

   For separate projects, add the my_product project path as the second argument. The project must contain odm/ and my_product/ plus the kitchen's odm_fs_config and odm_file_contexts metadata.
4. Require all 11 [ok] checks. Repack and flash BOTH odm.img and my_product.img, then reboot. Flashing only my_product leaves the XML requesting missing waveforms again.

The installer preserves other K13 feature entries (including haptic_vibrator_v1.support), keeps RichTap/luxun enabled, adds the continuous home-hold feature if missing, and removes audio.haptic_channel_support only if present. No jar/APK patch, new library, second service, module, or change to libcts_hook.so is included.

Check or revert an unpacked project:

    sh apply.sh --check PROJECT [MY_PRODUCT_PROJECT]
    sh apply.sh --revert PROJECT [MY_PRODUCT_PROJECT]

Backups use ohaptics_k13_backup. Revert restores backed-up data and Xiaomi's stock HAL/rc/manifest; it is not a downgrade to an earlier custom HAL.

## Live testing

A temporary /dev-backed firmware overlay can restore missing waveforms on the connected phone and is cleared by reboot. It does not persist to partition images. A HAL restart reloads the waveform library; framework XML duration changes become authoritative after flashing/reboot. Persistent use requires installing BOTH images as above.

Rebuild with python tools/package_k13.py after pulling the K13 XML/features and audio files to work/k13 and installing SoundFile into work/audio_deps (or making it importable). The approved authored input kit is out/coloros_release/rom. Raw decoded ordinary stereo audio from earlier diagnostic attempts is never used as haptic data. k13_timing_report.json records each tone's frame count, duration and source hash.