"""Local OCR comparison utilities. No metadata writes or image uploads."""
import csv
import io
import math
import subprocess
import unicodedata


def normalized(text):
    return ''.join(c for c in unicodedata.normalize('NFKC', text) if not c.isspace())


def field_errors(expected, text):
    """Levenshtein distance to the best substring; does not forgive substitutions."""
    expected, text = normalized(expected), normalized(text)
    if not expected:
        raise ValueError('Ground truth must not be empty')
    previous = [0] * (len(text) + 1)
    for i, char in enumerate(expected, 1):
        current = [i]
        for j, actual in enumerate(text, 1):
            current.append(min(current[-1] + 1, previous[j] + 1,
                               previous[j - 1] + (char != actual)))
        previous = current
    return min(previous)


def ordered_boxes(polygons, width, height):
    """Bound detector rectangles; preserve columns instead of making one big crop."""
    boxes = []
    for poly in polygons:
        if len(poly) != 4 or any(len(p) != 2 for p in poly):
            raise ValueError('Detector must return quadrilaterals')
        if not all(math.isfinite(float(v)) for point in poly for v in point):
            raise ValueError('Non-finite detector coordinate')
        x0 = max(0, math.floor(min(p[0] for p in poly)) - 4)
        y0 = max(0, math.floor(min(p[1] for p in poly)) - 4)
        x1 = min(width, math.ceil(max(p[0] for p in poly)) + 4)
        y1 = min(height, math.ceil(max(p[1] for p in poly)) + 4)
        if x1 - x0 >= 4 and y1 - y0 >= 4:
            boxes.append([x0, y0, x1, y1])
    if len(boxes) > 256:
        raise ValueError('Too many text regions')
    # Reading order is provisional. Vertical pages read columns right to left.
    vertical = sum(b[3] - b[1] > 1.5 * (b[2] - b[0]) for b in boxes)
    if boxes and vertical > len(boxes) / 2:
        return sorted(boxes, key=lambda b: (-b[0], b[1]))
    return sorted(boxes, key=lambda b: (b[1], b[0]))


def tesseract(image, executable, data, language, layout, directory):
    from pathlib import Path
    # Same bounded grayscale/upscale/border preprocessing as the app.
    from PIL import ImageOps
    page = image.convert('RGB')
    page.thumbnail((4000, 4000))
    if max(page.size) < 2000:
        page = page.resize((page.width * 2, page.height * 2))
    page = ImageOps.expand(page.convert('L'), border=20, fill='white')
    page.save(Path(directory) / 'input.png')
    run = subprocess.run([str(executable), 'input.png', 'stdout', '--tessdata-dir',
                          str(data), '-l', language, '--oem', '1', '--psm', str(layout),
                          '--dpi', '300', '-c', 'tessedit_create_tsv=1',
                          '-c', 'tessedit_create_txt=0'], cwd=directory,
                         capture_output=True, timeout=90, check=True)
    if b'Failed loading language' in run.stderr:
        raise RuntimeError('Missing Tesseract language')
    rows = csv.DictReader(io.StringIO(run.stdout.decode('utf-8-sig')), delimiter='\t')
    lines, weights, scores = {}, 0, 0.0
    for row in rows:
        if row['level'] != '5' or not row['text'].strip():
            continue
        key = tuple(row[k] for k in ('page_num', 'block_num', 'par_num', 'line_num'))
        lines.setdefault(key, []).append(row['text'])
        conf = float(row['conf'])
        if conf >= 0:
            weights += len(row['text'])
            scores += conf * len(row['text'])
    return {'text': '\n'.join(' '.join(line) for line in lines.values()),
            'confidence': scores / weights if weights else -1}
