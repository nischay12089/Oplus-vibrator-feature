#!/system/bin/sh
# O-Haptics for POCO F7 (onyx) OxygenOS ports -- on-phone partition integration. Nothing is patched.
# One HAL replaces Xiaomi's vibrator HAL binary and serves what the OnePlus framework expects of
# IVibrator/default - its calling convention and IOplusVibrator (RichTap) - driving the motor itself through the
# kernel force-feedback device. Xiaomi's init rc and VINTF manifest stay byte-for-byte stock: the rc only names
# this path (service vibratorfeature-hal-service) and the manifest only declares the interface it serves.
#
#   sh apply.sh <project> [my_product_project]          apply (and verify)
#   sh apply.sh --check <project> [my_product_project]  only report what is / isn't in place
#   sh apply.sh --revert <project> [my_product_project] restore the backed-up stock files
#
# <project> is your kitchen's unpacked project: it must contain odm/ (the POCO odm) and the
# odm_fs_config / odm_file_contexts files (searched up to two levels deep, e.g. config/).
# my_product/ (the OnePlus my_product) may live in the same project or in a second one.
# Every file this touches is copied to <project>/ohaptics_backup/ on the first run.
KIT=$(cd "$(dirname "$0")" && pwd)
KIT_ID=fb7bc8ea
# The O-Haptics HAL, at Xiaomi's own path so Xiaomi's stock rc starts it
SVC_REL=odm/bin/hw/vendor.xiaomi.hardware.vibratorfeature.service
SVC_MD5=4cd70c8825f3168889f6a725f9cc2566
SVC_MARKER="O-Haptics vibrator HAL"   # string inside the HAL binary
# Xiaomi's own HAL binary: what the kit was built against, and what --revert puts back
STOCK_MD5=2e2c998ec557ad8359b8f2a8e506c118
OLD_PATCHED_MD5S="b350ed946b86091e43c76e6ae6fedc74 4aa14de4bcf65021b8b87153e4403cc3"   # earlier kits changed it
# Xiaomi's init rc and VINTF manifest: installed exactly stock, in case an earlier kit rewrote them
XM_RC_REL=odm/etc/init/vendor.xiaomi.hardware.vibratorfeature.service.rc
XM_RC_MD5=a1c78826c1f966a7cefc27b42d2764ac
XM_VINTF_REL=odm/etc/vintf/manifest/vendor.xiaomi.hardware.vibratorfeature.service.xml
XM_VINTF_MD5=4fc1ff4e78e7793da1fc93411009bd26
# left behind by earlier kits: removed
OLD_FILES="odm/bin/hw/vendor.oplus.hardware.vibrator-service odm/etc/init/vendor.oplus.hardware.vibrator-service.rc
odm/bin/hw/vendor.xiaomi.hardware.vibratorfeature.stock odm/lib64/libohaptics_richtap.so
odm/etc/vintf/manifest/vibrator-default.xml"
EW_REL=my_product/etc/vibrator/effect_waveform.xml
EW_BASE_MD5=0c868aa6e842311e81d54d3c73e39d33
EW_BASE_MD5S="0c868aa6e842311e81d54d3c73e39d33 6649abae456c949f962b8b0cffa6006d 6d626fe2012f93fde4ac837e92724f5b 5b87a9532706acefe67f6e2ff060fc23 cdcf0efc592a4e11c66138d20b3f8984"
EW_NEW_MD5=cdcf0efc592a4e11c66138d20b3f8984
FEAT_REL=my_product/etc/permissions/oplus.product.feature_multimedia_unique.xml
FEATA_REL=my_product/etc/permissions/oplus.feature.android.xml
LUXUN='    <oplus-feature name="oplus.software.vibrator_luxunvibrator" />'
LUXUN_GREP='name="oplus.software.vibrator_luxunvibrator"'
HOLD='    <oplus-feature name="oplus.software.vibrator.long_press_home_wave_support" />'
HOLD_GREP='name="oplus.software.vibrator.long_press_home_wave_support"'
SCALE_PROP=ro.odm.mm.vibrator.scaleStreaming=false
BK=ohaptics_backup

