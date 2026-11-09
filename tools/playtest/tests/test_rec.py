import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import rec  # noqa: E402

HEADER = {'rvrec': 1, 'game': 'port', 'build': {'sha': 'x', 'name': 'test'},
          'start': {'from': 'newgame'}, 'seed': 1, 'tick_hz': 24, 'timing': 'lockstep'}


class RecordingFormat(unittest.TestCase):
    def roundtrip(self, recording: rec.Recording) -> rec.Recording:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / rec.RECORDING_NAME
            recording.save(path)
            return rec.Recording.load(Path(tmp))          # a bundle directory loads its recording

    def test_every_line_kind_roundtrips(self):
        items = [rec.Key(0, 'CONTROL', True), rec.Key(0, 'BACK', True), rec.Char(1, 97),
                 rec.Mouse(2, 412, 233, rec.BUTTON_LEFT), rec.Check(24, {'pos': [1, 2, 3], 'draws': 9}),
                 rec.Mark(30, 'quicksave', {'slot': 'Quick Save 1'}), rec.End(48)]
        loaded = self.roundtrip(rec.Recording(dict(HEADER), items))
        self.assertEqual(loaded.items, items)
        self.assertEqual(loaded.end, 48)
        self.assertEqual(loaded.checks, {24: {'pos': [1, 2, 3], 'draws': 9}})
        self.assertEqual(rec.validate(loaded), [])

    def test_wheel_roundtrips_without_changing_held_buttons(self):
        events = [rec.Mouse(0, 500, 400, rec.BUTTON_RIGHT), rec.Wheel(1, 500, 400, 120),
                  rec.Wheel(2, 500, 400, -60), rec.Mouse(3, 500, 400, 0)]
        for timing in ('lockstep', 'frames'):
            loaded = self.roundtrip(rec.Recording(dict(HEADER, timing=timing), events))
            self.assertEqual(loaded.events, events)
        for units in (True, 1.5, 1 << 31):
            with self.assertRaises(rec.FormatError):
                rec.parse_item({'t': 0, 'mouse': [500, 400], 'wheel': units})

    def test_frame_timing_keys_events_by_frame_and_keeps_exact_dt(self):
        dt = 0.016666666666666666
        items = [rec.Frame(0, dt), rec.Key(0, 'A', True), rec.Frame(1, 0.1), rec.Check(2, {'draws': 1})]
        loaded = self.roundtrip(rec.Recording(dict(HEADER, timing='frames'), items))
        self.assertEqual(loaded.items, items)
        self.assertEqual(loaded.items[0].dt, dt)       # bit for bit
        line = rec.item_line(items[1], 'frames')
        self.assertEqual(line, {'f': 0, 'key': 'A', 'down': True})

    def test_bad_lines_are_refused(self):
        for bad in ({'t': 1, 'key': 'NOPE', 'down': True}, {'t': 1, 'key': 'A'},
                    {'t': 1, 'char': 0}, {'t': 1, 'mouse': [1], 'buttons': 0},
                    {'t': 1, 'mouse': [1, 2], 'buttons': 8}, {'t': 'x', 'char': 65}, {'t': 1}):
            with self.subTest(bad=bad), self.assertRaises(rec.FormatError):
                rec.parse_item(bad)

    def test_key_codes(self):
        self.assertEqual(rec.vk_code('PRIOR'), 0x21)
        self.assertEqual(rec.vk_code('F9'), 0x78)
        self.assertEqual(rec.vk_code('0x5D'), 0x5D)
        self.assertEqual(rec.VK_NAME[0x41], 'A')

    def test_validate_finds_order_and_bounds_problems(self):
        recording = rec.Recording(dict(HEADER), [rec.Key(5, 'A', True), rec.Key(3, 'A', False),
                                                 rec.Mouse(6, 640, 10, 0), rec.End(7), rec.Char(8, 65)])
        problems = rec.validate(recording)
        self.assertTrue(any('back in time' in p for p in problems))
        self.assertTrue(any('off the 640x480' in p for p in problems))
        self.assertTrue(any('end line' in p for p in problems))


class Divergence(unittest.TestCase):
    def test_first_differing_tick_and_only_shared_fields(self):
        a = {24: {'pos': [1, 2, 3], 'draws': 5}, 48: {'pos': [2, 2, 3], 'draws': 9, 'mana': 4}}
        b = {24: {'pos': [1, 2, 3], 'draws': 5, 'extra': 1}, 48: {'pos': [2, 2, 3], 'draws': 10}}
        d = rec.compare_checks(a, b)
        self.assertEqual((d.t, d.fields), (48, {'draws': (9, 10)}))
        self.assertIsNone(rec.compare_checks(a, {24: a[24]}))
        self.assertIn('tick 48', str(d))


class SlotHash(unittest.TestCase):
    def test_spec_example(self):
        with tempfile.TemporaryDirectory() as tmp:
            slot = Path(tmp)
            (slot / 'CurMap').mkdir()
            (slot / 'game.sav').write_bytes(b'abc')
            (slot / 'CurMap' / '1_2_3.DAT').write_bytes(b'xyz')
            import hashlib
            expected = hashlib.sha256(b'curmap/1_2_3.dat\0xyz\0game.sav\0abc\0').hexdigest()
            self.assertEqual(rec.slot_sha(slot), expected)


if __name__ == '__main__':
    unittest.main()
