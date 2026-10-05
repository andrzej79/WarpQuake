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

`quake/*.s` holds 68060 assembly that replaces C routines, assembled with vasm. `WQ_ASM` in
`quakedef.h` selects it, the way `id386` once selected id's x86 code. `make ASM=0` builds the
all-C reference (`build/WarpQuake_C`, its own object directory), and every assembly routine must
match it under `-crc`. Currently in assembly: `D_DrawSpans8`, `D_DrawSpans16` and `D_DrawZSpans` (`d_spans060.s`), and
`R_DrawSurfaceBlock8_mip0..3`, the surface-cache lighting (`r_surf060.s`), and the edge-list scan
core: `R_GenerateSpans`, `R_StepActiveU`, `R_InsertNewEdges`, `R_RemoveEdges` (`r_edge060.s`).
Brush-model surfaces still go through the C `R_LeadingEdge`, whose 1/z ordering is float. The alias-model rasterizer's integer parts are in `d_polyse060.s`: the span drawer
`D_PolysetDrawSpans8`, the left-edge walker `D_PolysetScanLeftEdge`, and the subdivision
rasterizer `D_PolysetRecursiveTriangle`. `R_ClipEdge` with `R_EmitEdge` folded in is in
`r_draw060.s`: vbcc's float code for both, with the recursion turned into a loop and one register
save per edge. The platform side also
has `src/move16.s`, the MOVE16 copies.

Other v2 changes to the engine:
- **Surface cache at least 4 MB** (`d_surf.c`). The default 600 KB evicted about 10% of the
  blocks rebuilt each frame. `-surfcachesize <KB>` still overrides it.
- **z coverage** (`d_zcover.c`, cvar `r_zcover`). Only alias models, sprites and particles ever
  read the z-buffer, so before the world is drawn the screen area they can touch is bounded row
  by row, and `D_DrawZSpans` writes only that. In demo1 that cuts z writes by 65%. The values
  written are unchanged, so frames are identical. `r_zcover 0` writes every row in full, and
  `-crc` against it is the check that no bound is too small.
- **16-pixel span subdivision** (`D_DrawSpans16`, cvar `d_subdiv16`, default 1). This is what
  id's x86 assembly drew in DOS and Windows Quake. It halves the divides and the per-block work
  of the 8-pixel C version, which `d_subdiv16 0` still selects. Texels shift by a sub-pixel amount
  against the 8-pixel version, so it has its own C reference and its own CRC.
- **World vertices are projected once per view setup** (`R_ProjectedVertex`, `r_draw.c`). A vertex
  is shared by several edges. `R_EmitEdge` now projects both ends with one function and keeps the
  result per vertex until `modelorg` or the view axes change (`r_projstamp`). In demo1 that is 172
  projections and 237 reuses a frame. It changes edge positions by sub-pixel rounding against id's
  code, which projected the two ends of an edge at different precisions, so it has its own CRCs.
- **Particles are projected once** (`D_ProjectParticle`, `d_part.c`). The z coverage pass needs
  their screen positions before the world is drawn, and `D_DrawParticle` reuses them.
- **Particle limit 1536** instead of 2048 (`r_part.c`; `-particles <n>` overrides). demo1 and
  demo3 hit 2048 in bursts, since one explosion spawns 1024, and at that peak particles cost about
  20% of a frame. A single explosion still gets all its particles. The profile reports the average
  and peak active count.
- **Not `-O2`.** Thirteen hot C files compiled frame-identical at `-O2` but ran slower on the
  68060 (world BSP 6.98 → 7.34 ms), and the whole engine at `-O2` does not even give identical
  frames. `O2_FILES` in the Makefile is the knob for further experiments.
- **Model lighting cached per entity** (`R_LightPointEntity`, `r_light.c`): the light trace's hit
  is kept while the entity stands still, and only the light-style sum is recomputed.
- **`TransformVector` is an inline macro** (`r_shared.h`). As a call it was 1.2% of the frame.

**What "bit-exact with the C" means here.** vbcc rounds a float local to single precision only
where it spills it to memory, and where it spills depends on register allocation, so on the
whole function. The assembly copies the float sequence of the game's own `d_scan.o`. A test
that compiles the C in a different context, even with only the counter lines removed, gets
different rounding and reports false mismatches.

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

