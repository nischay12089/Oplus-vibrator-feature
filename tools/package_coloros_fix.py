from pathlib import Path
import re, hashlib, shutil, json, zipfile
import numpy as np
from scipy.signal import hilbert, butter, sosfiltfilt
root=Path(__file__).resolve().parents[1]
base=root/'work/coloros'
built=root/'out/coloros/rom'
kit=root/'out/ColorOS_haptic_sync_fix'
kit.mkdir(parents=True,exist_ok=True)
old=(base/'effect_waveform.xml').read_text(encoding='utf-8')
new=(built/'my_product/etc/vibrator/effect_waveform.xml').read_text(encoding='utf-8')
names={p.stem for p in (base/'haptic_raw').glob('*.s16')}
refs={n:e for n,e in re.findall(r'<ringtone_effect ringtone_name="([^"]+)" effect_id="(\d+)"',old) if n in names}
assert len(refs)==58
newtags={re.search(r'effect_id="(\d+)"',t)[1]:t for t in re.findall(r'<effect [^>]*/>',new)}
selected=set(refs.values())
fixed=re.sub(r'<effect [^>]*/>',lambda m:newtags[re.search(r'effect_id="(\d+)"',m[0])[1]] if re.search(r'effect_id="(\d+)"',m[0])[1] in selected else m[0],old)
xml=kit/'my_product/etc/vibrator/effect_waveform.xml'
xml.parent.mkdir(parents=True,exist_ok=True)
xml.write_bytes(fixed.encode())
fw=kit/'odm/firmware'; fw.mkdir(parents=True,exist_ok=True)
report=[]
for name,eid in sorted(refs.items()):
    tag=newtags[eid]; wf=int(re.search(r'waveform_id="(\d+)"',tag)[1]); duration=int(re.search(r'duration="(\d+)"',tag)[1])
    src=next((built/'odm/firmware').glob(f'{wf}_*'))
    rawbytes=(base/'haptic_raw'/f'{name}.s16').stat().st_size
    assert abs(src.stat().st_size/24000 - rawbytes/96000) < 1/24000
    assert abs(duration - rawbytes/96) <= 0.51
    shutil.copy2(src,fw/src.name)
    report.append({'ringtone':name,'effect':int(eid),'waveform':wf,'duration_ms':duration,'audio_haptic_samples':rawbytes//2})
# The home-hold/CTS ramp is also used by COUI. Preserve its envelope and length,
# but drive it with one resonant carrier, rather than shifting its spectral centroid
# (its weak ramp and final pulse have different carriers).
source=base/'original_362_RTP.bin'
x=np.frombuffer(source.read_bytes(),dtype=np.int8).astype(float)
envelope=sosfiltfilt(butter(4,80,fs=24000,output='sos'),np.abs(hilbert(x)))
envelope=np.maximum(envelope,0)
y=envelope*np.sin(2*np.pi*168.7*np.arange(len(x))/24000)
y*=np.max(np.abs(x))/max(np.max(np.abs(y)),1e-9)
(fw/'362_op362_RTP.bin').write_bytes(np.clip(np.rint(y),-127,127).astype(np.int8).tobytes())
shutil.copy2(base/'effect_waveform.xml',kit/'original_effect_waveform.xml')
(kit/'timing_report.json').write_text(json.dumps(report,indent=2)+'\n',newline='\n')
files=[str(p.relative_to(kit)).replace('\\','/') for p in sorted(fw.iterdir())]+['my_product/etc/vibrator/effect_waveform.xml']
(kit/'files.md5').write_text(''.join(hashlib.md5((kit/f).read_bytes()).hexdigest()+'  '+f+'\n' for f in files),newline='\n')
(kit/'config').mkdir(exist_ok=True)
(kit/'config/odm_fs_config.add').write_text(''.join(f'odm/firmware/{p.name} 0 0 0644\n' for p in sorted(fw.iterdir())),newline='\n')
(kit/'config/odm_file_contexts.add').write_text(''.join(f'/odm/firmware/{p.name.replace(".",r"\.")} u:object_r:vendor_file:s0\n' for p in sorted(fw.iterdir())),newline='\n')
print('base',hashlib.md5((base/'effect_waveform.xml').read_bytes()).hexdigest(),'new',hashlib.md5(xml.read_bytes()).hexdigest())
print('58 unique tracks verified: exact frame counts; rounded XML durations within 0.51 ms; only these 58 effect entries changed')
shutil.copy2(root/'tools/coloros_apply_template.sh',kit/'apply.sh')
shutil.copy2(base/'README.md',kit/'README.md')
archive=root/'out/ColorOS_haptic_sync_fix.zip'
with zipfile.ZipFile(archive,'w',zipfile.ZIP_DEFLATED,compresslevel=6) as z:
    for p in sorted(kit.rglob('*')):
        if p.is_file():
            z.write(p,'ColorOS_haptic_sync_fix/'+p.relative_to(kit).as_posix())
print(archive,hashlib.sha256(archive.read_bytes()).hexdigest())
