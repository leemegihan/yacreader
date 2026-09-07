"""Offline, single-page OCR worker. Invoked by Qt without a command shell."""
import argparse
import json
import math
import os
from pathlib import Path
import socket
import sys

os.environ['PADDLE_PDX_DISABLE_MODEL_SOURCE_CHECK'] = 'True'
os.environ['HF_HUB_OFFLINE'] = '1'
os.environ['HF_HUB_DISABLE_TELEMETRY'] = '1'
os.environ['OMP_NUM_THREADS'] = '2'


def no_network(*args, **kwargs):
    raise OSError('The local OCR worker does not allow network access')


socket.socket.connect = no_network
socket.socket.connect_ex = no_network
socket.create_connection = no_network


def boxes_from(polygons, width, height):
    boxes = []
    for polygon in polygons:
        if len(polygon) != 4 or not all(math.isfinite(float(v)) for p in polygon for v in p):
            raise ValueError('Invalid text region')
        box = [max(0, math.floor(min(p[0] for p in polygon)) - 4),
               max(0, math.floor(min(p[1] for p in polygon)) - 4),
               min(width, math.ceil(max(p[0] for p in polygon)) + 4),
               min(height, math.ceil(max(p[1] for p in polygon)) + 4)]
        if box[2]-box[0] >= 4 and box[3]-box[1] >= 4:
            boxes.append(box)
    if len(boxes) > 256:
        raise ValueError('Too many text regions on this page')
    vertical = sum(b[3]-b[1] > 1.5*(b[2]-b[0]) for b in boxes)
    return sorted(boxes, key=(lambda b: (-b[0], b[1])) if boxes and vertical > len(boxes)/2 else (lambda b: (b[1], b[0])))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--image', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--language', required=True, choices=['auto', 'jpn', 'kor'])
    args = parser.parse_args()
    image_path, output_path = args.image.resolve(), args.output.resolve()
    root = Path(__file__).resolve().parent
    os.chdir(root)
    model_names = ['PP-OCRv5_mobile_det', 'PP-OCRv5_server_rec', 'korean_PP-OCRv5_mobile_rec']
    for name in model_names:
        if not (Path('models') / name / 'inference.pdiparams').is_file():
            raise RuntimeError('Offline OCR model is missing: ' + name)
    from PIL import Image
    import numpy as np
    from paddleocr import TextDetection, TextRecognition
    image = Image.open(image_path).convert('RGB')
    if image.width * image.height > 17000000:
        raise ValueError('OCR input is too large')
    common = dict(device='cpu', enable_mkldnn=False, cpu_threads=min(4, os.cpu_count() or 1))
    detector = TextDetection(model_name=model_names[0], model_dir='models/' + model_names[0], **common)
    detected = next(iter(detector.predict(np.array(image)[:, :, ::-1])))
    boxes = boxes_from(detected['dt_polys'], image.width, image.height)
    languages = ['jpn', 'kor'] if args.language == 'auto' else [args.language]
    readers = {language: TextRecognition(model_name=model_names[1 if language == 'jpn' else 2],
                model_dir='models/' + model_names[1 if language == 'jpn' else 2], **common)
               for language in languages} if boxes else {}
    lines = []
    for box in boxes:
        crop = image.crop(box)
        if crop.height > 1.5 * crop.width:
            crop = crop.transpose(Image.Transpose.ROTATE_90)
        readings = []
        for language, model in readers.items():
            result = next(iter(model.predict(np.array(crop)[:, :, ::-1])))
            text, score = result['rec_text'].strip(), float(result['rec_score'])
            if not math.isfinite(score) or not 0 <= score <= 1:
                raise ValueError('Invalid recognition confidence')
            if text and len(text) <= 1000:
                readings.append({'text': text, 'confidence': score * 100, 'language': language, 'box': box})
        if readings:
            # Uncalibrated cross-model choice: always requires review in Qt.
            best = max(readings, key=lambda r: r['confidence'])
            if best['confidence'] >= 35:
                lines.append(best)
    output_path.write_text(json.dumps({'version': 1, 'engine': 'paddle-regions',
                           'language': args.language, 'lines': lines}, ensure_ascii=False), encoding='utf-8')


if __name__ == '__main__':
    try:
        main()
    except Exception as error:
        print(str(error), file=sys.stderr)
        sys.exit(1)
