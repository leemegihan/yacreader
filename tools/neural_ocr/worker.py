"""Offline OCR: one model session per sampled work, batched region recognition."""
import argparse
import json
import math
import os
from pathlib import Path
import re
import socket
import sys
import time
import unicodedata

MODEL_NAMES = ['PP-OCRv5_mobile_det', 'PP-OCRv5_server_rec', 'korean_PP-OCRv5_mobile_rec']


def no_network(*args, **kwargs):
    raise OSError('The local OCR worker does not allow network access')


def write_json(path, value):
    temporary = path.with_suffix('.tmp')
    temporary.write_text(json.dumps(value, ensure_ascii=False), encoding='utf-8')
    # Windows readers can briefly hold the old file without delete sharing.
    # Preserve atomic replacement; never truncate the published JSON in place.
    for attempt in range(51):
        try:
            temporary.replace(path)
            return
        except PermissionError as error:
            if os.name != 'nt' or getattr(error, 'winerror', None) not in (5, 32, 33) or attempt == 50:
                raise
            time.sleep(.01)


def regions_from(polygons, width, height):
    regions = []
    for polygon in polygons:
        if len(polygon) != 4 or not all(len(p) == 2 and all(math.isfinite(float(v)) for v in p) for p in polygon):
            raise ValueError('Invalid text region')
        points = [(max(0., min(width, float(p[0]))), max(0., min(height, float(p[1])))) for p in polygon]
        box = [max(0, math.floor(min(p[0] for p in points)) - 4),
               max(0, math.floor(min(p[1] for p in points)) - 4),
               min(width, math.ceil(max(p[0] for p in points)) + 4),
               min(height, math.ceil(max(p[1] for p in points)) + 4)]
        if box[2] - box[0] >= 4 and box[3] - box[1] >= 4:
            regions.append((box, points))
    if len(regions) > 256:
        raise ValueError('Too many text regions on this page')
    vertical = sum(b[3] - b[1] > 1.5 * (b[2] - b[0]) for b, _ in regions)
    return sorted(regions, key=(lambda r: (-r[0][0], r[0][1])) if regions and vertical > len(regions) / 2
                  else (lambda r: (r[0][1], r[0][0])))


def crop_region(image, region):
    from PIL import Image, ImageOps
    box, p = region
    width = round(max(math.dist(p[0], p[1]), math.dist(p[3], p[2])))
    height = round(max(math.dist(p[0], p[3]), math.dist(p[1], p[2])))
    if min(width, height) < 4:
        return image.crop(box)
    # Paddle quadrilaterals: top-left, top-right, bottom-right, bottom-left.
    # Pillow QUAD: top-left, bottom-left, bottom-right, top-right.
    quad = tuple(v for point in (p[0], p[3], p[2], p[1]) for v in point)
    crop = image.transform((width, height), Image.Transform.QUAD, quad, Image.Resampling.BICUBIC)
    return ImageOps.expand(crop, 3, fill=crop.getpixel((0, 0)))


