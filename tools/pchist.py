#!/usr/bin/env python3
"""Per-instruction histogram of one asm function from a WarpQuake sample profile.

  pchist.py <profile> <vlink map> <object .s file> [<global symbol> [<lines around>]]

Like wprof.py (same profile and map), but for the samples inside one object file of
assembly: each sampled PC is matched against a vasm listing of that file (assembled here with
-L, with the Makefile's flags, so offsets are those of the linked object) and printed next to
its source line.  With a symbol, only the samples from it to the next global count.  A sample
names the instruction AFTER the one that stalled (the PC the 68060 saves), so a hot line
usually means the line before it waits for something.
"""
import os, re, subprocess, sys, tempfile, collections, bisect, struct

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import wprof

def listing(src):
    """{section offset: source text} from a vasm listing of src (CODE section only)."""
    tmp = tempfile.mkdtemp()
    lst = os.path.join(tmp, 'x.lst')
    subprocess.run(['vasmm68k_mot', '-quiet', '-Fhunk', '-m68060', '-Iquake', '-L', lst,
                    '-o', os.path.join(tmp, 'x.o'), src], check=True, stdout=subprocess.DEVNULL)
    lines = {}
    for line in open(lst, errors='replace'):
        # "00:00000ABC 4E75                 1234:     rts", or a macro's
        # expansion, numbered within the macro: "...     7M \tmove.b ..."
        m = re.match(r'([0-9A-F]{2}):([0-9A-F]{8}) ([0-9A-F]+)\s+(\d+:|\d+M)\s?(.*)$', line)
        if m and m.group(1) == '00':
            off = int(m.group(2), 16)
            lines.setdefault(off, m.group(5).rstrip())
    return lines

def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    prof, mapfile, src = sys.argv[1:4]
    fn = sys.argv[4] if len(sys.argv) > 4 else None
    data = open(prof, 'rb').read()
    magic, ver, window, count, calib0, calib1 = struct.unpack('>6I', data[:24])
    rec = (2 + window) * 4
    files, syms, code_size = wprof.load_map(mapfile)
    symaddr = dict((n, a) for a, n in syms)
    base = calib0 - symaddr['_wprofCalibSpin']
    obj = os.path.basename(src)              # vlink names asm units by source
    frange = [f for f in files if f[2].strip().endswith(obj)]
    if not frange:
        sys.exit('%s not in the map' % obj)
    f0, f1 = frange[0][0], frange[0][1]
    lo, hi = f0, f1
    if fn:
        lo = symaddr[fn]
        later = [a for a, _ in syms if a > lo]
        hi = min(later) if later else f1

    # the PC slot, as wprof.py finds it
    samples = []
    for i in range(count):
        r = data[24 + i * rec:24 + (i + 1) * rec]
        tag, _ = struct.unpack('>2I', r[:8])
        samples.append((tag & 0xFF, (tag >> 8) & 0xFF, r[8:]))
    hits = collections.Counter()
    for t, st, w in samples:
        if t == 1 and st != wprof.TS_WAIT:
            for off in range(0, len(w) - 3, 2):
                v = struct.unpack('>I', w[off:off + 4])[0]
                if calib0 <= v < (calib1 if calib1 > calib0 else calib0 + 256):
                    hits[off] += 1
    slots = [off for off, n in hits.most_common(3) if n >= 3]

    pcs = collections.Counter()
    running = 0
    for t, st, w in samples:
        if t != 2 or st == wprof.TS_WAIT:
            continue
        running += 1
        for off in slots:
            v = struct.unpack('>I', w[off:off + 4])[0] - base
            if 0 <= v < code_size:
                if lo <= v < hi:
                    pcs[v - f0] += 1
                break
    lines = listing(src)
    offs = sorted(lines)
    total = sum(pcs.values())
    print('%d samples in %s%s (%.2f%% of %d running)' % (total, obj, ' ' + fn if fn else '',
          100.0 * total / max(running, 1), running))
    shown = collections.Counter()
    for pc, n in pcs.items():
        i = bisect.bisect_right(offs, pc) - 1
        shown[offs[i] if i >= 0 else pc] += n
    for off in sorted(shown):
        n = shown[off]
        print('%6d %5.1f%%  %06x  %s' % (n, 100.0 * n / total, off, lines.get(off, '?')))

if __name__ == '__main__':
    main()
