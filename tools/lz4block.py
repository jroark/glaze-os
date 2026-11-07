"""LZ4 block format compressor and decompressor (standard library only).

Block format: https://github.com/lz4/lz4/blob/dev/doc/lz4_Block_format.md
Used by tools/mkromfs.py for compressed romfs pages, which kernel/lz4.c
decodes. Greedy matching over a hash table with a short chain: slower than
the reference compressor, but it runs at build time on 4 KiB pages.
"""

MIN_MATCH = 4
LAST_LITERALS = 5       # the block's last 5 bytes are always literals
MF_LIMIT = 12           # no match may start within 12 bytes of the end
MAX_OFFSET = 65535


def _length(out, n):
    while n >= 255:
        out.append(255)
        n -= 255
    out.append(n)


def compress(src, chain=16):
    n = len(src)
    out = bytearray()
    anchor = 0
    if n >= MF_LIMIT + 1:
        heads = {}
        prev = [-1] * n
        i = 0
        limit = n - MF_LIMIT
        while i < limit:
            key = src[i:i + MIN_MATCH]
            best_len = best_off = 0
            cand = heads.get(key, -1)
            tries = chain
            while cand >= 0 and i - cand <= MAX_OFFSET and tries:
                tries -= 1
                m = MIN_MATCH
                end = n - LAST_LITERALS
                while i + m < end and src[cand + m] == src[i + m]:
                    m += 1
                if m > best_len:
                    best_len, best_off = m, i - cand
                cand = prev[cand]
            prev[i] = heads.get(key, -1)
            heads[key] = i
            if best_len < MIN_MATCH:
                i += 1
                continue
            lit = i - anchor
            ml = best_len - MIN_MATCH
            out.append((min(lit, 15) << 4) | min(ml, 15))
            if lit >= 15:
                _length(out, lit - 15)
            out += src[anchor:i]
            out += bytes((best_off & 255, best_off >> 8))
            if ml >= 15:
                _length(out, ml - 15)
            # Index the matched bytes so later matches can refer into them.
            for j in range(i + 1, min(i + best_len, limit)):
                k = src[j:j + MIN_MATCH]
                prev[j] = heads.get(k, -1)
                heads[k] = j
            i += best_len
            anchor = i
    lit = n - anchor
    out.append(min(lit, 15) << 4)
    if lit >= 15:
        _length(out, lit - 15)
    out += src[anchor:]
    return bytes(out)


def decompress(src, size):
    out = bytearray()
    i = 0
    while i < len(src):
        token = src[i]; i += 1
        lit = token >> 4
        if lit == 15:
            while True:
                b = src[i]; i += 1
                lit += b
                if b != 255:
                    break
        out += src[i:i + lit]
        i += lit
        if i >= len(src):
            break
        off = src[i] | src[i + 1] << 8; i += 2
        if not off or off > len(out):
            raise ValueError("bad LZ4 offset")
        ml = (token & 15) + MIN_MATCH
        if token & 15 == 15:
            while True:
                b = src[i]; i += 1
                ml += b
                if b != 255:
                    break
        for _ in range(ml):
            out.append(out[-off])
    if len(out) != size:
        raise ValueError(f"LZ4 block decoded to {len(out)} bytes, not {size}")
    return bytes(out)
