import unittest
try:
    import tomllib
except ImportError:  # Python 3.9/3.10 can still run the other settings tests.
    tomllib = None
from linux_settings import update


class SettingsTests(unittest.TestCase):
    @unittest.skipIf(tomllib is None, "stdlib TOML parser requires Python 3.11")
    def test_renderer_is_valid_toml_and_preserves_other_settings(self):
        source = 'fps_cap = 240\nlight_streaks = false\nroad_shine = false\n'
        result = update(source, {'linux_renderer': 'vulkan'})
        parsed = tomllib.loads(result)
        self.assertEqual(parsed, dict(fps_cap=240, light_streaks=False, road_shine=False,
                                      linux_renderer='vulkan'))
        self.assertEqual(tomllib.loads(update(result, {'linux_renderer': 'opengl'}))['linux_renderer'], 'opengl')

    def test_updates_root_preserving_sections_and_unrelated_values(self):
        source = '# keep\nfps_cap = 30 # old\njoin_ip = "example"\n[other]\nfps_cap = 12\n'
        result = update(source, {'fps_cap': 120, 'motion_blur': False})
        self.assertIn('fps_cap = 120\n', result)
        self.assertIn('join_ip = "example"', result)
        self.assertIn('motion_blur = false\n[other]\nfps_cap = 12', result)
        self.assertTrue(result.startswith('# keep\n'))

    def test_idempotent_and_reversible(self):
        disabled = update('', {'light_streaks': False})
        self.assertEqual(update(disabled, {'light_streaks': False}), disabled)
        self.assertEqual(update(disabled, {'light_streaks': True}).count('light_streaks'), 1)
        self.assertIn('light_streaks = true', update(disabled, {'light_streaks': True}))

    def test_indented_root_key_does_not_create_duplicates(self):
        result = update('  fps_cap = 30\n', {'fps_cap': 60})
        self.assertEqual(result, 'fps_cap = 60\n')


if __name__ == '__main__':
    unittest.main()
