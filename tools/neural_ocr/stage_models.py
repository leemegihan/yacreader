"""Build-time only: download official models, record all deployed bytes."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import sys
os.environ['PADDLE_PDX_MODEL_SOURCE'] = 'BOS'
os.environ['PADDLE_PDX_DISABLE_MODEL_SOURCE_CHECK'] = 'True'
from paddleocr import TextDetection, TextRecognition
root = Path(sys.argv[1])
common = dict(device='cpu', enable_mkldnn=False, cpu_threads=2)
for name, factory in [('PP-OCRv5_mobile_det', TextDetection), ('PP-OCRv5_server_rec', TextRecognition), ('korean_PP-OCRv5_mobile_rec', TextRecognition)]:
    factory(model_name=name, **common)
    source = Path.home() / '.paddlex/official_models' / name
    target = root / 'models' / name
    target.mkdir(parents=True, exist_ok=True)
    for file in ('inference.json', 'inference.pdiparams', 'inference.yml'):
        shutil.copy2(source / file, target / file)
manifest = []
for file in sorted((root / 'models').rglob('*')):
    if file.is_file():
        with file.open('rb') as stream:
            digest = hashlib.file_digest(stream, 'sha256').hexdigest()
        manifest.append({'path': file.relative_to(root).as_posix(), 'sha256': digest})
expected = json.loads(Path(__file__).with_name('models.lock.json').read_text(encoding='utf-8'))
if {f['path']: f['sha256'] for f in manifest} != {f['path']: f['sha256'] for f in expected}:
    raise RuntimeError('Downloaded model bytes differ from the tested model lock')
(root / 'models.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')