def upright_strip(crop):
    """Reflow separated upright vertical glyphs; never rotate Hangul syllables."""
    import numpy as np
    from PIL import Image, ImageOps
    if crop.height < 2 * crop.width:
        return None
    gray = np.asarray(crop.convert('L'))
    # Choose minority foreground for either white lettering or black ink.
    foreground = gray < (float(gray.min()) + float(gray.max())) / 2
    if foreground.mean() > .5:
        foreground = ~foreground
    occupied = foreground.sum(axis=1) >= max(2, crop.width * .08)
    runs, start = [], None
    for y, ink in enumerate(list(occupied) + [False]):
        if ink and start is None:
            start = y
        if not ink and start is not None:
            if y - start >= crop.width * .3:
                runs.append((start, y))
            start = None
    if not 2 <= len(runs) <= 24 or any(b - a > crop.width * 1.7 for a, b in runs):
        return None
    if sum(b - a for a, b in runs) < crop.height * .35:
        return None
    cell = crop.width
    background = 255 if gray.mean() > 128 else 0
    strip = Image.new('RGB', ((cell + 4) * len(runs), cell + 8), (background,) * 3)
    for i, (a, b) in enumerate(runs):
        glyph = ImageOps.contain(crop.crop((0, max(0, a - 1), crop.width, min(crop.height, b + 1))), (cell, cell))
        strip.paste(glyph, (i * (cell + 4), (cell + 8 - glyph.height) // 2))
    return strip


def merge_lines(lines, additions):
    """Keep stronger duplicate readings, but preserve conflicting nearby text."""
    for new in additions:
        matched = False
        a = new['box']
        for index, old in enumerate(lines):
            b = old['box']
            overlap = max(0, min(a[2], b[2]) - max(a[0], b[0])) * max(0, min(a[3], b[3]) - max(a[1], b[1]))
            area = min((a[2] - a[0]) * (a[3] - a[1]), (b[2] - b[0]) * (b[3] - b[1]))
            if area and overlap / area > .65:
                if new['confidence'] > old['confidence'] and (new['text'].replace(' ', '') == old['text'].replace(' ', '') or old['confidence'] < 65):
                    lines[index] = new
                matched = True
                break
        if not matched and len(lines) < 256:
            lines.append(new)
    return lines


def select_reading(options):
    """Keep the score winner; expose cross-language ambiguity for review only."""
    if not options:
        return None
    best = max(options, key=lambda item: item['confidence'])
    if best['confidence'] < 35:
        return None
    result = dict(best)
    compact = lambda text: ''.join(unicodedata.normalize('NFKC', text).split())

    def has_script(text, language):
        if language == 'jpn':
            return any('\u3040' <= ch <= '\u30ff' or '\u3400' <= ch <= '\u9fff' for ch in text)
        return language == 'kor' and any('\uac00' <= ch <= '\ud7a3' for ch in text)

    primary = compact(best['text'])
    ascii_fragment = primary.isascii() and any(ch.isalnum() for ch in primary)
    delimiter_fragment = primary in {'/', ':', '・'}
    alternatives, seen = [], set()
    for option in sorted(options, key=lambda item: item['confidence'], reverse=True):
        if option['language'] == best['language'] or option['confidence'] < 85 or option['box'] != best['box']:
            continue
        alternate = compact(option['text'])
        identity = (option['language'], alternate)
        if not alternate or alternate == primary or identity in seen:
            continue
        extended = ((ascii_fragment or (delimiter_fragment and best['confidence'] - option['confidence'] <= 8))
                    and primary in alternate and len(alternate) >= len(primary) + 2
                    and has_script(alternate.replace(primary, '', 1), option['language']))
        # Scores from different models are not calibrated correctness probabilities.
        # A small gap only controls review volume; it never changes primary text.
        disputed = (best['confidence'] - option['confidence'] <= 8
                    and has_script(primary, best['language'])
                    and has_script(alternate, option['language']))
        if not (extended or disputed):
            continue
        alternatives.append(dict(option))
        seen.add(identity)
        if len(alternatives) == 2:
            break
    if alternatives:
        result['alternatives'] = alternatives
    return result


class Engine:
    def __init__(self, root, language, threads, device, progress=lambda stage: None, diagnostics=False):
        from paddleocr import TextDetection, TextRecognition
        import paddle
        self.progress = progress
        self.diagnostics_enabled = diagnostics
        self.diagnostics = []
        self.diagnostic_pass = None
        self.language = language
        self.device = device
        self.warning = ''
        if device == 'gpu:0' and (not paddle.is_compiled_with_cuda() or paddle.device.cuda.device_count() < 1):
            self.device = 'cpu'
            self.warning = 'GPU unavailable; CPU used'
        common = dict(device=self.device, enable_mkldnn=False, cpu_threads=threads)
        self.progress('models')
        self.detector = TextDetection(model_name=MODEL_NAMES[0], model_dir=str(root / 'models' / MODEL_NAMES[0]), **common)
        languages = ['jpn', 'kor'] if language == 'auto' else [language]
        self.readers = {}
        for language in languages:
            name = MODEL_NAMES[1 if language == 'jpn' else 2]
            self.readers[language] = TextRecognition(model_name=name, model_dir=str(root / 'models' / name), **common)

    def regions(self, image):
        import numpy as np
        detected = next(iter(self.detector.predict(np.asarray(image)[:, :, ::-1])))
        return regions_from(detected['dt_polys'], image.width, image.height)

    def read_regions(self, image, regions):
        import numpy as np
        from PIL import Image
        crops, owners, variants = [], [], []
        for index, region in enumerate(regions):
            crop = crop_region(image, region)
            vertical = crop.height > 1.5 * crop.width
            crops.append(crop.transpose(Image.Transpose.ROTATE_90) if vertical else crop)
            owners.append(index)
            variants.append('normal')
            if vertical and 'kor' in self.readers:
                strip = upright_strip(crop)
                if strip is not None:
                    crops.append(strip)
                    owners.append(index)
                    variants.append('upright-kor')
        choices = [[] for _ in regions]
        for language, model in self.readers.items():
            indexes = [i for i, variant in enumerate(variants) if variant == 'normal' or language == 'kor']
            for start in range(0, len(indexes), 8):
                self.progress('recognize')
                chunk = indexes[start:start + 8]
                results = list(model.predict([np.asarray(crops[i])[:, :, ::-1] for i in chunk], batch_size=len(chunk)))
                if len(results) != len(chunk):
                    raise ValueError('Incomplete OCR recognition batch')
                for i, result in zip(chunk, results):
                    text, score = result['rec_text'].strip(), float(result['rec_score'])
                    if not math.isfinite(score) or not 0 <= score <= 1:
                        raise ValueError('Invalid recognition confidence')
                    audit = getattr(self, 'diagnostic_pass', None)
                    if audit is not None:
                        audit['recognitions'].append({'region': owners[i], 'language': language,
                                                      'variant': variants[i], 'text': text[:160],
                                                      'textTruncated': len(text) > 160,
                                                      'confidence': score * 100})
                    if not text or len(text) > 1000:
                        continue
                    if variants[i] == 'upright-kor' and (score < .65 or sum('\uac00' <= c <= '\ud7a3' for c in text) < len(text.replace(' ', '')) * .7):
                        continue
                    choices[owners[i]].append({'text': text, 'confidence': score * 100,
                                               'language': language, 'box': regions[owners[i]][0]})
        # Cross-model scores are uncalibrated: alternatives never replace the winner.
        return [reading for options in choices for reading in [select_reading(options)] if reading]

    def read_pass(self, image, origin=(0, 0), scale=(1., 1.)):
        regions = self.regions(image)
        audit = None
        if getattr(self, 'diagnostics_enabled', False):
            audit = {'origin': list(origin), 'scale': list(scale),
                     'detectedRegions': [region[0] for region in regions], 'recognitions': []}
            self.diagnostics.append(audit)
        self.diagnostic_pass = audit
        try:
            return self.read_regions(image, regions)
        finally:
            self.diagnostic_pass = None

    def read(self, image):
        self.progress('detect')
        self.diagnostics = []
        lines = self.read_pass(image)
        # A small credit label is a useful local hint. Re-detect just the area
        # below it at higher resolution instead of repeatedly scaling a page.
        label = re.compile(r'^(?:発行[／/]?著者|著者|作者|誌名|タイトル|저자|작가|지음)[:：\s]*$')
        anchors = [line for line in lines if line['confidence'] >= 65 and label.fullmatch(line['text'].replace(' ', ''))
                   and line['box'][1] > image.height * .35]
        for anchor in anchors[:3]:
            self.progress('credits')
            x1, y1, x2, y2 = anchor['box']
            h = y2 - y1
            roi = (max(0, x1 - image.width // 5), max(0, y1 - 2 * h),
                   min(image.width, x2 + image.width // 3), min(image.height, y2 + 6 * h))
            crop = image.crop(roi)
            scale = min(2., math.sqrt(4000000 / (crop.width * crop.height)))
            if scale <= 1:
                continue
            enlarged = crop.resize((round(crop.width * scale), round(crop.height * scale)))
            sx, sy = enlarged.width / crop.width, enlarged.height / crop.height
            additions = self.read_pass(enlarged, roi[:2], (sx, sy))
            for line in additions:
                a, b, c, d = line['box']
                line['box'] = [max(0, math.floor(a / sx + roi[0])), max(0, math.floor(b / sy + roi[1])),
                               min(image.width, math.ceil(c / sx + roi[0])), min(image.height, math.ceil(d / sy + roi[1]))]
                for alternative in line.get('alternatives', []):
                    alternative['box'] = list(line['box'])
            merge_lines(lines, additions)
        vertical = sum(b['box'][3] - b['box'][1] > 1.5 * (b['box'][2] - b['box'][0]) for b in lines)
        lines.sort(key=(lambda line: (-line['box'][0], line['box'][1])) if lines and vertical > len(lines) / 2
                   else (lambda line: (line['box'][1], line['box'][0])))
        return lines


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--image', type=Path)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--manifest', type=Path)
    parser.add_argument('--language', required=True, choices=['auto', 'jpn', 'kor'])
    parser.add_argument('--threads', type=int, default=8)
    parser.add_argument('--device', choices=['cpu', 'gpu:0'], default='cpu')
    parser.add_argument('--diagnostics', action='store_true', help='Include local detection/recognition evidence for error triage')
    args = parser.parse_args()
    threads = max(1, min(args.threads, os.cpu_count() or 1, 16))
    os.environ.update(PADDLE_PDX_DISABLE_MODEL_SOURCE_CHECK='True', HF_HUB_OFFLINE='1',
                      HF_HUB_DISABLE_TELEMETRY='1', OMP_NUM_THREADS=str(threads))
    socket.socket.connect = no_network
    socket.socket.connect_ex = no_network
    socket.create_connection = no_network
    progress_path = None
    if args.manifest:
        manifest = json.loads(args.manifest.read_text(encoding='utf-8'))
        if manifest.get('version') != 1 or not 1 <= len(manifest.get('pages', [])) <= 12:
            raise ValueError('Invalid OCR page manifest')
        pages = [(Path(p['image']), Path(p['output'])) for p in manifest['pages']]
        progress_path = args.manifest.parent / 'progress.json'
        for image_path, output_path in pages:
            if image_path.resolve().parent != args.manifest.resolve().parent or output_path.resolve().parent != args.manifest.resolve().parent:
                raise ValueError('OCR manifest paths must be in the job directory')
    elif args.image and args.output:
        pages = [(args.image.resolve(), args.output.resolve())]
    else:
        parser.error('Use --manifest or both --image and --output')
    root = Path(__file__).resolve().parent
    os.chdir(root)
    for name in MODEL_NAMES:
        if not (root / 'models' / name / 'inference.pdiparams').is_file():
            raise RuntimeError('Offline OCR model is missing: ' + name)
    from PIL import Image
    index = 0
    def progress(stage):
        if progress_path:
            write_json(progress_path, {'completed': index, 'total': len(pages), 'stage': stage})
    start = time.perf_counter()
    progress('models')
    # Relative model paths preserve Paddle's Windows Unicode-path workaround.
    engine = Engine(Path('.'), args.language, threads, args.device, progress, args.diagnostics)
    initialization_ms = round((time.perf_counter() - start) * 1000)
    for index, (image_path, output_path) in enumerate(pages):
        started = time.perf_counter()
        result = {'version': 1, 'engine': 'paddle-regions', 'language': args.language,
                  'device': engine.device, 'cpuThreads': threads, 'warning': engine.warning,
                  'initializationMs': initialization_ms if index == 0 else 0, 'lines': []}
        engine.diagnostics = []
        try:
            with Image.open(image_path) as source:
                if source.width * source.height > 17000000:
                    raise ValueError('OCR input is too large')
                image = source.convert('RGB')
            result['lines'] = engine.read(image)
        except Exception as error:
            result['error'] = str(error)[:1500]
        if args.diagnostics:
            result['diagnostics'] = {'version': 1, 'passes': engine.diagnostics}
        result['elapsedMs'] = round((time.perf_counter() - started) * 1000)
        write_json(output_path, result)
    index = len(pages)
    progress('done')


if __name__ == '__main__':
    try:
        main()
    except Exception as error:
        print(str(error), file=sys.stderr)
        sys.exit(1)
