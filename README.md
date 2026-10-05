# warpQuake

Quake for AmigaOS 3.x on a Warp 68060 card, Picasso96 only.

**Status: v1.** id's software renderer on an 8-bit CLUT RTG screen at any mode from 320x200
to 1280x1024, with keyboard and mouse, plus built-in profiling. There is no sound, CD audio or
networking yet. v0 ran `timedemo demo1` at 16.8 fps on a 68060 @ 108 MHz (320x200). The
fastest port known on the same machine, ClickBOOM's, does 25.4.

The engine in `quake/` is id's GPL WinQuake through
[quakegeneric](https://github.com/erysdren/quakegeneric). That is WinQuake with the x86
assembly and the DOS/Windows code removed, so the renderer is pure C. It was imported
unchanged (see the first commit touching `quake/`); every change since is an ordinary diff.
`src/` is the AmigaOS platform layer.

## Roadmap

- **v0**: runs at all. 320x200x8, null sound.
- **v1**: a screen-mode chooser and real resolutions. Instrumentation: phase timers per
  frame, the on-machine sampling profiler, and bytes moved per stage.
- **v2**: optimise what v1 measures (span drawing in asm, surface cache, edge sorting,
  direct-to-VRAM).
- **v3+**: the fastest Amiga Quake. Offload stages to the board's ARM or the FPGA where the
  transfer costs less than the 68060 time it saves.

## Building

```
make -j8            # build/WarpQuake (and copy to TARGET_DIR)
make app            # build only
make prof           # build/WarpQuake_sym, unstripped, plus a link map
make ftp            # upload to WORK:Games/warpQuake (AMIGA_FTP_HOST/USER/PASSWORD)
make ftp-data       # upload the paks to WORK:Games/warpQuake/id1 (create it first)
```

vbcc, `-cpu=68060 -fpu=68060 -O1` with `-lm060`. The optimisation level is -O1 and not -O2
because of vbcc's known -O2 loop miscompile (see warpPDFViewer). `OPT=2` builds an experiment
into a separate object directory.

The engine keeps no AmigaOS header next to `quakedef.h`, because exec's inline macros clash
with Quake's identifiers. Whatever the engine needs from the OS goes through the `QG_*` hooks
in `quake/quakegeneric.h`, which are implemented in `src/`.

## Installing and running

```
WORK:Games/warpQuake/WarpQuake
WORK:Games/warpQuake/id1/pak0.pak
WORK:Games/warpQuake/id1/pak1.pak      (registered version)
```

Run it from a Shell: `WarpQuake [options]`. It finds `id1/` next to the program (PROGDIR:).

**Screen mode.** On the first start an ASL requester lists the 8-bit RTG modes from 320x200
to 1280x1024 (the engine's limits). The choice is saved in `PROGDIR:WarpQuake.mode` and used
from then on. `-asl` asks again. `-modeid` or `-width`/`-height` override it for one run,
which is handy in benchmark scripts. The engine renders at the mode's full size.

| option | meaning |
|---|---|
| `-asl` | choose the screen mode again |
| `-modeid 0x...` | use this mode |
| `-width W -height H` | use the 8-bit mode nearest WxH (headless: render at WxH) |
| `-aspect <f>` | pixel aspect (height/width). The default: modes up to 640 wide are shown 4:3, wider modes are square |
| `-mem <MB>` | hunk size, 16 by default |
| `-basedir <dir>` | where `id1/` is, if not next to the program |
| `-benchmark` | quit after the first timedemo |
| `-headless` | no screen at all; used for vamos runs |
| `+<command>` | any console command, e.g. `+timedemo demo1` |

The console is on the key left of `1`. Esc opens the menu.

Cvar `vid_blit` (saved in config.cfg) picks how a frame reaches the screen. `0` uses
WriteChunkyPixels. `1` locks the bitmap with p96LockBitMap and copies rows into video memory.
The profile shows what each costs.

## Profiling

**Benchmark with a phase profile:**

```
WarpQuake -benchmark +timedemo demo1
```

At the end of every timedemo the console, the Shell and `RAM:WarpQuake_prof.txt`
(`-proflog <file>` to change) get a table like this:

```
---- warpQuake profile: 968 frames, 59.52 ms/frame (16.8 fps) ----
phase                            ms/frm      %   calls
  3D view (R_RenderView)  ...
    world BSP + edges     ...
    scan edges (all)      ...
      edge sort + span gen
      draw surfaces
        surface cache build
        spans, z, sky, water
    alias models + sprites
  ...
-- work per frame
world span pixels / z-buffer words / alias pixels / surface cache KB built / blit KB
-- timer: ... us/read, ... reads/frame = ... ms/frame of overhead
```

The timers are E-clock reads at phase boundaries only, a few dozen per frame. The last line
says what they cost. Inside the hot loops there are only counter increments; those give the
bytes each stage moves, which is the number that decides whether an offload to the ARM or
FPGA could pay. The console commands are `prof` (report now; `prof <file>` also writes it)
and `profreset`.

**Function-level hot spots** (the sampling profiler from warpPDFViewer):

```
WarpQuake -benchmark -sample +timedemo demo1    # writes RAM:WarpQuake.wprf (-samplefile <f>)
make prof                                       # on the Mac: the link map of the same objects
tools/amiget.py ram:WarpQuake.wprf
tools/wprof.py temp/WarpQuake.wprf build/WarpQuake_sym.map
```

`profsample` and `profsample stop` do the same by hand from the console. The `make prof` map
matches the uploaded `build/WarpQuake`, as long as both come from the same build. vbcc leaves
static functions nameless, so their time lands on the global function before them.

**Checking that an optimisation draws the same pixels** (`-crc`):

```
WarpQuake -headless -basedir work: -benchmark -crc +host_framerate 0.05 +timedemo demo1
```

This prints a CRC-32 over every frame rendered. `-crcfile <f>` writes one line per frame,
which locates the first frame that differs. With a fixed `host_framerate` the timedemo renders
the same frames at any speed. `-crc` reseeds `rand()` at the timedemo (the particles use it) and
hides the console notify lines (they expire by wall-clock time). A match holds only between
builds that evaluate floats the same way. An integer rewrite (a span loop in asm) must match
exactly. A different `-O` level, or an FPU-path change, legitimately moves a few pixels, so for
those compare the frame lists instead. The reference at 320x200, current build, under vamos:
`a00347db`.

## Checking on the Mac (vamos)

`-headless` runs the whole engine without graphics.library. Under vamos (see the vamos note in
the csAmigaWarp notes: amitools from git, machine68k, greenlet) this checks loading, game code
and the renderer end to end. It does not measure speed.

```
vamos -C 68040 -s 600 -m 65536 -H disable -V work:<dir> -- work:WarpQuake -headless -basedir work: -benchmark +timedemo demo1
```

`-basedir work:` is needed because vamos has no PROGDIR:. The `--` is needed because otherwise
vamos takes `-basedir` as one of its own options.
