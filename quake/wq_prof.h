// wq_prof.h -- warpQuake instrumentation: per-frame phase timers and
// per-stage work counters.
//
// The timers read the platform's tick counter (QG_Ticks) at phase boundaries
// only - a few dozen reads a frame.  Inside the hot loops there are only
// counter increments.  A read is not free (an E-clock read is ~10 us on a
// 68060 Amiga, 0.6 ms a frame for all phases), so the phase timers run only
// with -prof; the frame timer always runs, for the report's ms/frame.  Where a hot function's time is wanted, the sampling
// profiler (profsample) answers that without disturbing it.

#ifndef WQ_PROF_H
#define WQ_PROF_H

#include "quakegeneric.h"

typedef enum
{
	WQP_FRAME,          // one _Host_Frame that ran (not the filtered-out ones)
	WQP_INPUT,          // key events, IN_Commands, console commands
	WQP_SERVER,         // Host_ServerFrame
	WQP_QC,             // PR_ExecuteProgram, outermost calls only
	WQP_CLIENT,         // CL_ReadFromServer
	WQP_SCREEN,         // SCR_UpdateScreen, all of it
	WQP_RENDER,         // R_RenderView
	WQP_R_SETUP,        // R_SetupFrame + R_MarkLeaves
	WQP_R_WORLD,        // R_RenderWorld: BSP walk, clipping, edge emission
	WQP_R_BMODELS,      // R_DrawBEntitiesOnList: doors, lifts, ...
	WQP_R_SCAN,         // R_ScanEdges: edge sort, span generation, and D_DrawSurfaces
	WQP_D_SURFS,        // D_DrawSurfaces: texture mapping, z, sky, water
	WQP_D_CACHE,        // building surface-cache blocks (cache misses only)
	WQP_R_ENTS,         // R_DrawEntitiesOnList: alias models, sprites
	WQP_R_VIEWMODEL,    // the weapon
	WQP_R_PARTICLES,
	WQP_R_WARP,         // D_WarpScreen, underwater
	WQP_BLIT,           // VID_Update: frame to the display
	WQP_SOUND,          // S_Update
	WQP_CRC,            // -crc checksumming: ours, not the game's
	WQP_COUNT
} wqp_phase_t;

typedef enum
{
	WQC_SPAN_PIXELS,    // D_DrawSpans8: textured world pixels written
	WQC_TURB_PIXELS,    // D_DrawTurbulent8Span: water/slime/lava pixels
	WQC_ZSPAN_PIXELS,   // D_DrawZSpans: z-buffer words written
	WQC_POLY_PIXELS,    // D_PolysetDrawSpans8: alias model pixels considered
	WQC_CACHE_BUILDS,   // surface-cache blocks built
	WQC_CACHE_BYTES,    // ... and their size
	WQC_BLIT_BYTES,     // bytes handed to the display
	WQC_SPANS,          // D_DrawSpans8 spans: asm build only (d_spans060.s knows this index)
	WQC_PARTICLES,      // active particles
	WQC_COUNT
} wqp_counter_t;

extern unsigned long	wqp_ticks[WQP_COUNT];
extern unsigned long	wqp_calls[WQP_COUNT];
extern unsigned long	wqc_count[WQC_COUNT];
extern int				wqp_qcdepth;
extern unsigned long	wqc_particlepeak;	// most active particles in one frame

extern qboolean			wqp_phases;	// -prof: the phase timers run

#define WQP_BEGIN(p)	unsigned long wqp_t0_##p = wqp_phases ? QG_Ticks () : 0
#define WQP_END(p)		((wqp_phases ? (wqp_ticks[p] += QG_Ticks () - wqp_t0_##p) : 0), wqp_calls[p]++)
// the frame timer: always on
#define WQP_BEGIN_FRAME	unsigned long wqp_t0_frame = QG_Ticks ()
#define WQP_END_FRAME	(wqp_ticks[WQP_FRAME] += QG_Ticks () - wqp_t0_frame, wqp_calls[WQP_FRAME]++)
#define WQC_ADD(c, n)	(wqc_count[c] += (unsigned long)(n))

extern qboolean			wqp_crcon;  // -crc

void WQP_Init (void);           // registers the console commands
void WQP_FrameCRC (const unsigned char *buf, int width, int height, int rowbytes);
void WQP_FrameStart (void);     // runs a reset requested mid-frame
void WQP_TimedemoCommand (void); // "timedemo" typed: before the demo loads
void WQP_TimedemoStart (void);  // the timedemo's measured part begins
void WQP_TimedemoEnd (void);    // ... and ends: report, and save a sample run

#endif
