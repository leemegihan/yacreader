"""Generate non-user, non-copyright comic-like layouts with exact labels."""
import argparse
import json
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont, ImageFilter


def generate(output, font_path):
    output.mkdir(parents=True, exist_ok=True)
    cases = []
    for lang, title, author in [('jpn', '青い空の旅', '山田太郎'),
                                 ('kor', '푸른 하늘 여행', '홍길동')]:
        labels = ['タイトル', title, '著者', author] if lang == 'jpn' else ['제목', title, '작가', author]
        for layout in ['colophon', 'panels', 'small', 'white']:
            page = Image.new('RGB', (1200, 1600), 'white')
            draw = ImageDraw.Draw(page)
            size = 40 if layout != 'small' else 24
            font = ImageFont.truetype(str(font_path), size)
            if layout in ('panels', 'small', 'white'):
                for y in (20, 540, 1070):
                    draw.rectangle((20, y, 1180, min(y + 490, 1580)), outline='black', width=7)
                for i in range(32):
                    draw.line((30, 50 + i * 42, 1160, 420 + i * 26), fill=(140, 140, 140), width=2)
                for box in [(80, 100, 650, 390), (570, 650, 1120, 990)]:
                    draw.ellipse(box, fill='white', outline='black', width=3)
                # Realistic clutter, but no actual comic illustrations.
            positions = [(140, 160), (140, 235), (650, 720), (650, 795)] if layout in ('panels', 'small') else [(180, 220 + i * 160) for i in range(4)]
            truth_boxes = []
            for text, (x, y) in zip(labels, positions):
                box = draw.textbbox((x, y), text, font=font)
                if layout == 'white':
                    draw.rectangle((box[0]-15, box[1]-15, box[2]+15, box[3]+15), fill='black')
                draw.text((x, y), text, font=font, fill='white' if layout == 'white' else 'black')
                truth_boxes.append(list(box))
            if layout == 'small':
                page = page.filter(ImageFilter.GaussianBlur(0.5))
            name = f'{lang}-{layout}'
            page.save(output / f'{name}.png')
            cases.append({'id': name, 'image': f'{name}.png', 'language': lang,
                          'title': title, 'author': author, 'boxes': truth_boxes})
    blank = Image.new('RGB', (1200, 1600), 'white')
    blank.save(output / 'blank.png')
    cases.append({'id': 'blank', 'image': 'blank.png', 'language': 'jpn', 'title': '', 'author': '', 'boxes': []})
    (output / 'manifest.json').write_text(json.dumps({'synthetic': True, 'cases': cases}, ensure_ascii=False, indent=2), encoding='utf-8')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--font', type=Path, required=True)
    args = parser.parse_args()
    generate(args.output, args.font)
