"""SMOL-V decoding and Godot shader caches: dm/tools/smolv.py and the 'godot' input kind of dm/gfx/tools/vk_shaders.py.
The two SMOL-V fixtures are modules of Until Then's shader cache ({l1} and {l2} bytes) encoded with the reference encoder (smol-v, encoding version 1); their keys are the first 8 bytes of the
decoded module's SHA-256, the names Until Then's shaders have in the host's capture folders. The second has member decorations, vector shuffles and access chains."""
import base64
import hashlib
import struct
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/tools'))
sys.path.insert(0, str(REPO / 'dm/gfx/tools'))
import smolv  # noqa: E402
import vk_shaders as vs  # noqa: E402
try: import zstandard
except ImportError: zstandard = None

FIXTURES = (('b05799013f261d44', 'TE9NUwAFAQELAAgAEwAAAAAAAABYAgAAkQIB0RACR0xTTC5zdGQuNDUwAAAAAKAEAAHvDgQE7cKl8wYACQ2gAgQHoQgCwgM1BAAAAG1haW4AAAAARQkAAABmcmFnX2NvbG9yAAA1CwAAAEluZm8AAAAANgsAAAAAAAAAbXZwAEYLAAAAAQAAAGNvbG9yAAAANQ0AAABpbmZvAAAAABASHgA3BAQABQAjAAAHARABI0AAAAKTAgKhBAICpgIGILcCAgYEPgIDBzkIAgO4AgIHBL4CAgoHPgIJCzkMAgm1AgIgAbsEDgIBAAAAPgIJB8YGAhcAAxgCExAYDQ8BBwICIhIAjR6IBg=='), ('6c29f1aa325c994a', 'TE9NUwAFAQELAAgAMgAAAAAAAABEBQAAkQIB0RACR0xTTC5zdGQuNDUwAAAAAKAEAAH/DgAE7cKl8wYAGBwpoQgCwgM1BAAAAG1haW4AAAAARQwAAABiYXNlX2FycgAAAABVFgAAAGdsX1BlclZlcnRleAAAAABWFgAAAAAAAABnbF9Qb3NpdGlvbgBmFgAAAAEAAABnbF9Qb2ludFNpemUAAAAAZhYAAAACAAAAZ2xfQ2xpcERpc3RhbmNlAGYWAAAAAwAAAGdsX0N1bGxEaXN0YW5jZQAlGAAAAAAAAABVHAAAAGdsX1ZlcnRleEluZGV4AABFKQAAAHV2X2ludGVycAAAAEcsBAALAQABCwEBAQsBAwELAQQAAAIQDAsqEBoeAJMCAqEEAgKmAgYgtwICBgK1AgIgALsECAIDAAAAvAICBwk+AgcKuwQGBAAAgL/MBAcCAgK7BAYCAABAQMwEBwIGAswEBwIECNwECgIIBAK3AgIGBLsECAIBAAAAvAICBhTeAgITBhUVPgIDFjkXAgO1AgIgAbsEGQIAAAAAPgIBGTkbAgE+BAcHuwQGBgAAAAC7BAYCAACAPz4IAxM+BAMHOSgCA8wEBwgYGMwEBwIYGLsEBgQAAABAxgYCVwADGAI5Cw4HIgALARkiAhMeBAwdAQcCAsEKBgYGAMEKBgIIAeAKEwIEAggGEyYEGBoiAAQTJgYYGgETAgItBwICAhB8BwYBKywtLs4QBwQEAiIQAI0eiAY='))


def key(module): return hashlib.sha256(module).hexdigest()[:16]


class Smolv(unittest.TestCase):
    def test_fixtures_decode_to_the_original_modules(self):
        for k, b in FIXTURES:
            module = smolv.decode(base64.b64decode(b))
            self.assertEqual(key(module), k)
            self.assertTrue(vs.whole_module(module, 0, len(module)))

    def test_damaged_data_is_refused(self):
        data = base64.b64decode(FIXTURES[1][1])
        for bad in (b'', data[:20], data[:-7], b'SPIR' + data[4:], struct.pack('<I', 0x07230203) + data[4:], data[:5] + b'\xff' * 4 + data[9:]):
            with self.assertRaises(ValueError): smolv.decode(bad)


@unittest.skipUnless(zstandard, 'needs the zstandard module')
class GodotCache(unittest.TestCase):
    def cache(self, *stages, junk=b''):   # a cache file: the header, then each stage as a zstd frame between other bytes
        out = b'GDSC' + struct.pack('<II', 3, len(stages)) + junk
        for s in stages: out += struct.pack('<I', len(s)) + zstandard.ZstdCompressor().compress(s) + b'\0\0\0'
        return out

    def test_smolv_and_plain_stages_are_found_and_chance_matches_are_dropped(self):
        a, b = (base64.b64decode(x) for _, x in FIXTURES)
        plain = smolv.decode(b)
        data = self.cache(a, plain, b'not a shader', junk=zstandard.MAGIC_NUMBER.to_bytes(4, 'little') if hasattr(zstandard, 'MAGIC_NUMBER') else b'\x28\xb5\x2f\xfd')
        found = [key(m) for _, m in vs.godot_modules(data)]
        self.assertEqual(found, [FIXTURES[0][0], FIXTURES[1][0]])

    def test_a_cache_file_in_a_folder_is_a_shader_input_and_modules_dedup_by_key(self):
        a, b = (base64.b64decode(x) for _, x in FIXTURES)
        with tempfile.TemporaryDirectory() as d:
            for n, stages in (('one.vulkan.cache', (a, b)), ('two.vulkan.cache', (b,)), ('other.bin', (a,))):
                Path(d, n).write_bytes(self.cache(*stages))
            self.assertEqual({k for k, _, _ in vs.spirv_sources([d])}, {FIXTURES[0][0], FIXTURES[1][0]})
            self.assertEqual(len(list(vs.spirv_sources([d]))), 2)   # two.vulkan.cache's module is the second file's second
            self.assertEqual(len(list(vs.spirv_files(d))), 2)       # one.vulkan.cache and two.vulkan.cache; other.bin has no header


if __name__ == '__main__':
    unittest.main()
