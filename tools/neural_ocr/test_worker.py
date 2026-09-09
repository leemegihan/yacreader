"""No models/network required: geometric and batched inference regressions."""
import math
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import numpy as np
from PIL import Image, ImageDraw
import worker


class Reader:
    def __init__(self, text='Author: Alice', score=.95):
        self.text, self.score, self.calls = text, score, []

    def predict(self, inputs, batch_size):
        self.calls.append((len(inputs), batch_size))
        return [{'rec_text': self.text, 'rec_score': self.score} for _ in inputs]


class WorkerTest(unittest.TestCase):
    def test_region_validation_and_clipping(self):
        region = worker.regions_from([[[-5, -2], [20, 0], [22, 16], [0, 18]]], 20, 20)[0]
        self.assertEqual(region[0], [0, 0, 20, 20])
        with self.assertRaises(ValueError):
            worker.regions_from([[[0, 0], [math.nan, 0], [20, 16], [0, 18]]], 20, 20)
        with self.assertRaises(ValueError):
            worker.regions_from([[[0, 0], [20, 0], [20, 16], [0, 18]]] * 257, 20, 20)

    def test_slanted_region_is_rectified(self):
        image = Image.new('RGB', (200, 100), 'white')
        polygon = [(20, 20), (160, 50), (160, 70), (20, 40)]
        ImageDraw.Draw(image).polygon(polygon, fill='black')
        crop = worker.crop_region(image, worker.regions_from([polygon], 200, 100)[0])
        self.assertLess(crop.height, 30)
        self.assertGreater(crop.width, 140)
        self.assertLess(np.asarray(crop)[10:-10, 10:-10].mean(), 30)

    def test_upright_glyphs_are_reflowed_not_rotated(self):
        image = Image.new('RGB', (40, 150), 'white')
        draw = ImageDraw.Draw(image)
        for y in (5, 55, 105):
            draw.rectangle((5, y, 30, y + 29), fill='black')
        strip = worker.upright_strip(image)
        self.assertIsNotNone(strip)
        self.assertGreater(strip.width, strip.height)
        self.assertIsNone(worker.upright_strip(Image.new('RGB', (40, 150), 'white')))

    def test_batching_reuses_reader_across_pages(self):
        engine = worker.Engine.__new__(worker.Engine)
        engine.readers = {'jpn': Reader(), 'kor': Reader('작가 홍길동', .97)}
        engine.progress = lambda stage: None
        image = Image.new('RGB', (300, 300), 'white')
        regions = worker.regions_from([[(5, y), (150, y), (150, y+10), (5, y+10)] for y in range(0, 180, 10)], 300, 300)
        for _ in range(2):
            lines = engine.read_regions(image, regions)
            self.assertEqual(len(lines), 18)
            self.assertTrue(all(line['language'] == 'kor' for line in lines))
        for reader in engine.readers.values():
            self.assertEqual(reader.calls, [(8, 8), (8, 8), (2, 2)] * 2)

    def test_bad_batch_and_confidence_fail_explicitly(self):
        engine = worker.Engine.__new__(worker.Engine)
        reader = Reader(score=math.nan)
        engine.readers, engine.progress = {'jpn': reader}, lambda stage: None
        image = Image.new('RGB', (200, 100), 'white')
        regions = worker.regions_from([[(5, 5), (150, 5), (150, 20), (5, 20)]], 200, 100)
        with self.assertRaises(ValueError):
            engine.read_regions(image, regions)
        with patch.object(reader, 'predict', return_value=[]), self.assertRaises(ValueError):
            engine.read_regions(image, regions)

    def test_retry_merges_duplicates_without_overwriting_strong_conflicts(self):
        lines = [{'text': 'Alice', 'confidence': 92, 'box': [10, 10, 100, 30]}]
        worker.merge_lines(lines, [{'text': 'A lice', 'confidence': 98, 'box': [9, 9, 101, 31]},
                                   {'text': 'Bob', 'confidence': 99, 'box': [10, 10, 100, 30]},
                                   {'text': 'Title', 'confidence': 95, 'box': [10, 80, 200, 100]}])
        self.assertEqual([line['text'] for line in lines], ['A lice', 'Title'])

    def test_diagnostics_distinguish_detection_from_rejected_text(self):
        engine = worker.Engine.__new__(worker.Engine)
        engine.readers = {'jpn': Reader('uncertain', .2)}
        engine.progress = lambda stage: None
        engine.diagnostics_enabled = True
        engine.diagnostics = []
        image = Image.new('RGB', (200, 100), 'white')
        regions = worker.regions_from([[(5, 5), (150, 5), (150, 20), (5, 20)]], 200, 100)
        with patch.object(engine, 'regions', return_value=regions):
            self.assertEqual(engine.read(image), [])
        audit = engine.diagnostics[0]
        self.assertEqual(len(audit['detectedRegions']), 1)
        self.assertEqual(audit['recognitions'][0]['text'], 'uncertain')
        self.assertEqual(audit['recognitions'][0]['confidence'], 20)
        self.assertIsNone(engine.diagnostic_pass)
        with patch.object(engine, 'regions', return_value=[]):
            self.assertEqual(engine.read(image), [])
        self.assertEqual(engine.diagnostics[0]['detectedRegions'], [])
        engine.diagnostics_enabled = False
        with patch.object(engine, 'regions', return_value=regions):
            self.assertEqual(engine.read(image), [])
        self.assertEqual(engine.diagnostics, [])

    def test_diagnostic_retry_coordinates_are_explicit(self):
        engine = worker.Engine.__new__(worker.Engine)
        engine.readers = {'jpn': Reader()}
        engine.progress = lambda stage: None
        engine.diagnostics_enabled = True
        engine.diagnostics = []
        image = Image.new('RGB', (200, 100), 'white')
        regions = worker.regions_from([[(5, 5), (150, 5), (150, 20), (5, 20)]], 200, 100)
        with patch.object(engine, 'regions', return_value=regions):
            lines = engine.read_pass(image, (40, 80), (2., 1.5))
        self.assertEqual(len(lines), 1)
        self.assertEqual(engine.diagnostics[0]['origin'], [40, 80])
        self.assertEqual(engine.diagnostics[0]['scale'], [2., 1.5])

    def test_diagnostic_cap_does_not_truncate_actual_reading(self):
        engine = worker.Engine.__new__(worker.Engine)
        engine.readers = {'jpn': Reader('A' * 200)}
        engine.progress = lambda stage: None
        engine.diagnostics_enabled = True
        engine.diagnostics = []
        image = Image.new('RGB', (200, 100), 'white')
        regions = worker.regions_from([[(5, 5), (150, 5), (150, 20), (5, 20)]], 200, 100)
        with patch.object(engine, 'regions', return_value=regions):
            reading = engine.read(image)
        self.assertEqual(reading[0]['text'], 'A' * 200)
        audit = engine.diagnostics[0]['recognitions'][0]
        self.assertEqual(len(audit['text']), 160)
        self.assertTrue(audit['textTruncated'])

    def test_partial_language_reading_is_retained_without_replacing_winner(self):
        short = {'text': '[Circle]', 'confidence': 97, 'language': 'kor', 'box': [0, 0, 100, 20]}
        full = dict(short, text='[Circle]月の工房', confidence=93, language='jpn')
        result = worker.select_reading([full, short, full])
        self.assertEqual(result['text'], short['text'])
        self.assertEqual(result['confidence'], short['confidence'])
        self.assertEqual(result['alternatives'], [full])
        self.assertNotIn('alternatives', short)
        self.assertNotIn('alternatives', full)
        for alternate in [dict(full, confidence=84), dict(full, language='kor'),
                          dict(full, text='[Studio]月の工房'), dict(full, text='[Circle] Moon Studio'),
                          dict(full, text='[Circle]月')]:
            self.assertNotIn('alternatives', worker.select_reading([short, alternate]))
        self.assertNotIn('alternatives', worker.select_reading([full]))
        self.assertIsNone(worker.select_reading([]))
        self.assertIsNone(worker.select_reading([dict(short, confidence=34)]))
        korean = dict(short, text='그PC-90과', confidence=90)
        latin = dict(full, text='PC-90', confidence=98)
        self.assertEqual(worker.select_reading([latin, korean])['alternatives'], [korean])

    def test_cjk_disagreement_retains_both_directions_and_score_ties(self):
        japanese = {'text': '雨', 'confidence': 96, 'language': 'jpn', 'box': [0, 0, 100, 20]}
        korean = dict(japanese, text='가상의 글', confidence=94, language='kor')
        for options in ([japanese, korean], [dict(korean, confidence=98), japanese],
                        [dict(korean, confidence=96), japanese], [japanese, dict(korean, confidence=96)]):
            winner = max(options, key=lambda x: x['confidence'])
            result = worker.select_reading(options)
            self.assertEqual({k: v for k, v in result.items() if k != 'alternatives'}, winner)
            self.assertEqual(result['alternatives'], [x for x in options if x is not winner])
            self.assertTrue(all('alternatives' not in x for x in options))

    def test_cjk_disagreement_filters_low_scores_gaps_scripts_and_duplicate_text(self):
        japanese = {'text': '雨', 'confidence': 96, 'language': 'jpn', 'box': [0, 0, 100, 20]}
        korean = dict(japanese, text='가상의 글', confidence=88, language='kor')
        self.assertEqual(worker.select_reading([japanese, korean])['alternatives'], [korean])
        for other in [dict(korean, confidence=87.99), dict(korean, confidence=84.99),
                      dict(korean, text='Latin words'), dict(korean, text='雨'),
                      dict(korean, language='jpn'), dict(korean, box=[1, 0, 100, 20])]:
            self.assertNotIn('alternatives', worker.select_reading([japanese, other]))
        self.assertNotIn('alternatives', worker.select_reading([dict(japanese, confidence=84.99), dict(korean, confidence=84)]))
        self.assertEqual(len(worker.select_reading([dict(japanese, confidence=85), dict(korean, confidence=85)])['alternatives']), 1)
        same = dict(korean, text='가 상 의 글')
        another = dict(korean, text='검토 문구')
        third = dict(korean, text='세 번째 대안')
        result = worker.select_reading([japanese, korean, same, another, third])
        self.assertEqual(result['alternatives'], [korean, another])
        mixed = dict(japanese, text='雨가상의글')
        identical = dict(korean, text='雨 가상의 글')
        self.assertNotIn('alternatives', worker.select_reading([mixed, identical]))

    def test_batched_reader_preserves_disputed_cjk_without_extra_inference(self):
        engine = worker.Engine.__new__(worker.Engine)
        engine.readers = {'jpn': Reader('雨', .96), 'kor': Reader('가상의 글', .94)}
        engine.progress = lambda stage: None
        image = Image.new('RGB', (200, 100), 'white')
        regions = worker.regions_from([[(5, 5), (150, 5), (150, 20), (5, 20)]], 200, 100)
        with patch.object(engine.readers['jpn'], 'predict', wraps=engine.readers['jpn'].predict) as jpn, \
                patch.object(engine.readers['kor'], 'predict', wraps=engine.readers['kor'].predict) as kor:
            result = engine.read_regions(image, regions)
        self.assertEqual(jpn.call_count, 1)
        self.assertEqual(kor.call_count, 1)
        self.assertEqual(result[0]['text'], '雨')
        self.assertEqual(result[0]['alternatives'][0]['text'], '가상의 글')
        self.assertEqual(result[0]['box'], result[0]['alternatives'][0]['box'])

    def test_batched_reader_exposes_partial_alternative(self):
        engine = worker.Engine.__new__(worker.Engine)
        engine.readers = {'jpn': Reader('[Author]青木そら', .93), 'kor': Reader('[Author]', .97)}
        engine.progress = lambda stage: None
        image = Image.new('RGB', (200, 100), 'white')
        regions = worker.regions_from([[(5, 5), (150, 5), (150, 20), (5, 20)]], 200, 100)
        result = engine.read_regions(image, regions)
        self.assertEqual(result[0]['text'], '[Author]')
        self.assertEqual(result[0]['alternatives'][0]['text'], '[Author]青木そら')
        self.assertEqual(result[0]['box'], result[0]['alternatives'][0]['box'])

    def test_retry_alternative_uses_parent_page_coordinates(self):
        engine = worker.Engine.__new__(worker.Engine)
        engine.progress = lambda stage: None
        image = Image.new('RGB', (200, 200), 'white')
        anchor = {'text': '著者', 'confidence': 99, 'language': 'jpn', 'box': [80, 100, 120, 110]}
        addition = {'text': '[Author]', 'confidence': 97, 'language': 'kor', 'box': [10, 10, 50, 30],
                    'alternatives': [{'text': '[Author]青木そら', 'confidence': 93, 'language': 'jpn', 'box': [10, 10, 50, 30]}]}
        with patch.object(engine, 'read_pass', side_effect=[[anchor], [addition]]):
            result = engine.read(image)
        row = next(line for line in result if 'alternatives' in line)
        self.assertEqual(row['box'], [45, 85, 65, 95])
        self.assertEqual(row['alternatives'][0]['box'], row['box'])

    def test_atomic_unicode_output(self):
        with tempfile.TemporaryDirectory(prefix='OCR 한글 ') as folder:
            path = Path(folder) / 'result.json'
            worker.write_json(path, {'text': '青木そら 홍길동'})
            self.assertIn('홍길동', path.read_text(encoding='utf-8'))
            self.assertFalse(path.with_suffix('.tmp').exists())


if __name__ == '__main__':
    unittest.main()
