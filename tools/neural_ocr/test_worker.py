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

    def test_atomic_unicode_output(self):
        with tempfile.TemporaryDirectory(prefix='OCR 한글 ') as folder:
            path = Path(folder) / 'result.json'
            worker.write_json(path, {'text': '青木そら 홍길동'})
            self.assertIn('홍길동', path.read_text(encoding='utf-8'))
            self.assertFalse(path.with_suffix('.tmp').exists())


if __name__ == '__main__':
    unittest.main()
