# Extracts the resources the launcher needs from the game's own Crysis2.exe:
#
#   res/cursor_<id>.cur   the cursor groups, ids 103-107
#   res/icon_101.ico      the game's icon, group id 101
#
# Why the cursors are needed: the game loads its cursor with LoadCursorA against the running
# executable, which with this project is the launcher rather than Crysis2.exe. The original
# carries those resources; a launcher built without them makes LoadCursorA return NULL and the
# cursor invisible - the mouse still works and menu buttons still highlight, but nothing is
# drawn under the pointer.
#
# Why the icon is needed: without one, Windows draws the default blank executable icon, and the
# launcher looks like a stray tool next to the game rather than a way to start it.
#
# Both are Crytek assets and are deliberately not stored in this repository. They are extracted
# from the copy of the game already installed on the machine, at build time.
import struct, os, sys, zlib

RT_CURSOR, RT_ICON, RT_GROUP_CURSOR, RT_GROUP_ICON = 1, 3, 12, 14


def _read_resources(exe_path):
    """Returns (images, groups) for both cursors and icons, keyed by resource id."""
    d = open(exe_path, 'rb').read()
    pe = struct.unpack_from('<I', d, 0x3c)[0]
    nsec = struct.unpack_from('<H', d, pe + 6)[0]
    optsz = struct.unpack_from('<H', d, pe + 20)[0]
    magic = struct.unpack_from('<H', d, pe + 24)[0]
    dd = pe + 24 + (112 if magic == 0x20b else 96)
    rrva, _ = struct.unpack_from('<II', d, dd + 16)
    if rrva == 0:
        raise SystemExit('%s has no resource directory' % exe_path)

    secs = []
    off = pe + 24 + optsz
    for _ in range(nsec):
        vs, va, rs, ra = struct.unpack_from('<IIII', d, off + 8)
        secs.append((va, vs, ra, rs))
        off += 40

    def rva_to_off(r):
        for va, vs, ra, rs in secs:
            if va <= r < va + max(vs, rs):
                return ra + (r - va)
        return None

    base = rva_to_off(rrva)

    def entries(o):
        nnamed, nid = struct.unpack_from('<HH', d, o + 12)
        return [struct.unpack_from('<II', d, o + 16 + i * 8) for i in range(nnamed + nid)]

    wanted = (RT_CURSOR, RT_ICON, RT_GROUP_CURSOR, RT_GROUP_ICON)
    out = {t: {} for t in wanted}
    for nm, o2 in entries(base):
        if nm & 0x80000000 or nm not in wanted:
            continue
        for nm2, o3 in entries(base + (o2 & 0x7fffffff)):
            rid = nm2 & 0x7fffffff
            for _, o4 in entries(base + (o3 & 0x7fffffff)):
                drva, dsize = struct.unpack_from('<II', d, base + o4)
                start = rva_to_off(drva)
                out[nm][rid] = d[start:start + dsize]
                break
    return out


