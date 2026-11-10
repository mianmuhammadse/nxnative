"""Coldwood kit archives: the index, linked LZ4 blocks, stored members and Phyre effects inside, without game files."""
from pathlib import Path
import hashlib
import os
import struct
import sys
import tempfile
import unittest

import lz4.block

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
os.environ.setdefault('DM_GAME', '010015100B514000')
import shader_split as split   # puts dm/tools (romfs) on the path
import kit
from test_vbf import program


def lz4_member(data):
    """Linked 64 KB blocks: each block may refer to the 64 KB before it, as the game's packer writes them."""
    out = b''
    for i in range(0, len(data), 0x10000):
        blk = lz4.block.compress(data[i:i + 0x10000], store_size=False, dict=data[max(0, i - 0x10000):i])
        out += struct.pack('<I', len(blk)) + blk
    return out


def index(records, chunks):
    buckets = 0x10 * 8
    head = b'kit!' + struct.pack('<IIIIIII', 1, 1, 1, buckets, 0, len(records), chunks) + b'/'
    body = b''
    for name, flags, chunk, offset, stored, data in records:
        body += struct.pack('<IIH', name, flags, chunk) + offset.to_bytes(6, 'little') + struct.pack('<QQ', len(stored), len(data))
        body += hashlib.sha1(data).digest() + bytes(4)
    return head + bytes(buckets) + body + b'!tik'


class Kit(unittest.TestCase):
    def build(self, tmp, members):
        """members: [(name, data, compressed)] all in chunk 0 → the index path"""
        chunk, records = b'', []
        for name, data, packed in members:
            stored = lz4_member(data) if packed else data
            records.append((name, 9 if packed else 2, 0, len(chunk), stored, data)); chunk += stored
        root = Path(tmp)
        (root / 'Data.kit.0').write_bytes(chunk)
        (root / 'Data.kit').write_bytes(index(records, 1))
        return root / 'Data.kit'

    def test_linked_blocks_and_stored_members(self):
        big = (bytes(range(256)) * 300)[:0x10000] * 3 + b'tail' * 999   # later blocks refer back into the block before them
        with tempfile.TemporaryDirectory() as tmp:
            path = self.build(tmp, [(1, big, True), (2, b'stored member', False)])
            es = kit.entries(path)
            self.assertEqual([e[0] for e in es], [1, 2])
            with open(str(path) + '.0', 'rb') as f: self.assertEqual([kit.read(f, e) for e in es], [big, b'stored member'])
            bad = bytearray(path.read_bytes()); bad[0x21 + 0x80 + 32] ^= 1   # the first record's SHA-1
            path.write_bytes(bad)
            with open(str(path) + '.0', 'rb') as f:
                with self.assertRaises(ValueError): kit.read(f, kit.entries(path)[0])
            path.write_bytes(bytes(bad[:-8]) + b'!tik')
            with self.assertRaises(ValueError): kit.entries(path)

    def test_split_reads_effects_and_skips_chunks(self):
        effect = b'RYHP' + bytes(9) + program('vs') + bytes(7) + program('ps')
        with tempfile.TemporaryDirectory() as tmp:
            path = self.build(tmp, [(0xabc, effect, True), (0xdef, b'not an effect', True)])
            rel, keys, _, extra = split.scan_file((str(path), 'Data.kit'))
            self.assertEqual(sorted(v[1] for v in keys.values()), ['ps', 'vs'])
            self.assertTrue(all(v[0].startswith('/Data.kit.0/00000abc#PHYRE@') for v in keys.values()))
            self.assertEqual(split.scan_file((str(path) + '.0', 'Data.kit.0'))[1], {})


if __name__ == '__main__': unittest.main()
