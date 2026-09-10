# Extracts the Crysis cursor resources (ids 103-107) from the game's own Crysis2.exe
# into res/cursor_<id>.cur, so they can be linked into the launcher.
#
# Why this is needed: the game loads its cursor with LoadCursorA against the running
# executable, which is the launcher. The original Crysis2.exe carries those resources;
# a launcher built without them makes LoadCursorA return NULL and the cursor invisible -
# the mouse still works and menu buttons still highlight, but nothing is drawn.
#
# The .cur files are Crytek assets and are deliberately not stored in this repository.
# They are extracted from the copy of the game already installed on the machine.
import struct, os, sys

def extract(exe_path, out_dir):
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

    cursors, groups = {}, {}
    for nm, o2 in entries(base):
        if nm & 0x80000000 or nm not in (1, 12):
            continue
        for nm2, o3 in entries(base + (o2 & 0x7fffffff)):
            rid = nm2 & 0x7fffffff
            for _, o4 in entries(base + (o3 & 0x7fffffff)):
                drva, dsize = struct.unpack_from('<II', d, base + o4)
                raw = d[rva_to_off(drva):rva_to_off(drva) + dsize]
                (cursors if nm == 1 else groups)[rid] = raw
                break

    if not os.path.isdir(out_dir):
        os.makedirs(out_dir)

    made = 0
    for gid, g in sorted(groups.items()):
        count = struct.unpack_from('<H', g, 4)[0]
        imgs = []
        for i in range(count):
            w, h, planes, bits, nbytes, ordinal = struct.unpack_from('<HHHHIH', g, 6 + i * 14)
            cur = cursors.get(ordinal)
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

if __name__ == '__main__':
    exe = sys.argv[1] if len(sys.argv) > 1 else '../../Crysis 2/bin32/Crysis2.exe'
    out = sys.argv[2] if len(sys.argv) > 2 else 'res'
    n = extract(exe, out)
    print('extracted %d cursor group(s) into %s' % (n, out))
