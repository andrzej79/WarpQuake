// WarpQuake - membench: what the board's memory costs the 68060, measured
// with the access patterns the renderer uses.
//
// The v2 span rewrite showed the renderer is bound by memory, not by
// instructions (Warp's CPU path to its DDR3 copies at only ~30 MB/s), so the
// next optimisations must be chosen by traffic.  This gives the per-pattern
// costs to choose with: sequential byte and longword writes (the frame and
// the z-buffer), reads, random byte reads over a surface-cache-sized area
// (texels), a copy (the blit), and writes into the screen's own memory.
//
// Memory system (Warp, 2026-10-04): 68060 L1 8 KB data + 8 KB instruction,
// 16-byte lines; FPGA L2 96 KB, 6-way, 128-byte lines, in front of the DDR3
// fast RAM; RTG memory uncached, built for MOVE16 bursts.

#ifndef __VBCC__
#define __reg(x)
#define __saveds
#endif

#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/execbase.h>
#include <proto/exec.h>

#include <libraries/Picasso96.h>
#pragma stdargs-on
#include <clib/Picasso96_protos.h>
#pragma stdargs-off
#include <inline/Picasso96_protos.h>

#include "quakegeneric.h"
#include "amiga_qg.h"

extern struct Library *P96Base;

#define SMALL (4 * 1024)          // fits the 8 KB data cache
#define FRAME (64 * 1024)         // a 320x200 frame
#define LARGE (1024 * 1024)       // well past both caches
#define CHASE (4 * 1024 * 1024)   // the largest pointer-chase area
#define CHASE_STRIDE 128          // one L2 line per node
#define CHASE_LOADS 20000
#define RUNS 5                    // best of

static char report[3072];
static int reportLen;

static void out(const char *fmt, ...)
{
  va_list ap;

  va_start(ap, fmt);
  reportLen += vsnprintf(report + reportLen, sizeof(report) - reportLen, fmt, ap);
  va_end(ap);
}

// Plain C loops: vbcc at -O1 keeps them as simple load/store loops.  Read
// results go to a volatile sink so they are not optimised away.

static void writeBytes(UBYTE *p, ULONG n)
{
  UBYTE v = 0;

  while(n--) {
    *p++ = v++;
  }
}

static void writeLongs(ULONG *p, ULONG n)
{
  ULONG v = 0;

  n /= 4;
  while(n--) {
    *p++ = v++;
  }
}

static ULONG readLongs(const ULONG *p, ULONG n)
{
  ULONG sum = 0;

  n /= 4;
  while(n--) {
    sum += *p++;
  }
  return sum;
}

// The pointer chase: CHASE_LOADS dependent loads, each the address of the
// next.  Nothing overlaps them, so the time per load is the latency of
// wherever the line comes from.
static void *chase(void *p, ULONG n)
{
  while(n--) {
    p = *(void **)p;
  }
  return p;
}

// Links the nodes CHASE_STRIDE apart in [buf, buf + size) into one cycle in a
// pseudo-random order, so no stride prefetch or page locality helps.
static void *buildChase(UBYTE *buf, ULONG size)
{
  static ULONG order[CHASE / CHASE_STRIDE];
  ULONG nodes = size / CHASE_STRIDE, i, x = 12345;

  for(i = 0; i < nodes; i++) {
    order[i] = i;
  }
  for(i = nodes - 1; i > 0; i--) {   // Fisher-Yates
    ULONG j, t;

    x = x * 1103515245UL + 12345UL;
    j = (x >> 8) % (i + 1);
    t = order[i];
    order[i] = order[j];
    order[j] = t;
  }
  for(i = 0; i < nodes; i++) {
    *(void **)(buf + order[i] * CHASE_STRIDE) = buf + order[(i + 1) % nodes] * CHASE_STRIDE;
  }
  return buf + order[0] * CHASE_STRIDE;
}

static ULONG volatile sink;

typedef ULONG (*bench_fn)(void *a, void *b, ULONG n);

static ULONG bWriteBytes(void *a, void *b, ULONG n)
{
  writeBytes(a, n);
  return 0;
}

static ULONG bWriteLongs(void *a, void *b, ULONG n)
{
  writeLongs(a, n);
  return 0;
}

static ULONG bReadLongs(void *a, void *b, ULONG n)
{
  return readLongs(a, n);
}

static ULONG bCopy(void *a, void *b, ULONG n)
{
  CopyMemQuick(a, b, n);
  return 0;
}

static ULONG bMove16(void *a, void *b, ULONG n)
{
  qgMove16Copy(a, b, n);
  return 0;
}

// Best-of-RUNS time of fn, in microseconds.  The caches are flushed first so
// every run starts the same way (dirty lines written back, nothing valid).
static double timeIt(bench_fn fn, void *a, void *b, ULONG n)
{
  double best = 1e30, rate = (double)QG_TickRate();
  int i;

  for(i = 0; i < RUNS; i++) {
    unsigned long t0, t1;
    double us;

    CacheClearU();
    t0 = QG_Ticks();
    sink += fn(a, b, n);
    t1 = QG_Ticks();
    us = (double)(t1 - t0) * 1e6 / rate;
    if(us < best) {
      best = us;
    }
  }
  return best;
}

