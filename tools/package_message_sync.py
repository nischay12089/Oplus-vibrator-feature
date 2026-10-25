"""Build a data-only notification haptic-channel update for the connected ColorOS ROM."""
from pathlib import Path
import hashlib, json, re, sys, zipfile
import xml.etree.ElementTree as ET
sys.path.insert(0,str(Path(__file__).resolve().parent))
from build_bridge import convert_haptic_track, apply_gain
root=Path(__file__).resolve().parents[1]
base=root/'work/coloros'
kit=root/'out/ColorOS_message_tone_sync'
xml=(base/'message-base.xml').read_bytes()
text=xml.decode('utf-8')
raw=base/'notification_raw'
tracks=sorted(raw.glob('notification_*.s16'))
assert len(tracks)==46
log=(base/'notification-extract.log').read_text()
assert len(re.findall(r'^OK notification_\d+ rate=48000 channels=2/2 haptic=1 frames=\d+',log,re.M))==46
names={n:e for n,e in re.findall(r'<ringtone_effect ringtone_name="([^"]+)" effect_id="(\d+)"',text)}
used={int(x) for x in re.findall(r'waveform_id="(\d+)"',text)}
updates={}; report=[]
fw=kit/'odm/firmware'; fw.mkdir(parents=True,exist_ok=True)
for index,track in enumerate(tracks):
 # Stay below the HAL's +1000 soft-variant namespace. Using 3000 here would
 # accidentally become the Gentle variant of ringtone waveform 2000.
 name=track.stem; eid=names[name]; wf=800+index
 assert wf not in used
 pcm=apply_gain(convert_haptic_track(str(track),43.0),0.60,allow_waveshape=False)
 dur=round(len(pcm)/24)
 assert abs(len(pcm)/24000-track.stat().st_size/96000)<1/24000
 assert abs(dur-track.stat().st_size/96)<=0.51
 filename=f'{wf}_msg{index+1:03d}_RTP.bin'
 (fw/filename).write_bytes(pcm)
 updates[eid]=(wf,dur)
 report.append(dict(tone=name,effect_id=int(eid),waveform_id=wf,duration_ms=dur,
                    raw_haptic_frames=track.stat().st_size//2,pcm_samples=len(pcm),gain=0.60))
assert len(updates)==46
changed=[]
def replace(m):
 tag=m[0]; eid=re.search(r'effect_id="(\d+)"',tag)[1]
 if eid not in updates:return tag
 wf,dur=updates[eid]; changed.append(eid)
 tag=re.sub(r'waveform_id="\d+"',f'waveform_id="{wf}"',tag)
 return re.sub(r'duration="\d+"',f'duration="{dur}"',tag)
fixed=re.sub(r'<effect [^>]*/>',replace,text)
assert len(changed)==46
ET.fromstring(fixed)
rel='my_product/etc/vibrator/effect_waveform.xml'
(kit/rel).parent.mkdir(parents=True,exist_ok=True)
(kit/rel).write_bytes(fixed.encode())
base_md5=hashlib.md5(xml).hexdigest(); new_md5=hashlib.md5(fixed.encode()).hexdigest()
# All other effect tags and ringtone/CTS mapping entries remain byte-for-byte unchanged.
assert re.sub(r'<effect [^>]*/>',lambda m:'' if re.search(r'effect_id="(\d+)"',m[0])[1] in updates else m[0],text)==re.sub(r'<effect [^>]*/>',lambda m:'' if re.search(r'effect_id="(\d+)"',m[0])[1] in updates else m[0],fixed)
(kit/'config').mkdir(exist_ok=True)
(kit/'config/odm_fs_config.add').write_text(''.join(f'odm/firmware/{p.name} 0 0 0644\n' for p in sorted(fw.iterdir())),newline='\n')
(kit/'config/odm_file_contexts.add').write_text(''.join('/odm/firmware/'+p.name.replace('.',r'\.')+' u:object_r:vendor_file:s0\n' for p in sorted(fw.iterdir())),newline='\n')
files=[p.relative_to(kit).as_posix() for p in sorted(fw.iterdir())]+[rel]
(kit/'files.md5').write_text(''.join(hashlib.md5((kit/f).read_bytes()).hexdigest()+'  '+f+'\n' for f in files),newline='\n')
(kit/'timing_report.json').write_text(json.dumps(report,indent=2)+'\n',newline='\n')
script=(root/'tools/message_sync_apply_template.sh').read_text().replace('@BASE_MD5@',base_md5).replace('@NEW_MD5@',new_md5)
(kit/'apply.sh').write_text(script,encoding='utf-8',newline='\n')
(kit/'original_effect_waveform.xml').write_bytes(xml)
(kit/'README.md').write_text('''# ColorOS message/notification tone sync

For POCO F7 with ColorOS PHY110_16.0.10.500(CN01), the working standalone O-Haptics HAL, and the previous ringtone fix.

46 installed notification tones (notification_001..notification_046) each receive their own haptic track extracted from the audio file's ANDROID_HAPTIC=1 channel. They no longer share generic fallback patterns. The original timing and pauses remain; carrier shifts +43 Hz to this motor, with the existing 0.60 linear track trim. No time stretching or per-track loudness normalization.

Apply on your phone's unpacked ColorOS partition project, as root:

    sh /sdcard/Download/ColorOS_message_tone_sync/apply.sh /data/DNA/YOUR_PROJECT

For separate odm and my_product projects, supply the my_product project as the second argument. Require [ok], repack and flash BOTH odm.img and my_product.img, and reboot. XML and supported-waveform tables are cached at startup. Settings must use the sync/follow-tone vibration mode and the relevant app notification channel must allow sound and vibration.

This update preserves ringtone and CTS effect mappings, all HAL strength constants, the CTS feature XML, libcts_hook.so, and stock rc/manifest. It removes audio.haptic_channel_support if present so the framework uses the motor path. No HAL, jar, APK, library, or module is included. Custom/imported sounds and app-provided vibration patterns without a matching built-in tone are not converted.

Run `sh apply.sh --check PROJECT` to verify or `sh apply.sh --revert PROJECT` to restore the pre-update files before repacking. Backups are in ohaptics_message_sync_backup. The installer refuses an unrelated waveform XML and preserves DNA's NUL-terminated file_contexts format.

Validation: 46/46 source files declare haptic channels; Android decoder confirms 48 kHz, stereo, one haptic channel. PCM sample counts preserve source duration to within one 24 kHz sample; rounded XML durations differ by at most 0.51 ms. All unrelated XML entries are unchanged. Actual message arrival timing still requires confirmation after flashing; per-app channel behavior can override the system default.
''',encoding='utf-8',newline='\n')
archive=root/'out/ColorOS_message_tone_sync.zip'
with zipfile.ZipFile(archive,'w',zipfile.ZIP_DEFLATED) as z:
 for p in sorted(kit.rglob('*')):
  if p.is_file():z.write(p,'ColorOS_message_tone_sync/'+p.relative_to(kit).as_posix())
print('46 exact-duration notification tracks; only their waveform/duration attributes changed.')
print('XML',base_md5,'->',new_md5)
print(archive,'SHA256',hashlib.sha256(archive.read_bytes()).hexdigest())