def _write_cursors(res, out_dir):
    made = 0
    for gid, g in sorted(res[RT_GROUP_CURSOR].items()):
        count = struct.unpack_from('<H', g, 4)[0]
        imgs = []
        for i in range(count):
            w, h, planes, bits, nbytes, ordinal = struct.unpack_from('<HHHHIH', g, 6 + i * 14)
            cur = res[RT_CURSOR].get(ordinal)
            if cur is None:
                continue
            hx, hy = struct.unpack_from('<HH', cur, 0)   # hotspot precedes the DIB
            imgs.append((w, h // 2, hx, hy, cur[4:]))    # height is doubled (XOR+AND masks)
        if not imgs:
            continue
        buf = struct.pack('<HHH', 0, 2, len(imgs))       # ICONDIR, type 2 = cursor
        data_off = 6 + 16 * len(imgs)
        blobs = b''
        for (w, h, hx, hy, body) in imgs:
            buf += struct.pack('<BBBBHHII', w & 0xff, h & 0xff, 0, 0, hx, hy, len(body), data_off + len(blobs))
            blobs += body
        open(os.path.join(out_dir, 'cursor_%d.cur' % gid), 'wb').write(buf + blobs)
        made += 1
    return made


def _dib_to_png(dib):
    """Re-encodes one 32-bit icon image from its DIB into PNG. Returns None if it is not a
    32-bit bottom-up DIB, in which case the caller keeps the original bytes."""
    if len(dib) < 40:
        return None
    hdr_size, width, height, planes, bits = struct.unpack_from('<IiiHH', dib, 0)
    if hdr_size != 40 or bits != 32 or planes != 1:
        return None

    height //= 2                      # the DIB stores XOR and AND masks stacked
    if width <= 0 or height <= 0:
        return None

    pixels = dib[hdr_size:hdr_size + width * height * 4]
    if len(pixels) < width * height * 4:
        return None

    # DIB rows are bottom-up and BGRA; PNG wants top-down RGBA with a filter byte per row.
    rows = []
    for y in range(height - 1, -1, -1):
        src = pixels[y * width * 4:(y + 1) * width * 4]
        row = bytearray(len(src))
        for x in range(0, len(src), 4):
            row[x] = src[x + 2]       # R
            row[x + 1] = src[x + 1]   # G
            row[x + 2] = src[x]       # B
            row[x + 3] = src[x + 3]   # A
        rows.append(row)

    # Filtering is what makes PNG worth using here. Stored unfiltered, this icon barely
    # compresses at all - it is a photographic image with gradients. Each row is tried with
    # every filter and the one with the smallest sum of absolute differences is kept, which is
    # the heuristic the PNG specification suggests.
    raw = bytearray()
    prev = bytearray(width * 4)
    for row in rows:
        best, best_score = None, None
        for ftype in range(5):
            out = bytearray(len(row))
            for i in range(len(row)):
                a = row[i - 4] if i >= 4 else 0
                b = prev[i]
                c = prev[i - 4] if i >= 4 else 0
                if ftype == 0:
                    pred = 0
                elif ftype == 1:
                    pred = a
                elif ftype == 2:
                    pred = b
                elif ftype == 3:
                    pred = (a + b) >> 1
                else:
                    p = a + b - c
                    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                    pred = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                out[i] = (row[i] - pred) & 0xff
            score = sum(v if v < 128 else 256 - v for v in out)
            if best_score is None or score < best_score:
                best, best_score = (ftype, out), score
        raw.append(best[0])
        raw += best[1]
        prev = row

    def chunk(tag, data):
        return (struct.pack('>I', len(data)) + tag + data
                + struct.pack('>I', zlib.crc32(tag + data) & 0xffffffff))

    ihdr = struct.pack('>IIBBBBB', width, height, 8, 6, 0, 0, 0)   # 8-bit RGBA
    return (b'\x89PNG\r\n\x1a\n'
            + chunk(b'IHDR', ihdr)
            + chunk(b'IDAT', zlib.compress(bytes(raw), 9))
            + chunk(b'IEND', b''))


def _write_icons(res, out_dir):
    # A GRPICONDIRENTRY is a ICONDIRENTRY with the trailing 4-byte file offset replaced by a
    # 2-byte resource id, so the first 12 bytes carry over unchanged.
    made = 0
    for gid, g in sorted(res[RT_GROUP_ICON].items()):
        count = struct.unpack_from('<H', g, 4)[0]
        imgs = []
        for i in range(count):
            head = g[6 + i * 14: 6 + i * 14 + 12]
            ordinal = struct.unpack_from('<H', g, 6 + i * 14 + 12)[0]
            body = res[RT_ICON].get(ordinal)
            if body is None:
                continue
            # The large sizes dominate the file when stored raw: the 256x256 image alone is
            # 264 KB against 54 KB for the entire launcher. Windows Vista and later read PNG
            # inside .ico, so anything from 64 pixels up is re-encoded, which costs nothing in
            # quality and takes the icon from 345 KB to about 30 KB.
            width = head[0] or 256
            if width >= 64:
                png = _dib_to_png(body)
                if png is not None:
                    body = png
            imgs.append((head, body))
        if not imgs:
            continue
        buf = struct.pack('<HHH', 0, 1, len(imgs))       # ICONDIR, type 1 = icon
        data_off = 6 + 16 * len(imgs)
        blobs = b''
        for head, body in imgs:
            # Bytes 8..11 of the entry are dwBytesInRes. The value carried over from the
            # resource describes the original DIB, so it has to be rewritten with the size
            # actually being written - otherwise re-encoded images are described by a length
            # that no longer matches them.
            buf += head[:8] + struct.pack('<II', len(body), data_off + len(blobs))
            blobs += body
        open(os.path.join(out_dir, 'icon_%d.ico' % gid), 'wb').write(buf + blobs)
        made += 1
    return made


def extract(exe_path, out_dir):
    res = _read_resources(exe_path)
    if not os.path.isdir(out_dir):
        os.makedirs(out_dir)
    return _write_cursors(res, out_dir), _write_icons(res, out_dir)


if __name__ == '__main__':
    exe = sys.argv[1] if len(sys.argv) > 1 else '../../Crysis 2/bin32/Crysis2.exe'
    out = sys.argv[2] if len(sys.argv) > 2 else 'res'
    cursors, icons = extract(exe, out)
    print('extracted %d cursor group(s) and %d icon(s) into %s' % (cursors, icons, out))
