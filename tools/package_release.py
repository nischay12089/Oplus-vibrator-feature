"""Assemble the tested HAL and all ColorOS data fixes into one partition kit.

Uses local extraction outputs when present; a published out/coloros_release/rom
also contains the inputs needed to assemble the same release again.
"""
from pathlib import Path
import hashlib,json,re,shutil,zipfile

ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'out/coloros_release'
ROM=OUT/'rom'
BASE=ROOT/'out/rom'
SVC='odm/bin/hw/vendor.xiaomi.hardware.vibratorfeature.service'
RC='odm/etc/init/vendor.xiaomi.hardware.vibratorfeature.service.rc'
VINTF='odm/etc/vintf/manifest/vendor.xiaomi.hardware.vibratorfeature.service.xml'
EW='my_product/etc/vibrator/effect_waveform.xml'
CTS_ID=900
CTS_GAIN=0.85
md5=lambda p:hashlib.md5(Path(p).read_bytes()).hexdigest()

def first_existing(*paths):
    for p in paths:
        if p.is_file():return p
    raise FileNotFoundError('Required release input missing: '+', '.join(map(str,paths)))

# Read inputs before copying the original kit over an existing release directory.
xml_source=first_existing(ROOT/'out/ColorOS_message_tone_sync'/EW,ROM/EW)
xml=xml_source.read_text(encoding='utf-8')
ring_dir=ROOT/'out/ColorOS_haptic_sync_fix/odm/firmware'
msg_dir=ROOT/'out/ColorOS_message_tone_sync/odm/firmware'
extra={}
for i in range(2000,2058):
    choices=list(ring_dir.glob(f'{i}_*_RTP.bin'))+list((ROM/'odm/firmware').glob(f'{i}_*_RTP.bin'))
    if not choices:raise FileNotFoundError(f'ringtone waveform {i}')
    extra[choices[0].name]=choices[0].read_bytes()
for i in range(800,846):
    choices=list(msg_dir.glob(f'{i}_*_RTP.bin'))+list((ROM/'odm/firmware').glob(f'{i}_*_RTP.bin'))
    if not choices:raise FileNotFoundError(f'notification waveform {i}')
    extra[choices[0].name]=choices[0].read_bytes()
source=first_existing(ring_dir/'362_op362_RTP.bin',ROM/'odm/firmware/362_op362_RTP.bin')
source_pcm=source.read_bytes()
assert md5(source)=='a2a8f2e906e0f3398c67471f808809ea', 'unexpected CTS source; do not double-trim'
extra['362_op362_RTP.bin']=source_pcm
trimmed=bytes(round((b if b<128 else b-256)*CTS_GAIN)&255 for b in source_pcm)
extra[f'{CTS_ID}_cts_hold_RTP.bin']=trimmed
assert len(trimmed)==14400
assert max(abs(b if b<128 else b-256) for b in trimmed)==73
assert not re.search(r'waveform_id="900"',xml) or 'effect_id="527" waveform_id="900"' in xml
changed=[]
def cts(m):
    tag=m[0]
    if re.search(r'effect_id="527"(?:\s|>)',tag):
        changed.append(tag)
        return re.sub(r'waveform_id="\d+"',f'waveform_id="{CTS_ID}"',tag)
    return tag
fixed=re.sub(r'<effect [^>]*/>',cts,xml)
assert len(changed)==1
# This only changes the CTS mapping; duration, usage, ringtone and message entries stay untouched.
assert re.sub(r'<effect effect_id="527"[^>]*/>','',fixed)==re.sub(r'<effect effect_id="527"[^>]*/>','',xml)