die() { echo "ERROR: $*"; exit 1; }
sum() { md5sum "$1" 2>/dev/null | cut -d' ' -f1; }
ends_nl() { [ ! -s "$1" ] || [ -z "$(tail -c1 "$1")" ] || echo >> "$1"; }
backup() {  # backup <base> <relative path>
    [ -e "$1/$2" ] || return 0
    [ -e "$1/$BK/$2" ] && return 0
    mkdir -p "$(dirname "$1/$BK/$2")" && cp -p "$1/$2" "$1/$BK/$2"
}
# Line edits are done with grep + "cat > file" (writes through the existing file). No "sed -i": some
# kitchen shells don't apply it, and it reported success while leaving my_product unpatched.
# DNA ends every file_contexts line with a NUL byte. grep calls such a file binary - toybox prints "Binary file
# ... matches" instead of the lines, busybox drops the NULs - so NULs are swapped for \001 around every grep.
strip_lines() {  # strip_lines <file> <grep -F pattern file>
    tr '\000' '\001' < "$1" | grep -v -F -f "$2" | tr '\001' '\000' > "$1.ohap.tmp"
    cat "$1.ohap.tmp" > "$1" && rm -f "$1.ohap.tmp"
}
count_lines() {  # count_lines <file> <grep -F pattern file>: lines of <file> matching any pattern
    tr '\000' '\001' < "$1" | grep -c -F -f "$2"
}
append_lines() {  # append_lines <file> <lines file>: in the file's own format (NUL before each newline or not)
    ends_nl "$1"
    if [ "$(tail -c 2 "$1" | od -An -tx1 | tr -d ' \n')" = 000a ]; then
        while IFS= read -r l; do printf '%s\0\n' "$l"; done < "$2" >> "$1"
    else
        cat "$2" >> "$1"
    fi
}
drop_line() {  # drop_line <file> <fixed string>
    grep -v -F "$2" "$1" > "$1.ohap.tmp"
    cat "$1.ohap.tmp" > "$1" && rm -f "$1.ohap.tmp"
    grep -q -F "$2" "$1" && die "could not edit $1 (is the project writable?)"
    return 0
}
count_present() {  # count_present <dir> <list file>
    c=0
    while read -r n; do [ -f "$1/$n" ] && c=$((c + 1)); done < "$2"
    echo $c
}
add_feature() {  # add_feature <xml> <line> <grep -F needle> <closing tag>: insert before the closing tag
    grep -q -F "$3" "$1" && return 0
    L=$(grep -n -F "$4" "$1" | tail -n1 | cut -d: -f1)
    [ -n "$L" ] || die "no $4 in $1"
    head -n $((L - 1)) "$1" > "$1.ohap.tmp"
    printf '%s\n' "$2" >> "$1.ohap.tmp"
    tail -n +$L "$1" >> "$1.ohap.tmp"
    cat "$1.ohap.tmp" > "$1" && rm -f "$1.ohap.tmp"
    grep -q -F "$3" "$1" || die "could not edit $1 (is the project writable?)"
    return 0
}
put() {  # put <source dir in kit> <relative path> <mode> <md5>: copy and verify
    mkdir -p "$(dirname "$P/$2")" && cp -f "$KIT/$1/$2" "$P/$2" && chmod "$3" "$P/$2" || die "copy $2 failed"
    [ "$(sum "$P/$2")" = "$4" ] || die "$2 copy is corrupt"
}
st() {  # st <exit code of the check> <description>
    if [ "$1" = 0 ]; then echo "  [ok]   $2"; else echo "  [FAIL] $2"; VERIFY_FAILED=1; fi
}
verify() {  # prints one line per item, returns 1 if anything is not in the expected state
    VERIFY_FAILED=0
    added=$(grep -c . "$KIT/lists/odm_firmware_add.txt")
    have=$(count_present "$P/odm/firmware" "$KIT/lists/odm_firmware_add.txt")
    left=$(count_present "$P/odm/firmware" "$KIT/lists/odm_firmware_remove.txt")
    fadd=$(grep -c . "$KIT/config/odm_fs_config.add")
    fsn=$(count_lines "$FS" "$KIT/config/odm_fs_config.add")
    fcn=$(count_lines "$FC" "$KIT/config/odm_file_contexts.add")
    okc=$( (cd "$P/odm/firmware" && md5sum -c "$KIT/lists/library.md5" 2>/dev/null) | grep -c ": OK$" )
    [ "$have" = "$added" ] && [ "$okc" = "$added" ]
    st $? "odm/firmware OnePlus effects $have/$added present, $okc/$added match this kit"
    [ "$left" = 0 ];         st $? "odm/firmware Xiaomi RTP effects left: $left"
    [ "$(sum "$P/$SVC_REL")" = "$SVC_MD5" ]
    st $? "odm vibratorfeature-hal-service = O-Haptics vibrator HAL ($SVC_REL)"
    [ "$(sum "$P/$XM_RC_REL")" = "$XM_RC_MD5" ] && [ "$(sum "$P/$XM_VINTF_REL")" = "$XM_VINTF_MD5" ]
    st $? "odm Xiaomi init rc and VINTF manifest are stock"
    old=0
    for f in $OLD_FILES; do [ -e "$P/$f" ] && old=1; done
    [ $old = 0 ]; st $? "odm has no files left from earlier kits"
    [ "$fsn" = "$fadd" ] && [ "$fcn" = "$fadd" ]; st $? "odm fs_config $fsn/$fadd, file_contexts $fcn/$fadd"
    if [ -n "$SCALE_PROP" ]; then
        [ "$(grep -c -F "$SCALE_PROP" "$P/odm/etc/build.prop")" = 1 ]; st $? "odm/etc/build.prop $SCALE_PROP"
    fi
    e=$(sum "$MP/$EW_REL")
    [ "$e" = "$EW_NEW_MD5" ]; st $? "my_product effect_waveform.xml md5 $e"
    ! grep -q -F "oplus.software.audio.haptic_channel_support" "$MP/$FEAT_REL"
    st $? "my_product feature xml without oplus.software.audio.haptic_channel_support"
    grep -q -F "$LUXUN_GREP" "$MP/$FEATA_REL"
    st $? "my_product oplus.feature.android.xml with vibrator_luxunvibrator"
    grep -q -F "$HOLD_GREP" "$MP/$FEATA_REL"
    st $? "my_product feature enables the continuous navigation hold effect"
    return $VERIFY_FAILED
}

