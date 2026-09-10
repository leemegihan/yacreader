# YACReader synthetic recovery worker v1
# Private fault fixture: no models, GPU kernels, original media or network.
import base64,hashlib,json,os,sys,time
from pathlib import Path
args=sys.argv
manifest=Path(args[args.index('--manifest')+1])
device=args[args.index('--device')+1]
mode=os.environ['YACREADER_FAULT_MODE']
pages=json.loads(manifest.read_text(encoding='utf8'))['pages']
hashes=[hashlib.sha256(Path(p['image']).read_bytes()).hexdigest() for p in pages]

def put(path,obj):
 path=Path(path);temp=path.with_suffix('.tmp');temp.write_text(json.dumps(obj),encoding='utf8')
 for attempt in range(51):
  try:os.replace(temp,path);return
  except PermissionError:
   if attempt==50:raise
   time.sleep(.01)
selected=range(len(pages))
if device=='gpu:0' and mode!='all-output-failure':selected=[1]
responses={}
for i in selected:
 lines=[] if mode=='blank-partial' and device=='gpu:0' else [{'text':hashes[i],'confidence':99,'box':[1,1,20,20],'language':'jpn'}]
 responses[i]=json.dumps({'version':1,'engine':'paddle-regions','device':device,'language':'auto','lines':lines})
if mode=='invalid-partial' and device=='gpu:0':responses[0]='invalid JSON'
with open(base64.b64decode(os.environ['YACREADER_FAULT_TRACE_BASE64']).decode('utf8'),'a',encoding='utf8') as f:
 f.write(json.dumps({'device':device,'images':hashes,'outputs':[p['output'] for p in pages],
  'responses':{str(i):base64.b64encode(raw.encode('utf8')).decode('ascii') for i,raw in responses.items()}})+'\n')
for i,raw in responses.items():
 if raw=='invalid JSON':Path(pages[i]['output']).write_text(raw,encoding='utf8')
 else:put(pages[i]['output'],json.loads(raw))
if device=='gpu:0':
 if mode=='cancel-after-page':
  put(manifest.parent/'progress.json',{'stage':'recognize'})
  time.sleep(30)
 sys.exit(73)
sys.exit(74 if mode=='partial-cpu-failure' else 0)
