import time,json,base64,zlib,io
from pathlib import Path
from PIL import Image
from board_client import BoardClient
out=Path(__file__).resolve().parents[1]/'captures'/time.strftime('final_%Y%m%d_%H%M%S');out.mkdir(parents=True)
records=[]
c=BoardClient('COM14')
try:
 for trial in range(3):
  deadline=time.monotonic()+60
  while not c.request('status')['ready']:
   assert time.monotonic()<deadline
   time.sleep(.2)
  if trial:
   assert c.request('restart')['ok'];c.close();time.sleep(.7)
   c=BoardClient('COM14')
   early=c.request('status'); assert not early['ready'],early
   refused=c.request('camera');assert not refused['ok'],refused
  deadline=time.monotonic()+60;phases=[]
  while True:
   s=c.request('status');assert s['leds'][0]==s['ready'],s
   phases.append(s['phase'])
   if s['ready']:break
   assert s['phase']!='error',s
   assert time.monotonic()<deadline
   time.sleep(.2)
  assert s['warmup_ms']>=4000 and s['gain_limit']==0,s
  started=time.monotonic();r=c.request('camera');assert r['ok'],r
  raw=base64.b64decode(r['jpeg']);assert zlib.crc32(raw)==r['crc32']
  im=Image.open(io.BytesIO(raw));im.load();assert im.size==(1600,1200)
  (out/f'trial{trial+1}.jpg').write_bytes(raw)
  record={**{k:v for k,v in r.items() if k!='jpeg'},'trial':trial+1,'phases':phases,'photo_seconds':round(time.monotonic()-started,2)}
  records.append(record);print(json.dumps(record),flush=True)
finally:
 c.close();(out/'report.json').write_text(json.dumps(records,indent=2),encoding='utf-8');print(out,flush=True)