REVERT=0
CHECK=0
[ "$1" = "--revert" ] && { REVERT=1; shift; }
[ "$1" = "--check" ] && { CHECK=1; shift; }
[ -n "$1" ] || die "usage: sh apply.sh [--check|--revert] <project> [my_product_project]"
echo "O-Haptics kit $KIT_ID"
P=$(cd "$1" && pwd) || die "no such dir: $1"
MP=$(cd "${2:-$1}" && pwd) || die "no such dir: $2"
[ -d "$P/odm/firmware" ] || die "$P/odm/firmware not found (unpack odm.img first)"
[ -d "$MP/my_product/etc" ] || die "$MP/my_product not found (unpack my_product.img first)"
FS=$(find "$P" -maxdepth 2 -name odm_fs_config -type f | grep -v "/$BK/" | head -n1)
FC=$(find "$P" -maxdepth 2 -name odm_file_contexts -type f | grep -v "/$BK/" | head -n1)
[ -n "$FS" ] && [ -n "$FC" ] || die "odm_fs_config / odm_file_contexts not found under $P"
FS_REL=${FS#$P/}
FC_REL=${FC#$P/}

if [ $REVERT = 1 ]; then
    [ -d "$P/$BK" ] || die "no backup in $P/$BK"
    while read -r n; do rm -f "$P/odm/firmware/$n"; done < "$KIT/lists/odm_firmware_add.txt"
    for f in $OLD_FILES; do rm -f "$P/$f"; done
    for base in "$P" "$MP"; do
        [ -d "$base/$BK" ] || continue
        (cd "$base/$BK" && find . -type f) | while read -r f; do cp -p "$base/$BK/$f" "$base/$f"; done
    done
    put stock "$SVC_REL" 755 "$STOCK_MD5"            # Xiaomi's own HAL binary back
    put stock "$XM_RC_REL" 644 "$XM_RC_MD5"
    put stock "$XM_VINTF_REL" 644 "$XM_VINTF_MD5"
    echo "reverted: stock files restored from $P/$BK"
    exit 0
fi

if [ $CHECK = 1 ]; then
    echo "O-Haptics state of $P ($MP):"
    verify && echo "all in place - ready to repack" || echo "NOT complete - run: sh $0 $P $MP"
    exit 0
fi

# ---- checks ---------------------------------------------------------------------------
NOW=$(sum "$P/$SVC_REL")
known=0
for m in $STOCK_MD5 $OLD_PATCHED_MD5S $SVC_MD5; do [ "$NOW" = "$m" ] && known=1; done
grep -q -F "$SVC_MARKER" "$P/$SVC_REL" 2>/dev/null && known=1   # an earlier build of this HAL
[ $known = 1 ] || die "$SVC_REL md5 $NOW is not the Xiaomi HAL this kit was built for ($STOCK_MD5)"
EW_NOW=$(sum "$MP/$EW_REL")
ew_known=0
for m in $EW_BASE_MD5S $EW_NEW_MD5; do [ "$EW_NOW" = "$m" ] && ew_known=1; done
[ "$ew_known" = 1 ] || [ "$FORCE" = 1 ] || \
    die "$EW_REL differs from the one the remap was built from (run with FORCE=1 to replace it anyway)"
[ -f "$MP/$FEAT_REL" ] || die "$FEAT_REL not found"
[ -f "$MP/$FEATA_REL" ] || die "$FEATA_REL not found"

# ---- backups (the HAL files need none: the kit carries Xiaomi's stock ones) ----------------------
for rel in odm/etc/build.prop "$FS_REL" "$FC_REL"; do backup "$P" "$rel"; done
while read -r n; do backup "$P" "odm/firmware/$n"; done < "$KIT/lists/odm_firmware_remove.txt"
backup "$MP" "$EW_REL"
backup "$MP" "$FEAT_REL"
backup "$MP" "$FEATA_REL"

# ---- odm: Xiaomi RTP effects out, OnePlus library in --------------------------------------------
removed=0
while read -r n; do
    [ -e "$P/odm/firmware/$n" ] && { rm -f "$P/odm/firmware/$n"; removed=$((removed + 1)); }
done < "$KIT/lists/odm_firmware_remove.txt"
cp -f "$KIT"/odm/firmware/*_RTP.bin "$P/odm/firmware/" || die "copy firmware failed"

# ---- odm: the O-Haptics HAL at Xiaomi's path, Xiaomi's rc and manifest stock --------------------
put stock "$XM_RC_REL" 644 "$XM_RC_MD5"
put stock "$XM_VINTF_REL" 644 "$XM_VINTF_MD5"
put . "$SVC_REL" 755 "$SVC_MD5"
for f in $OLD_FILES; do rm -f "$P/$f"; done

strip_lines "$FS" "$KIT/config/odm_fs_config.remove"
strip_lines "$FC" "$KIT/config/odm_file_contexts.remove"
append_lines "$FS" "$KIT/config/odm_fs_config.add"
append_lines "$FC" "$KIT/config/odm_file_contexts.add"

if [ -n "$SCALE_PROP" ] && [ -f "$P/odm/etc/build.prop" ]; then
    drop_line "$P/odm/etc/build.prop" "${SCALE_PROP%%=*}="
    ends_nl "$P/odm/etc/build.prop"; echo "$SCALE_PROP" >> "$P/odm/etc/build.prop"
fi

# ---- my_product: remapped effect table, no audio-coupled haptics flag ----------------------------
cp -f "$KIT/$EW_REL" "$MP/$EW_REL" || die "copy effect_waveform.xml failed"
drop_line "$MP/$FEAT_REL" "oplus.software.audio.haptic_channel_support"
# OnePlus gates the power-key/lock haptic, the long-press effect and every COUI widget haptic
# (sliders, seekbars, pickers, switches) behind this feature. Ports often drop the line.
add_feature "$MP/$FEATA_REL" "$LUXUN" "$LUXUN_GREP" "</oplus-config>"
add_feature "$MP/$FEATA_REL" "$HOLD" "$HOLD_GREP" "</oplus-config>"

echo "removed $removed Xiaomi RTP effects; verifying $P ($MP):"
if verify; then
    echo "backup: $P/$BK"
    echo "done - repack odm.img and my_product.img"
else
    die "the project is NOT complete (see [FAIL] above) - do not repack"
fi
