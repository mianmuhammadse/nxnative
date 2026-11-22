# SMOL-V decoder: turns the compact SPIR-V encoding of Aras Pranckevicius' smol-v (https://github.com/aras-p/smol-v, encoding versions 0 and 1) back into a SPIR-V module.
# A Python port of smolv::Decode from smolv.cpp (public domain, or MIT, at the user's choice; authored 2016-2024 by Aras Pranckevicius). Godot stores the SPIR-V of every shader
# stage in its shader caches (shader_cache/<family>/<hash>/<hash>.vulkan.cache) as SMOL-V inside a zstd frame; dm/gfx/tools/vk_shaders.py reads them through this module.
# decode(data) -> the SPIR-V bytes, or raises ValueError when the data is not whole SMOL-V. Decoding only: nothing here encodes.
import struct

SMOL_MAGIC = 0x534D4F4C   # "SMOL", the bytes "LOMS" in a file
SPIRV_MAGIC = 0x07230203
MAX_VERSION = 1

# SpvOp numbers the decoder tells apart
OP_DECORATE, OP_MEMBER_DECORATE, OP_VECTOR_SHUFFLE, OP_VECTOR_SHUFFLE_COMPACT = 71, 72, 79, 13
OP_LOAD, OP_ACCESS_CHAIN = 61, 65
OP_MODULE_PROCESSED, OP_QUAD_SWAP = 330, 366   # the last operation of encoding version 0 and of version 1 (the table below has one row per operation up to the latter)
DEC_OFFSET = 35

# Per operation, packed: bit 0 has a result ID, bit 1 has a type ID, bit 2 the rest of the words are varints, bits 3 and up: how many words after the type and result are
# deltas from the result ID (kSpirvOpData in smolv.cpp, row for row)
OPS = (
    0, 3, 0, 4, 0, 0, 0, 0, 4, 3, 0, 1, 7, 23, 4, 4, 4, 4, 3, 5, 5, 5, 5, 5,
    5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 4, 3, 3, 3, 3, 75, 7, 3, 3,
    3, 3, 3, 75, 3, 3, 7, 3, 0, 75, 3, 7, 3, 15, 20, 0, 0, 7, 3, 3, 3, 3, 3, 4,
    4, 1, 0, 0, 3, 15, 23, 23, 75, 15, 23, 11, 3, 3, 3, 23, 23, 31, 31, 23, 23, 31, 31, 23,
    31, 31, 23, 28, 11, 11, 11, 19, 11, 19, 11, 11, 3, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11,
    11, 11, 11, 15, 11, 3, 11, 11, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19,
    19, 19, 19, 19, 19, 19, 19, 19, 19, 3, 11, 11, 11, 11, 11, 11, 11, 19, 19, 19, 19, 19, 19, 19,
    11, 27, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19,
    3, 3, 19, 19, 19, 19, 19, 19, 11, 35, 27, 27, 11, 11, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
    3, 3, 0, 0, 0, 0, 3, 3, 24, 16, 3, 3, 0, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
    3, 3, 3, 3, 3, 3, 20, 12, 1, 8, 28, 0, 0, 0, 0, 0, 0, 0, 3, 3, 0, 3, 3, 3,
    3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 0, 0, 3, 3, 3, 3, 3, 0,
    0, 3, 3, 3, 3, 3, 3, 3, 3, 0, 0, 3, 3, 0, 0, 3, 3, 23, 23, 31, 31, 23, 23, 31,
    31, 23, 31, 31, 11, 0, 3, 0, 3, 3, 3, 3, 3, 3, 3, 3, 7, 20, 3, 4, 4, 15, 15, 15,
    15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
    15, 15, 15, 15, 15, 15, 15,
)
assert len(OPS) == OP_QUAD_SWAP + 1

# The most common operations are swapped into the numbers below 16 for compact varints (smolv_RemapOp; the swap is its own inverse)
REMAP = {}
for _a, _b in ((71, 0), (61, 1), (62, 2), (65, 3), (79, 4), (72, 7), (248, 8), (59, 9), (133, 10), (129, 11), (32, 14), (127, 15)):
    REMAP[_a], REMAP[_b] = _b, _a


def _decoration_extra_ops(dec):   # how many words follow a member decoration of this kind; -1: the stream gives the length
    if dec == 0 or 2 <= dec <= 5: return 0
    if 29 <= dec <= 37: return 1
    return -1