**Screen mode.** On the first start an ASL requester lists the 8-bit and 16-bit RTG modes from
320x200 to 1280x1024 (the engine's limits). The choice is saved in `PROGDIR:WarpQuake.mode` and used
from then on. `-asl` asks again. `-modeid` or `-width`/`-height` override it for one run,
which is handy in benchmark scripts. The engine renders at the mode's full size.

| option | meaning |
|---|---|
| `-asl` | choose the screen mode again |
| `-modeid 0x...` | use this mode |
| `-width W -height H` | use the 8-bit mode nearest WxH (headless: render at WxH) |
| `-bpp 8` / `-bpp 16` | only modes of that depth; a saved mode of the other depth stands for its size |
| `-aspect <f>` | pixel aspect (height/width). The default: modes up to 640 wide are shown 4:3, wider modes are square |
| `-mem <MB>` | hunk size, 16 by default |
| `-basedir <dir>` | where `id1/` is, if not next to the program |
| `-benchmark` | quit after the first timedemo |
| `-headless` | no screen at all; used for vamos runs |
| `+<command>` | any console command, e.g. `+timedemo demo1` |

**Mouse.** By default the mouse is read from an input handler: the raw counts, neither
accelerated nor ever dropped. Intuition's mouse-move messages, the old source, are accelerated
by the Input preferences and are held back while the window has five unread, which a fast USB
mouse can reach within one frame. `in_rawmouse 0` goes back to them. `m_filter 1` averages
each frame's movement with the last one's (WinQuake's filter). The mouse speed is then
`sensitivity` alone; both settings are saved in the config.

The console is on the key left of `1`. Esc opens the menu.

**16 bpp.** On a 16-bit RTG mode (5-6-5 or 5-5-5, either byte order) the renderer draws RGB
pixels instead of palette indexes. Lighting is computed per colour rather than rounded to the
nearest of the 256 palette entries, so it is smooth and keeps its hue; that is where 8-bit
Quake's banding comes from. Under water, slime or lava and with a powerup, the tint is built into
the colour tables. Changing one flushes the surface cache, as at a level start, so the first frame
after it is slower. The damage and bonus flashes are blended into the frame as it is shown;
`v_rgbflash 0` (saved) drops them, as ClickBOOM's 16-bit mode does. It costs about 5 ms a frame
at 320x200 over 8 bits: the surface cache, the spans and the copy to the screen move twice the
bytes. `screenshot` is 8-bit only.

**Under-water tint.** At 16 bpp water tints dark blue, slime green and lava orange-red (the tints
ClickBOOM's 16-bit mode uses); 8 bpp keeps id's brown water, tuned for the palette.
`v_watercolor` (saved) replaces the water tint with an R,G,B colour and an optional strength
(0..255, default 128), e.g. `v_watercolor 130,80,50` for id's brown at 16 bpp,
`0,30,70,80` for a lighter blue. Commas are needed on the command line, where a quoted value
would arrive as its first word only; `""` brings id's colour back.

Cvar `vid_blit` (saved in config.cfg) picks how a frame reaches the screen. `0` uses
WriteChunkyPixels. `1` locks the bitmap with p96LockBitMap and copies rows with CopyMemQuick.
`2` does the same with MOVE16 bursts: Warp's RTG memory is uncached and built for them. That
needs 16-byte aligned rows; otherwise it falls back to `1`. The profile shows what each costs.

**`membench`** (console command) measures this board's memory and writes
`RAM:WarpQuake_membench.txt`. It covers sequential reads and writes, CopyMemQuick against MOVE16
(fast RAM and VRAM), and a pointer chase at a 128-byte stride over 4 KB to 4 MB, which gives the
latency of L1, of the FPGA L2 (96 KB, 6-way, 128-byte lines) and of DDR3.

## Profiling

**Benchmark** (the number to compare with other ports):

```
WarpQuake -benchmark +timedemo demo1
```

**With a phase profile:**

```
WarpQuake -benchmark -prof +timedemo demo1
```

At the end of every timedemo the console, the Shell and `RAM:WarpQuake_prof.txt`
(`-proflog <file>` to change) get the frame time, the work counters and, with `-prof`, a
table like this:

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
says what they cost: about 10 us a read here, 0.6 ms a frame, which is why the phase timers
need `-prof` and a plain benchmark runs only the frame timer. Inside the hot loops there are only counter increments; those give the
bytes each stage moves, which is the number that decides whether an offload to the ARM or
FPGA could pay. The console commands are `prof` (report now; `prof <file>` also writes it)
and `profreset`.

**Mip bias** (trades texture sharpness for speed): `+d_mipscale 2` switches to the coarser
mip levels nearer the eye (1 is id's default; 1.5, 2, 3 are steps), `+d_mipcap 1` never uses
the full-size textures.  Fewer texels means fewer cache misses in the span loop and smaller
surface-cache blocks.  Models are not affected.

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

**Profiling another program** (for comparisons): `make attach` builds `build/WQAttach`
(`make ftp-attach` uploads it). Start it first, then the program in another Shell:

```
WQAttach CMD=quake060_cb TASKS="Quake Render Process,Quake Color Process,Quake Sound Process" SECONDS=120 TO=RAM:cb.watt
tools/amiget.py ram:cb.watt
tools/wattach.py temp/cb.watt <the program's executable> --disasm 10
```

It waits for a Shell process running CMD, records where its hunks were loaded, and samples that
process and the named tasks (a program's helper processes run its code too) at about 1 kHz until
the program exits. `wattach.py` finds the PC slot, ranks the hottest code ranges per task and
disassembles them (capstone) for reading. There are no symbols, so ranges stand in for functions.

**Checking that an optimisation draws the same pixels** (`-crc`):

```
WarpQuake -headless -basedir work: -benchmark -crc +host_framerate 0.05 +timedemo demo1
```

This prints a CRC-32 over every frame rendered. `-crcfile <f>` writes one line per frame,
which locates the first frame that differs. `-crcdump <n>` writes frame n raw (8-bit, no
palette) to `RAM:frame<n>_<w>x<h>.raw`. With a fixed `host_framerate` the timedemo renders
the same frames at any speed. `-crc` reseeds `rand()` at the timedemo (the particles use it). It also
hides the console notify lines, which expire by wall-clock time, and the console itself, which
shows the build's compile time while it slides away. A match holds only between
builds that evaluate floats the same way. An integer rewrite (a span loop in asm) must match
exactly. A different `-O` level, or an FPU-path change, legitimately moves a few pixels, so for
those compare the frame lists instead. The references at 320x200, current build, under vamos, identical for `ASM=1` and `ASM=0` and
for `r_zcover` 0 or 1:

| demo | `d_subdiv16 1` (default) | `d_subdiv16 0` (id's 8-pixel C) |
|---|---|---|
| demo1 | `c5b7b611` | `cf175444` |
| demo2 | `bdd76a83` | `7ef70975` |
| demo3 | `53966e81` | `40d6596e` |

(With the particle limit at 1536 and the world-vertex projection cache; see below.)

**16 bpp** (`-bpp 16`, headless: 5-6-5) is checked the same way with `-rgbtest`: the colour
tables then hold the 8-bit renderer's palette indexes, so the low byte of each 16-bit pixel is
the 8-bit frame, and `-crc`, which then hashes only those bytes, must print the 8-bit references
above. That checks every 16-bit drawer, asm and C, against the 8-bit renderer. Without
`-rgbtest` the `ASM=0` build differs from `ASM=1` by a pixel in some frames, because vbcc
rounds the C copy of the span drawer's floats differently (register allocation); the asm is the
8-bit code assembled again.

## Checking on the Mac (vamos)

`-headless` runs the whole engine without graphics.library. Under vamos (see the vamos note in
the csAmigaWarp notes: amitools from git, machine68k, greenlet) this checks loading, game code
and the renderer end to end. It does not measure speed.

```
vamos -C 68040 -s 600 -m 65536 -H disable -V work:<dir> -- work:WarpQuake -headless -basedir work: -benchmark +timedemo demo1
```

`-basedir work:` is needed because vamos has no PROGDIR:. The `--` is needed because otherwise
vamos takes `-basedir` as one of its own options.