ROM.mkdir(parents=True,exist_ok=True)
shutil.copytree(BASE,ROM,dirs_exist_ok=True)
for name,data in extra.items():(ROM/'odm/firmware'/name).write_bytes(data)
(ROM/EW).write_bytes(fixed.encode())
assert md5(ROM/SVC)=='4cd70c8825f3168889f6a725f9cc2566'
assert md5(ROM/'stock'/RC)=='a1c78826c1f966a7cefc27b42d2764ac'
assert md5(ROM/'stock'/VINTF)=='4fc1ff4e78e7793da1fc93411009bd26'
files=sorted((ROM/'odm/firmware').glob('*_RTP.bin'))
ids=[int(p.name.split('_')[0]) for p in files]
assert len(ids)==len(set(ids))==281
assert {int(i) for i in re.findall(r'waveform_id="(\d+)"',fixed)}<=set(ids)
# New ordinary effects must not alias another waveform's +1000 Gentle variant.
assert not ({800+i for i in range(46)}|{CTS_ID}) & {i+1000 for i in ids}
for f in ('odm_fs_config','odm_file_contexts'):
    p=ROM/'config'/f'{f}.add'
    base_lines=(BASE/'config'/f'{f}.add').read_text().splitlines()
    owned=[l for l in base_lines if not l.startswith(('odm/firmware/','/odm/firmware/'))]
    if f=='odm_fs_config':lines=[f'odm/firmware/{p.name} 0 0 0644' for p in files]+owned
    else:lines=['/odm/firmware/'+re.escape(p.name)+' u:object_r:vendor_file:s0' for p in files]+owned
    p.write_text('\n'.join(lines)+'\n',newline='\n')
    old=(BASE/'config'/f'{f}.remove').read_text()
    if f=='odm_fs_config':old+=''.join(f'odm/firmware/{p.name} \n' for p in files)
    else:old+=''.join('/odm/firmware/'+n+' \n' for p in files for n in dict.fromkeys([re.escape(p.name),p.name]))
    (ROM/'config'/f'{f}.remove').write_text(''.join(dict.fromkeys(old.splitlines(keepends=True))),newline='\n')
(ROM/'lists/odm_firmware_add.txt').write_text(''.join(p.name+'\n' for p in files),newline='\n')
library=''.join(md5(p)+'  '+p.name+'\n' for p in files)
(ROM/'lists/library.md5').write_text(library,newline='\n')
template=(ROOT/'tools/apply_template.sh').read_text()
kit_id=hashlib.md5((library+md5(ROM/SVC)+md5(ROM/EW)+template).encode()).hexdigest()[:8]
accepted=['0c868aa6e842311e81d54d3c73e39d33','6649abae456c949f962b8b0cffa6006d',
          '6d626fe2012f93fde4ac837e92724f5b','5b87a9532706acefe67f6e2ff060fc23',md5(ROM/EW)]
values={'KIT_ID':kit_id,'STOCK_MD5':md5(ROM/'stock'/SVC),'SVC_MD5':md5(ROM/SVC),
        'XM_RC_MD5':md5(ROM/'stock'/RC),'XM_VINTF_MD5':md5(ROM/'stock'/VINTF),
        'EW_BASE_MD5':accepted[0],'EW_NEW_MD5':md5(ROM/EW),
        'SCALE_PROP':'ro.odm.mm.vibrator.scaleStreaming=false'}
script=template
for key,value in values.items():script=script.replace('@'+key+'@',value)
script=script.replace('EW_BASE_MD5S="'+accepted[0]+'"','EW_BASE_MD5S="'+' '.join(accepted)+'"')
assert not re.search(r'@[A-Z_]+@',script)
(ROM/'apply.sh').write_text(script,newline='\n')
manifest=dict(kit_id=kit_id,hal_md5=md5(ROM/SVC),xml_md5=md5(ROM/EW),
              ringtones=58,notifications=46,firmware_files=len(files),cts_waveform=CTS_ID,
              cts_gain=CTS_GAIN,cts_peak_before=86,cts_peak_after=73,cts_duration_ms=600,
              xml_accepted_md5=accepted)
(ROM/'release_manifest.json').write_text(json.dumps(manifest,indent=2)+'\n',newline='\n')
for category,src in [('ringtones',ROOT/'out/ColorOS_haptic_sync_fix/timing_report.json'),
                     ('notifications',ROOT/'out/ColorOS_message_tone_sync/timing_report.json')]:
    if src.is_file():shutil.copy2(src,ROM/f'{category}_timing_report.json')
notes=(ROOT/'COLOROS_INTEGRATION.md').read_text().replace('@KIT_ID@',kit_id)
notes=re.sub(r'Kit [a-f0-9]{8}', 'Kit '+kit_id, notes)
(ROM/'ROM_INTEGRATION.md').write_text(notes,encoding='utf-8',newline='\n')
archive=OUT/'OHaptics_onyx_ColorOS.zip'
with zipfile.ZipFile(archive,'w',zipfile.ZIP_DEFLATED,compresslevel=6) as z:
    for p in sorted(ROM.rglob('*')):
        if p.is_file():z.write(p,'OHaptics_onyx/'+p.relative_to(ROM).as_posix())
sha=hashlib.sha256(archive.read_bytes()).hexdigest()
(OUT/'SHA256SUMS').write_text(sha+'  '+archive.name+'\n',newline='\n')
print(json.dumps(manifest,indent=2))
print(archive,sha)