def decode(data):
    data = bytes(data)
    if len(data) < 24: raise ValueError('SMOL-V: too short')
    magic, version_word, generator, bound, schema, size = struct.unpack_from('<6I', data, 0)
    version = version_word >> 24
    if magic != SMOL_MAGIC or version > MAX_VERSION or not 0x10000 <= version_word & 0xFFFFFF <= 0x10600 or size == 0 or size % 4:
        raise ValueError('SMOL-V: bad header')
    known = OP_MODULE_PROCESSED + 1 if version == 0 else OP_QUAD_SWAP + 1
    out = [SPIRV_MAGIC, version_word & 0xFFFFFF, generator, bound, schema]
    pos, end, prev_result, prev_decorate = 24, len(data), 0, 0
    limit = size // 4

    def varint():
        nonlocal pos
        v = shift = 0
        while pos < end:
            b = data[pos]; pos += 1
            v |= (b & 127) << shift; shift += 7
            if not b & 128: break
        return v & 0xFFFFFFFF

    def zig(u): return (u >> 1) ^ 0xFFFFFFFF if u & 1 else u >> 1

    while pos < end:
        val = varint()
        length, op = ((val >> 20) << 4) | ((val >> 4) & 0xF), ((val >> 4) & 0xFFF0) | (val & 0xF)
        op = REMAP.get(op, op)
        length += 1
        if op in (OP_VECTOR_SHUFFLE, OP_VECTOR_SHUFFLE_COMPACT): length += 4
        elif op == OP_DECORATE: length += 2
        elif op in (OP_LOAD, OP_ACCESS_CHAIN): length += 3
        swizzle = op == OP_VECTOR_SHUFFLE_COMPACT
        if swizzle: op = OP_VECTOR_SHUFFLE
        out.append((length << 16) | op)
        flags = OPS[op] if op < known else 0
        ioffs = 1
        if flags & 2: out.append(varint()); ioffs += 1   # type
        if flags & 1:   # result, a delta
            prev_result = (prev_result + zig(varint())) & 0xFFFFFFFF
            out.append(prev_result); ioffs += 1
        if op in (OP_DECORATE, OP_MEMBER_DECORATE):   # IDs relative to the previous decoration
            prev_decorate = (prev_decorate + zig(varint())) & 0xFFFFFFFF
            out.append(prev_decorate); ioffs += 1
        if op == OP_MEMBER_DECORATE:   # a run of member decorations of one struct
            if pos >= end: raise ValueError('SMOL-V: truncated')
            count = data[pos]; pos += 1
            prev_index = prev_offset = 0
            for m in range(count):
                index = prev_index = (varint() + prev_index) & 0xFFFFFFFF
                dec = varint()
                extra = _decoration_extra_ops(dec)
                mlen = varint() + 4 if extra == -1 else 4 + extra
                if m: out += [(mlen << 16) | op, prev_decorate]   # the first one's op and length were written above
                out += [index, dec]
                if dec == DEC_OFFSET:
                    if mlen != 5: raise ValueError('SMOL-V: bad member offset')
                    prev_offset = (varint() + prev_offset) & 0xFFFFFFFF
                    out.append(prev_offset)
                else:
                    for _ in range(4, mlen): out.append(varint())
            continue
        for _ in range(flags >> 3):   # IDs relative to the result
            if ioffs >= length: break
            out.append((prev_result - zig(varint())) & 0xFFFFFFFF); ioffs += 1
        if swizzle and length <= 9:
            if pos >= end: raise ValueError('SMOL-V: truncated')
            s = data[pos]; pos += 1
            for need, shift in ((5, 6), (6, 4), (7, 2), (8, 0)):
                if length > need: out.append((s >> shift) & 3)
        elif flags & 4:
            while ioffs < length: out.append(varint()); ioffs += 1
        else:
            if pos + 4 * (length - ioffs) > end: raise ValueError('SMOL-V: truncated')
            for _ in range(ioffs, length): out.append(struct.unpack_from('<I', data, pos)[0]); pos += 4
        if len(out) > limit: raise ValueError('SMOL-V: decodes past its declared size')
    if len(out) != limit: raise ValueError('SMOL-V: decoded size differs from the declared one')
    return struct.pack('<%dI' % len(out), *out)
