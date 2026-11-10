"""Coldwood kit archives (Unravel Two): an index file `<name>.kit` and its data chunks `<name>.kit.0` .. `<name>.kit.<n-1>`.

Index: a 0x21-byte header {'kit!', u32 1, u32 1, u32 1, u32 0x40000 (the bucket table's bytes), u32 0, u32 file count, u32 chunk count, u8 '/'}, a hash bucket table
of 0x8000 8-byte entries (not needed to list the files), one 56-byte record per file {u32 name hash, u32 flags, u16 chunk, u48 offset, u64 stored size, u64 size,
20-byte SHA-1 of the file's bytes, u32 0}, then '!tik'. Flags bit 3: the file is LZ4 blocks, each a u32 length and an LZ4 block of at most 64 KB that may refer back
to the 64 KB before it in the same file (no frame); without it the file is stored as is. Unravel Two 1.0.1: 22,268 records over 12 chunks, which they tile exactly
(453 + 14 share another record's bytes); every file decodes to its size and SHA-1. The index has no names, so members are labelled by their hash.
Read through romfs.open so the chunks inside a RomFS image need no extraction.
"""
import hashlib
import struct
from romfs import open

MAGIC, FOOTER = b'kit!', b'!tik'
HEAD, BUCKET, RECORD = 0x21, 8, 56
LZ4 = 8


def entries(path):
    """→ [(name hash, flags, chunk, offset, stored size, size, sha1)] in index order."""
    with open(path, 'rb') as f: d = f.read()
    if len(d) < HEAD + 4 or d[:4] != MAGIC or d[-4:] != FOOTER: raise ValueError('not a kit index')
    bucket_bytes, _, count, chunks = struct.unpack_from('<IIII', d, 0x10)
    if bucket_bytes % BUCKET or HEAD + bucket_bytes + count * RECORD + 4 != len(d): raise ValueError(f'kit index of {len(d)} bytes does not hold {count} records')
    out = []
    for o in range(HEAD + bucket_bytes, HEAD + bucket_bytes + count * RECORD, RECORD):
        name, flags, chunk = struct.unpack_from('<IIH', d, o)
        offset = int.from_bytes(d[o + 10:o + 16], 'little')
        stored, size = struct.unpack_from('<QQ', d, o + 16)
        if chunk >= chunks: raise ValueError(f'kit record {name:08x}: chunk {chunk} of {chunks}')
        if not flags & LZ4 and stored != size: raise ValueError(f'kit record {name:08x}: stored file of {stored} bytes, size {size}')
        out.append((name, flags, chunk, offset, stored, size, d[o + 32:o + 52]))
    return out


def read(f, entry):
    """The file's bytes from its open chunk f, checked against its size and SHA-1."""
    name, flags, _, offset, stored, size, sha1 = entry
    f.seek(offset); d = f.read(stored)
    if len(d) != stored: raise ValueError(f'{name:08x}: truncated kit file')
    if flags & LZ4:
        import lz4.block
        out = bytearray(); p = 0
        while p < stored:
            if p + 4 > stored: raise ValueError(f'{name:08x}: truncated kit block header')
            n = struct.unpack_from('<I', d, p)[0]
            if p + 4 + n > stored: raise ValueError(f'{name:08x}: truncated kit block')
            out += lz4.block.decompress(d[p + 4:p + 4 + n], uncompressed_size=0x10000, dict=bytes(out[-0x10000:]))
            p += 4 + n
        d = bytes(out)
    if len(d) != size or hashlib.sha1(d).digest() != sha1: raise ValueError(f'{name:08x}: kit file does not match its size and SHA-1')
    return d
