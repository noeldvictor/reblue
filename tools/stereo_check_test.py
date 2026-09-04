"""Regression checks for capture analysis, without a headset or game data."""
import contextlib
import io
import random
import unittest

from PIL import Image
from stereo_check import disparity, report


class StereoCheckTest(unittest.TestCase):
    def test_black_layers_are_inconclusive(self):
        image = Image.new("RGB", (640, 960))
        self.assertEqual(disparity(image, 640, 960, [0.32, 0.95], 90, True), [])
        with contextlib.redirect_stdout(io.StringIO()), self.assertRaises(SystemExit) as ex:
            report(image, 640, 960, 90, True)
        self.assertEqual(ex.exception.code, 2)

    def test_textured_layers_keep_crossed_disparity(self):
        rng = random.Random(7)
        left = Image.frombytes("L", (640, 480), rng.randbytes(640 * 480))
        right = Image.new("L", left.size)
        right.paste(left.crop((4, 0, 640, 240)), (0, 0))
        right.paste(left.crop((16, 240, 640, 480)), (0, 240))
        image = Image.new("L", (640, 960))
        image.paste(left, (0, 0))
        image.paste(right, (0, 480))
        rows = disparity(image, 640, 960, [0.1, 0.7], 90, True)
        self.assertEqual(rows, [(0.1, -4), (0.7, -16)])


if __name__ == "__main__":
    unittest.main()
