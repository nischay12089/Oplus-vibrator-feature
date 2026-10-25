Circle to Search's expansion/hold vibration is reduced by 15% using a dedicated waveform (effect 527 → waveform 900). Its 600 ms timing and carrier are preserved, peak drops from 86 to 73, and the shared brightness/UI waveform is unchanged.

This full partition kit also includes the 58 ColorOS ringtone sync tracks, 46 message/notification sync tracks, and the continuous navigation hold feature. It retains the exact working HAL `4cd70c8825f3168889f6a725f9cc2566`, the other haptic strength settings, and Xiaomi's stock rc/manifest. No CTS library, jar, APK or additional service is changed.

Download `OHaptics_onyx_ColorOS.zip`, extract into a fresh folder, and use its `apply.sh` on your phone's unpacked ColorOS project. Require all 11 `[ok]` checks, repack and flash both `odm.img` and `my_product.img`, then reboot. Full instructions are inside the zip and in `COLOROS_INTEGRATION.md`.

Validation: 281/281 firmware hashes and all XML references resolve; full on-phone fixture installation, repeat apply, check mode and stock revert passed. CTS RMS ratio is 0.8495; sample count and zero crossings are preserved. Physical housing sound and real notification/call timing require confirmation after flashing.

Built for POCO F7 (onyx), ColorOS PHY110_16.0.10.500(CN01). `SHA256SUMS` identifies the exact zip.