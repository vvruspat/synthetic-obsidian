import unittest

import numpy as np

from .train import Windows


class SynthesisSamplingTests(unittest.TestCase):
    def test_controlled_synthesis_uses_the_same_downweight_as_other_synthetic_data(self):
        example = {"features": np.zeros((152, 40), np.float32), "targets": np.zeros((3, 40), np.float32)}
        windows = Windows([dict(example), {**example, "augmentation": {}}, {**example, "synthesis": {}}])
        self.assertEqual(windows.sampling_weights(.1), [1.0, .1, .1])
        self.assertEqual(windows.sampling_weights(.1, .5), [1.0, .1, .5])


if __name__ == "__main__":
    unittest.main()
