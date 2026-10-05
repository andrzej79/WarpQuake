#!/usr/bin/env python3
"""Read a profile taken on the Amiga by WarpQuake (profsample, or -sample with a timedemo;
src/wqsample.c, a copy of warpPDFViewer's src/common/wpdfprof.c).

  wprof.py <profile> <vlink map of the SAME binary> [out.txt]

The map comes from "make prof" (build/WarpQuake_sym.map).  It matches build/WarpQuake as well,
because both are linked from the same objects in the same order; only the symbols differ.

Each sample holds the first 64 longwords at the main task's tc_SPReg. Where Exec put the PC
in there is learnt from the calibration samples, taken while the task spun inside
wprofCalibSpin(): the slot that points into that function in nearly every sample is the PC
slot.  A second slot is kept if it shows up too, since an FPU state frame can shift the
layout.  Every other sample is attributed through the map: first to the object file whose
code range holds the PC, then to the nearest global symbol before it.  vbcc leaves static
functions nameless, so their time lands on the global that precedes them.
"""
import sys, struct, re, bisect, collections

TS_WAIT = 4

def load_map(path):
    files, syms, code_size = [], [], None
    for line in open(path, errors='replace'):
        m = re.match(r'\s+[0-9a-f]{8} CODE\s+\(size ([0-9a-f]+)', line)
        if m and code_size is None:
            code_size = int(m.group(1), 16)
            continue
        m = re.match(r'\s+([0-9a-f]{8}) - ([0-9a-f]{8}) (.+)\(CODE\)', line)
        if m:
            files.append((int(m.group(1), 16), int(m.group(2), 16), m.group(3)))
            continue
        m = re.match(r'\s+0x([0-9a-f]+) (\S+):', line)
        if m and not re.match(r'^l\d+$', m.group(2)):
            syms.append((int(m.group(1), 16), m.group(2)))
    files.sort()
    syms.sort()
    return files, syms, code_size

def main():
    prof, mapfile = sys.argv[1], sys.argv[2]
    out = open(sys.argv[3], 'w') if len(sys.argv) > 3 else sys.stdout
    data = open(prof, 'rb').read()
    magic, ver, window, count, calib0, calib1 = struct.unpack('>6I', data[:24])
    if magic != 0x57505246:
        sys.exit('not a wprof profile')
    rec = (2 + window) * 4
    files, syms, code_size = load_map(mapfile)
    symaddr = dict((n, a) for a, n in syms)
    base = calib0 - symaddr['_wprofCalibSpin']
    code0, code1 = base, base + code_size
    if not (calib0 < calib1 < calib0 + 4096):
        calib1 = calib0 + 256

    samples = []
    for i in range(count):
        r = data[24 + i * rec:24 + (i + 1) * rec]
        tag, sp = struct.unpack('>2I', r[:8])
        samples.append((tag & 0xFF, (tag >> 8) & 0xFF, r[8:]))

    # The PC slot: the byte offset whose longword points into wprofCalibSpin().
    hits = collections.Counter()
    calib = [s for s in samples if s[0] == 1 and s[1] != TS_WAIT]
    for _, _, w in calib:
        for off in range(0, len(w) - 3, 2):
            v = struct.unpack('>I', w[off:off + 4])[0]
            if calib0 <= v < calib1:
                hits[off] += 1
    slots = [off for off, n in hits.most_common(3) if n >= max(3, len(calib) // 10)]
    if not slots:
        sys.exit('calibration found no PC slot (%d calibration samples)' % len(calib))

    fstarts = [f[0] for f in files]
    sstarts = [a for a, _ in syms]
    byfile, byfn = collections.Counter(), collections.Counter()
    waiting = outside = 0
    run = [s for s in samples if s[0] == 2]
    for _, state, w in run:
        if state == TS_WAIT:
            waiting += 1
            continue
        pc = None
        for off in slots:
            v = struct.unpack('>I', w[off:off + 4])[0]
            if code0 <= v < code1:
                pc = v
                break
        if pc is None:
            outside += 1
            continue
        o = pc - base
        i = bisect.bisect_right(fstarts, o) - 1
        fname = files[i][2] if i >= 0 and o < files[i][1] else '?'
        byfile[fname] += 1
        j = bisect.bisect_right(sstarts, o) - 1
        fn = syms[j][1] if j >= 0 and i >= 0 and sstarts[j] >= files[i][0] else '(static, before first global)'
        byfn[(fname, fn)] += 1

    inside = sum(byfile.values())
    total = len(run)
    out.write('%d samples (~1 ms each): %d in the program, %d running outside it (OS/ROM), %d waiting\n'
              % (total, inside, outside, waiting))
    out.write('PC slot(s) at tc_SPReg+%s, from %d calibration samples; code base %#x\n'
              % (','.join(str(s) for s in slots), len(calib), base))
    out.write('\n-- by source file, % of all running samples\n')
    running = inside + outside
    for name, n in byfile.most_common(40):
        out.write('%6.2f%%  %s\n' % (100.0 * n / running, name))
    out.write('%6.2f%%  (outside the program: OS, ROM, libraries)\n' % (100.0 * outside / running))
    out.write('\n-- by file + nearest preceding global (statics land on the global before them)\n')
    for (f, fn), n in byfn.most_common(60):
        out.write('%6.2f%%  %s: %s\n' % (100.0 * n / running, f, fn))

if __name__ == '__main__':
    main()
