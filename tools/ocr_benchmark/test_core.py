import unittest
from core import field_errors, ordered_boxes


class ComparisonTests(unittest.TestCase):
    def test_field_distance_not_fuzzy_success(self):
        self.assertEqual(field_errors('青い空', 'タイトル\n育い空\n著者'), 1)
        self.assertEqual(field_errors('홍길동', '작가 홍 길 동'), 0)
        self.assertEqual(field_errors('ABC', ''), 3)
        self.assertEqual(field_errors('ABC', 'ＡＢＣ'), 0)
        with self.assertRaises(ValueError):
            field_errors('', 'anything')

    def test_boxes_bounded_and_invalid_rejected(self):
        self.assertEqual(ordered_boxes([[[-10, -10], [50, -10], [50, 50], [-10, 50]]], 40, 40), [[0, 0, 40, 40]])
        self.assertEqual(ordered_boxes([], 40, 40), [])
        with self.assertRaises(ValueError):
            ordered_boxes([[[float('nan'), 0]] * 4], 40, 40)
        with self.assertRaises(ValueError):
            ordered_boxes([[[0, 0]] * 3], 40, 40)

    def test_vertical_order(self):
        polys = [[[x, 10], [x + 10, 10], [x + 10, 90], [x, 90]] for x in (10, 60)]
        boxes = ordered_boxes(polys, 100, 100)
        self.assertGreater(boxes[0][0], boxes[1][0])


if __name__ == '__main__':
    unittest.main()