static void line(const char *name, double us, ULONG bytes)
{
  out("%-34s %8.0f us  %7.1f MB/s\n", name, us, bytes / us);
}

static void chaseLine(UBYTE *buf, ULONG size)
{
  double best = 1e30, rate = (double)QG_TickRate();
  void *start = buildChase(buf, size);
  char name[40];
  int i;

  for(i = 0; i < RUNS; i++) {
    unsigned long t0, t1;
    double us;

    CacheClearU();
    chase(start, size / CHASE_STRIDE);     // warm whatever can hold it
    t0 = QG_Ticks();
    sink += (ULONG)chase(start, CHASE_LOADS);
    t1 = QG_Ticks();
    us = (double)(t1 - t0) * 1e6 / rate;
    if(us < best) {
      best = us;
    }
  }
  sprintf(name, "chase %4lu KB, %d B stride", size / 1024, CHASE_STRIDE);
  out("%-34s %8.1f ns/load\n", name, best * 1000.0 / CHASE_LOADS);
}

static UBYTE *align16(UBYTE *p)
{
  return (UBYTE *)(((ULONG)p + 15) & ~15UL);
}

const char *QG_MemBench(void)
{
  UBYTE *largeMem, *frameMem, *large, *frame;
  ULONG size;

  reportLen = 0;
  report[0] = 0;

  // 16-byte aligned for MOVE16; the large one also serves the chase
  largeMem = AllocVec(CHASE + 16, MEMF_FAST | MEMF_CLEAR);
  frameMem = AllocVec(FRAME + 16, MEMF_FAST | MEMF_CLEAR);
  large = align16(largeMem);
  frame = align16(frameMem);
  if(largeMem == NULL || frameMem == NULL) {
    out("membench: not enough fast RAM\n");
    goto done;
  }

  out("---- membench (best of %d, caches flushed before each run) ----\n", RUNS);
  out("-- the byte and in-cache rows are bound by the C loop, not by memory\n");
  line("write bytes, 4 KB (in cache)", timeIt(bWriteBytes, frame, NULL, SMALL), SMALL);
  line("write bytes, 64 KB (the frame)", timeIt(bWriteBytes, frame, NULL, FRAME), FRAME);
  line("write longs, 64 KB", timeIt(bWriteLongs, frame, NULL, FRAME), FRAME);
  line("write longs, 1 MB", timeIt(bWriteLongs, large, NULL, LARGE), LARGE);
  line("read longs, 4 KB (in cache)", timeIt(bReadLongs, frame, NULL, SMALL), SMALL);
  line("read longs, 64 KB", timeIt(bReadLongs, frame, NULL, FRAME), FRAME);
  line("read longs, 1 MB", timeIt(bReadLongs, large, NULL, LARGE), LARGE);
  line("copy 64 KB (CopyMemQuick)", timeIt(bCopy, large, frame, FRAME), FRAME);
  line("copy 64 KB (MOVE16)", timeIt(bMove16, large, frame, FRAME), FRAME);
  out("-- latency: dependent loads, one per 128-byte line, random order\n");
  for(size = 4 * 1024; size <= CHASE; size *= 2) {
    chaseLine(large, size);
  }

  // The screen's memory: what rendering straight into VRAM would cost.
  if(qgWindow != NULL && P96Base != NULL) {
    struct BitMap *bm = qgWindow->RPort->BitMap;
    struct RenderInfo ri;
    LONG lock = p96LockBitMap(bm, (UBYTE *)&ri, sizeof(ri));

    if(lock != 0) {
      ULONG bytes = (ULONG)ri.BytesPerRow * 150;   // stay inside a 320x200 screen

      if(bytes > FRAME) {
        bytes = FRAME;
      }
      line("VRAM write bytes", timeIt(bWriteBytes, ri.Memory, NULL, bytes), bytes);
      line("VRAM write longs", timeIt(bWriteLongs, ri.Memory, NULL, bytes), bytes);
      line("VRAM read longs", timeIt(bReadLongs, ri.Memory, NULL, bytes), bytes);
      line("copy fast RAM -> VRAM", timeIt(bCopy, frame, ri.Memory, bytes), bytes);
      if((((ULONG)ri.Memory) & 15) == 0) {
        line("copy fast RAM -> VRAM (MOVE16)", timeIt(bMove16, frame, ri.Memory, bytes & ~63UL), bytes & ~63UL);
      } else {
        out("copy fast RAM -> VRAM (MOVE16): screen memory not 16-byte aligned\n");
      }
      p96UnlockBitMap(bm, lock);
    }
  }

done:
  if(largeMem != NULL) {
    FreeVec(largeMem);
  }
  if(frameMem != NULL) {
    FreeVec(frameMem);
  }
  return report;
}
