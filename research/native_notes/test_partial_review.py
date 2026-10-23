import unittest

from .partial_review import reviewed_prefix


class PartialReviewTests(unittest.TestCase):
    def test_unreviewed_remainder_is_excluded(self):
        notes = [{"start": 1., "end": 2.}, {"start": 31., "end": 31.5}, {"start": 33., "end": 35.}]
        self.assertEqual(reviewed_prefix(notes, 40., 0., 32.), notes[:2])

    def test_crossing_note_and_nonprefix_range_are_rejected(self):
        with self.assertRaises(ValueError): reviewed_prefix([{"start": 31., "end": 33.}], 40., 0., 32.)
        with self.assertRaises(ValueError): reviewed_prefix([], 40., 1., 32.)
        with self.assertRaises(ValueError): reviewed_prefix([], 30., 0., 32.)


if __name__ == "__main__":
    unittest.main()
