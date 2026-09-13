"""Run identical pixels through page OCR and detected-region OCR, on CPU.

Manifest ground truth is only used after recognition, never for model selection.
Private images remain local. CI only supplies generated synthetic fixtures.
"""
import argparse
import hashlib
import re
import importlib.metadata
import json
import os
from pathlib import Path
import platform
import tempfile
import time

os.environ.setdefault('OMP_THREAD_LIMIT', '2')
os.environ.setdefault('PADDLE_PDX_DISABLE_MODEL_SOURCE_CHECK', 'True')
os.environ.setdefault('PADDLE_PDX_MODEL_SOURCE', 'BOS')

from PIL import Image, ImageDraw
from core import field_errors, normalized, ordered_boxes, tesseract, covered_fields


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--detector-model', choices=['PP-OCRv5_server_det', 'PP-OCRv5_mobile_det'], default='PP-OCRv5_server_det')
    parser.add_argument('--manifest', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--tesseract', type=Path, required=True)
    parser.add_argument('--tessdata', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    manifest = json.loads(args.manifest.read_text(encoding='utf-8'))
    from paddleocr import TextDetection, TextRecognition
    import numpy as np
    import torch
    from manga_ocr import MangaOcr
    torch.set_num_threads(min(4, os.cpu_count() or 1))
    start = time.perf_counter()
    common = dict(device='cpu', enable_mkldnn=False, cpu_threads=min(4, os.cpu_count() or 1))
    detector = TextDetection(model_name=args.detector_model, **common)
    recognizers = {lang: TextRecognition(model_name=model, **common) for lang, model in
                   [('jpn', 'PP-OCRv5_server_rec'), ('kor', 'korean_PP-OCRv5_mobile_rec')]}
    manga = MangaOcr(force_cpu=True)
    report = {'synthetic': manifest.get('synthetic', False), 'platform': platform.platform(), 'detector_model': args.detector_model,
              'model_load_seconds': time.perf_counter() - start,
              'versions': {p: importlib.metadata.version(p) for p in
                           ['paddleocr', 'paddlex', 'paddlepaddle', 'torch', 'manga-ocr', 'transformers']},
              'results': [], 'limitations': ['Language is supplied; this is not automatic language selection.',
                  'Synthetic layouts are not a real-manga accuracy benchmark.',
                  'Field matches measure text recovery, not safe metadata assignment.',
                  'Tesseract baseline chooses layout by its confidence, not ground truth.',
                  'Manga OCR has no calibrated confidence and can hallucinate.']}
    for case in manifest['cases']:
        if not re.fullmatch(r'[A-Za-z0-9_-]{1,80}', case['id']):
            raise ValueError('Case id must be a simple filename')
        if case['language'] not in ('jpn', 'kor'):
            raise ValueError('Unsupported reference language')
        image = Image.open(args.manifest.parent / case['image']).convert('RGB')
        if max(image.size) > 4000:
            raise ValueError('Prepare images and reference boxes at <=4000 pixels')
        lang = case['language']
        print(f"CASE {case['id']}", flush=True)
        with tempfile.TemporaryDirectory() as directory:
            start = time.perf_counter()
            passes = [tesseract(image, args.tesseract.resolve(), args.tessdata.resolve(), lang + '+eng', psm, directory) for psm in (11, 6)]
            whole = max(passes, key=lambda p: p['confidence'])
            methods = {'tesseract-page': dict(whole, seconds=time.perf_counter() - start)}
            start = time.perf_counter()
            detection = next(iter(detector.predict(np.array(image)[:, :, ::-1])))
            boxes = ordered_boxes(detection['dt_polys'], image.width, image.height)
            detection_seconds = time.perf_counter() - start
            coverage = covered_fields(case.get('boxes', []), boxes)
            overlay = image.copy()
            draw = ImageDraw.Draw(overlay)
            for i, box in enumerate(boxes):
                draw.rectangle(box, outline='red', width=2)
                draw.text((box[0], max(0, box[1] - 15)), str(i+1), fill='red')
            overlay.save(args.output / f"{case['id']}-regions.png")
            for method in ['detector-tesseract', 'detector-paddle'] + (['detector-manga'] if lang == 'jpn' else []):
                start = time.perf_counter()
                texts = []
                for box in boxes:
                    crop = image.crop(box)
                    if method == 'detector-tesseract':
                        text = tesseract(crop, args.tesseract.resolve(), args.tessdata.resolve(), lang + '+eng', 6, directory)['text']
                    elif method == 'detector-paddle':
                        if crop.height > 1.5 * crop.width:
                            crop = crop.transpose(Image.Transpose.ROTATE_90)
                        result = next(iter(recognizers[lang].predict(np.array(crop)[:, :, ::-1])))
                        text = result['rec_text']
                    else:
                        text = manga(crop)
                    texts.append(text)
                methods[method] = {'text': '\n'.join(texts), 'seconds': time.perf_counter() - start + detection_seconds}
            for method, result in methods.items():
                result.update(case=case['id'], method=method, regions=len(boxes),
                              reference_box_coverage=coverage)
                for field in ('title', 'author'):
                    expected = case[field]
                    result[field + '_errors'] = field_errors(expected, result['text']) if expected else None
                    result[field + '_exact'] = normalized(expected) in normalized(result['text']) if expected else None
                result['blank_false_positive'] = bool(result['text'].strip()) if not case['title'] else None
                report['results'].append(result)
                print(json.dumps({k: v for k, v in result.items() if k != 'text'}, ensure_ascii=True), flush=True)
        (args.output / 'report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    models = []
    for root in [Path.home() / '.paddlex/official_models', Path.home() / '.cache/huggingface/hub/models--kha-white--manga-ocr-base/snapshots']:
        if root.exists():
            for file in sorted(root.rglob('*')):
                if file.is_file():
                    with file.open('rb') as stream:
                        digest = hashlib.file_digest(stream, 'sha256').hexdigest()
                    models.append({'file': str(file.relative_to(root)), 'sha256': digest, 'bytes': file.stat().st_size})
    (args.output / 'models.json').write_text(json.dumps(models, indent=2), encoding='utf-8')
    lines = ['# OCR comparison', '', 'Synthetic fixtures only; not real-manga accuracy.', '',
             '| Case | Method | Title errors | Author errors | Seconds |', '|---|---|---:|---:|---:|']
    for r in report['results']:
        lines.append(f"| {r['case']} | {r['method']} | {r['title_errors']} | {r['author_errors']} | {r['seconds']:.2f} |")
    (args.output / 'report.md').write_text('\n'.join(lines), encoding='utf-8')


if __name__ == '__main__':
    main()
