#!/system/bin/sh
# ColorOS message/notification sync data fix: no HAL, rc, manifest, jar, APK or CTS library replacement.
set -eu
KIT=$(cd "$(dirname "$0")" && pwd)
BASE=@BASE_MD5@
NEW=@NEW_MD5@
BK=ohaptics_message_sync_backup
die() { echo "ERROR: $*"; exit 1; }
sum() { md5sum "$1" | cut -d' ' -f1; }
MODE=apply
case "${1:-}" in --check) MODE=check; shift;; --revert) MODE=revert; shift;; esac
[ -n "${1:-}" ] || die 'usage: sh apply.sh [--check|--revert] <odm_project> [my_product_project]'
P=$(cd "$1" && pwd)
MP=$(cd "${2:-$1}" && pwd)
EW=my_product/etc/vibrator/effect_waveform.xml
FE=my_product/etc/permissions/oplus.product.feature_multimedia_unique.xml
[ -d "$P/odm/firmware" ] && [ -f "$MP/$EW" ] && [ -f "$MP/$FE" ] || die 'unpack odm and my_product first'
FS=$(find "$P" -maxdepth 2 -name odm_fs_config -type f | grep -v "/$BK/" | head -n1)
FC=$(find "$P" -maxdepth 2 -name odm_file_contexts -type f | grep -v "/$BK/" | head -n1)
[ -n "$FS" ] && [ -n "$FC" ] || die 'odm_fs_config / odm_file_contexts missing'
backup() {
    mkdir -p "$1/$BK/$(dirname "$2")"
    [ ! -e "$1/$BK/$2" ] && [ ! -e "$1/$BK/$2.absent" ] || return 0
    if [ -e "$1/$2" ]; then cp -p "$1/$2" "$1/$BK/$2"; else touch "$1/$BK/$2.absent"; fi
}
verify() {
    count=0
    while read -r hash rel; do
        case "$rel" in odm/*) base=$P;; *) base=$MP;; esac
        [ "$(sum "$base/$rel")" = "$hash" ] || die "checksum: $rel"
        count=$((count+1))
    done < "$KIT/files.md5"
    ! grep -q -F 'oplus.software.audio.haptic_channel_support' "$MP/$FE" || die 'audio haptic flag still enabled'
    while read -r line; do
        grep -q -F "$line" "$FS" || die 'fs_config entry missing'
    done < "$KIT/config/odm_fs_config.add"
    tr '\000' '\001' < "$FC" > "$P/.oh-coloros-context-check"
    while read -r line; do
        grep -q -F "$line" "$P/.oh-coloros-context-check" || die 'file_contexts entry missing'
    done < "$KIT/config/odm_file_contexts.add"
    rm -f "$P/.oh-coloros-context-check"
    echo "[ok] $count file checksums; audio haptic flag removed; firmware metadata present"
}
if [ "$MODE" = check ]; then verify; exit 0; fi
if [ "$MODE" = revert ]; then
    [ -f "$MP/$BK/$EW" ] && [ -f "$P/$BK/${FS#$P/}" ] || die 'backup missing'
    for f in "$KIT"/odm/firmware/*; do
        rel=odm/firmware/${f##*/}
        if [ -f "$P/$BK/$rel" ]; then cp -p "$P/$BK/$rel" "$P/$rel"; else rm -f "$P/$rel"; fi
    done
    cp -p "$P/$BK/${FS#$P/}" "$FS"
    cp -p "$P/$BK/${FC#$P/}" "$FC"
    cp -p "$MP/$BK/$EW" "$MP/$EW"
    cp -p "$MP/$BK/$FE" "$MP/$FE"
    echo 'Restored pre-fix partition content; repack and flash both images.'
    exit 0
fi
# Check kit integrity before making changes (toybox md5sum -c exit status is unreliable).
while read -r hash rel; do [ "$(sum "$KIT/$rel")" = "$hash" ] || die "damaged kit: $rel"; done < "$KIT/files.md5"
CURRENT=$(sum "$MP/$EW")
[ "$CURRENT" = "$BASE" ] || [ "$CURRENT" = "$NEW" ] || die 'different ROM waveform XML; refusing to overwrite'
backup "$MP" "$EW"
backup "$MP" "$FE"
backup "$P" "${FS#$P/}"
backup "$P" "${FC#$P/}"
for f in "$KIT"/odm/firmware/*; do
    rel=odm/firmware/${f##*/}
    backup "$P" "$rel"
    cp "$f" "$P/$rel"
    chmod 0644 "$P/$rel"
done
cp "$KIT/$EW" "$MP/$EW"
# Write through the existing feature file so its other features and metadata survive.
grep -v -F 'oplus.software.audio.haptic_channel_support' "$MP/$FE" > "$MP/$FE.oh-coloros.tmp" || true
[ -s "$MP/$FE.oh-coloros.tmp" ] || die 'feature edit would produce an empty file'
cat "$MP/$FE.oh-coloros.tmp" > "$MP/$FE"
rm -f "$MP/$FE.oh-coloros.tmp"
# Preserve DNA's NUL-before-newline file_contexts format.
append_metadata() {
    dest=$1
    additions=$2
    nul=$3
    tr '\000' '\001' < "$dest" > "$dest.oh-coloros.tmp"
    while IFS= read -r line; do
        if ! grep -q -F "$line" "$dest.oh-coloros.tmp"; then
            [ ! -s "$dest" ] || [ -z "$(tail -c1 "$dest")" ] || printf '\n' >> "$dest"
            if [ "$nul" = 1 ]; then printf '%s\0\n' "$line" >> "$dest"; else printf '%s\n' "$line" >> "$dest"; fi
        fi
    done < "$additions"
    rm -f "$dest.oh-coloros.tmp"
}
NUL=0
[ "$(tail -c 2 "$FC" | od -An -tx1 | tr -d ' \n')" != 000a ] || NUL=1
append_metadata "$FS" "$KIT/config/odm_fs_config.add" 0
append_metadata "$FC" "$KIT/config/odm_file_contexts.add" "$NUL"
verify
echo 'Done. Repack and flash odm.img + my_product.img, then reboot. Framework feature/XML caches require reboot.'
echo 'HAL, strength constants, ringtone mappings, CTS features and libcts_hook.so unchanged.'
