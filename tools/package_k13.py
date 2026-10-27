"""Adapt the complete approved kit to K13 Turbo Pro tone durations, retaining the HAL."""
from pathlib import Path
import hashlib,json,re,shutil,sys,zipfile
import numpy as np
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'work/audio_deps'))
import soundfile as sf
BASE=ROOT/'out/coloros_release/rom'; INPUT=ROOT/'work/k13'; OUT=ROOT/'out/k13_release'; ROM=OUT/'rom'
BUILD='PLE110_16.0.10.600(CN01)'
SVC='odm/bin/hw/vendor.xiaomi.hardware.vibratorfeature.service'; RC='odm/etc/init/vendor.xiaomi.hardware.vibratorfeature.service.rc'; VINTF='odm/etc/vintf/manifest/vendor.xiaomi.hardware.vibratorfeature.service.xml'; EW='my_product/etc/vibrator/effect_waveform.xml'
md5=lambda p:hashlib.md5(Path(p).read_bytes()).hexdigest()
source_xml=(INPUT/'effect_waveform.xml').read_text(encoding='utf-8')
assert hashlib.md5(source_xml.encode()).hexdigest()=='cdcf0efc592a4e11c66138d20b3f8984'
shutil.copytree(BASE,ROM,dirs_exist_ok=True)
refs={n:e for n,e in re.findall(r'<ringtone_effect ringtone_name="([^"]+)" effect_id="(\d+)"',source_xml)}
tags={re.search(r'effect_id="(\d+)"',t)[1]:t for t in re.findall(r'<effect [^>]*/>',source_xml)}
updates={};report=[]
for category,expected in [('ringtones',58),('notifications',46)]:
 tones=sorted((INPUT/category).rglob('*.ogg'))
 assert len(tones)==expected
 for tone in tones:
  info=sf.info(tone)
  assert b'ANDROID_HAPTIC=1' not in tone.read_bytes()[:8192], 'This adapter expects the legacy K13 tones'
  eid=refs[tone.stem]; tag=tags[eid]; wf=int(re.search(r'waveform_id="(\d+)"',tag)[1])
  srcs=list((BASE/'odm/firmware').glob(f'{wf}_*_RTP.bin'));assert len(srcs)==1
  src=srcs[0]; authored=np.frombuffer(src.read_bytes(),dtype=np.int8)
  samples=int(round(info.frames*24000/info.samplerate))
  pcm=np.zeros(samples,dtype=np.int8); n=min(samples,len(authored));pcm[:n]=authored[:n]
  # Only a shortened, still-active tail gets a 4ms release to avoid an abrupt motor stop.
  if samples<len(authored) and samples>96 and abs(int(pcm[-1]))>1:
   pcm[-96:]=np.rint(pcm[-96:].astype(float)*np.linspace(1,0,96)).astype(np.int8)
  (ROM/'odm/firmware'/src.name).write_bytes(pcm.tobytes())
  duration=int(round(samples/24)); updates[eid]=duration
  report.append(dict(tone=tone.stem,category=category,effect_id=int(eid),waveform_id=wf,
                     audio_rate=info.samplerate,audio_frames=info.frames,duration_ms=duration,
                     source_haptic_samples=len(authored),output_haptic_samples=samples,
                     source='previous_authored_track_trim_or_silence_pad',
                     source_audio_sha256=hashlib.sha256(tone.read_bytes()).hexdigest()))
assert len(updates)==104
changed=[]
def patch(m):
 tag=m[0];eid=re.search(r'effect_id="(\d+)"',tag)[1]
 if eid not in updates:return tag
 changed.append(eid);return re.sub(r'duration="\d+"',f'duration="{updates[eid]}"',tag)
fixed=re.sub(r'<effect [^>]*/>',patch,source_xml)
assert len(changed)==104
(ROM/EW).write_text(fixed,encoding='utf-8',newline='\n')
files=sorted((ROM/'odm/firmware').glob('*_RTP.bin'));assert len(files)==281
library=''.join(md5(p)+'  '+p.name+'\n' for p in files)
(ROM/'lists/library.md5').write_text(library,newline='\n')
assert md5(ROM/SVC)=='4cd70c8825f3168889f6a725f9cc2566'
for wf in [1,2,7,105,107,111,362,900]:
 for src in (BASE/'odm/firmware').glob(f'{wf}_*_RTP.bin'):assert (ROM/'odm/firmware'/src.name).read_bytes()==src.read_bytes()
template=(ROOT/'tools/apply_template.sh').read_text(encoding='utf-8')
kit_id=hashlib.md5((BUILD+library+md5(ROM/EW)+md5(ROM/SVC)+template).encode()).hexdigest()[:8]
values={'KIT_ID':kit_id,'STOCK_MD5':md5(ROM/'stock'/SVC),'SVC_MD5':md5(ROM/SVC),
        'XM_RC_MD5':md5(ROM/'stock'/RC),'XM_VINTF_MD5':md5(ROM/'stock'/VINTF),
        'EW_BASE_MD5':md5(INPUT/'effect_waveform.xml'),'EW_NEW_MD5':md5(ROM/EW),
        'SCALE_PROP':'ro.odm.mm.vibrator.scaleStreaming=false'}
script=template
for key,value in values.items():script=script.replace('@'+key+'@',value)
script=script.replace('BK=ohaptics_backup','BK=ohaptics_k13_backup')
assert not re.search(r'@[A-Z_]+@',script)
(ROM/'apply.sh').write_text(script,newline='\n')
manifest=dict(kit_id=kit_id,device_base=BUILD,hal_md5=md5(ROM/SVC),xml_md5=md5(ROM/EW),
 firmware_files=281,ringtones=58,notifications=46,cts_gain=0.85,cts_waveform=900,
 audio_source='ordinary K13 audio, no embedded haptic channels',
 haptic_source='existing authored tracks; unchanged tempo; trim/silence padding to K13 file duration')
(ROM/'release_manifest.json').write_text(json.dumps(manifest,indent=2)+'\n',newline='\n')
(ROM/'k13_timing_report.json').write_text(json.dumps(report,indent=2)+'\n',newline='\n')
# Replace earlier timing reports so this release cannot claim the previous base's durations.
for cat in ['ringtones','notifications']:
 (ROM/f'{cat}_timing_report.json').write_text(json.dumps([r for r in report if r['category']==cat],indent=2)+'\n',newline='\n')
notes=(ROOT/'K13_INTEGRATION.md').read_text(encoding='utf-8').replace('@KIT_ID@',kit_id)
notes=re.sub(r'Kit [a-f0-9]{8}', 'Kit '+kit_id, notes)
(ROM/'ROM_INTEGRATION.md').write_text(notes,encoding='utf-8',newline='\n')
archive=OUT/'OHaptics_onyx_K13_Turbo_Pro.zip'
with zipfile.ZipFile(archive,'w',zipfile.ZIP_DEFLATED) as z:
 for p in sorted(ROM.rglob('*')):
  if p.is_file():z.write(p,'OHaptics_onyx/'+p.relative_to(ROM).as_posix())
(OUT/'SHA256SUMS').write_text(hashlib.sha256(archive.read_bytes()).hexdigest()+'  '+archive.name+'\n',newline='\n')
print(json.dumps(manifest,indent=2));print(archive)
