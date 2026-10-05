/*
Copyright (C) 1996-1997 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/

#ifndef __QUAKEGENERIC__
#define __QUAKEGENERIC__

#include "quakekeys.h"

#define QUAKEGENERIC_RES_X 320
#define QUAKEGENERIC_RES_Y 200

#define QUAKEGENERIC_JOY_MAX_AXES 6
#define QUAKEGENERIC_JOY_AXIS_X 0
#define QUAKEGENERIC_JOY_AXIS_Y 1
#define QUAKEGENERIC_JOY_AXIS_Z 2
#define QUAKEGENERIC_JOY_AXIS_R 3
#define QUAKEGENERIC_JOY_AXIS_U 4
#define QUAKEGENERIC_JOY_AXIS_V 5


// provided functions
void QG_Tick(double duration);
void QG_Create(int argc, char *argv[]);

// user must implement these
void QG_Init(void);
void QG_Quit(void);
void QG_DrawFrame(void *pixels);
// warpQuake: only rows y .. y+rows-1 of the frame changed (the rest of the
// display already shows the rest of pixels)
void QG_DrawFrameRows(void *pixels, int y, int rows);
void QG_SetPalette(unsigned char palette[768]);
int QG_GetKey(int *down, int *key);
void QG_GetMouseMove(int *x, int *y);
// warpQuake: 1 = raw mouse counts (an input handler), 0 = the window's
// (accelerated) mouse move events
void QG_SetMouseMode(int raw);
void QG_GetJoyAxes(float *axes);

// warpQuake: the rest of the OS the engine needs, so that no AmigaOS header
// is ever included next to quakedef.h (exec's inline macros and Quake's
// identifiers do not mix).
const char *QG_BaseDir(void);           // where id1/ lives
double QG_FloatTime(void);              // seconds, monotonic
void QG_SendKeyEvents(void);            // drain the window's messages
void QG_Mkdir(const char *path);
void QG_Error(const char *msg);         // report a fatal error, then exit(1)

// v1: the display mode is chosen at run time
void QG_GetVideoSize(int *width, int *height);  // valid after QG_Init()
void QG_SetBlitMode(int mode);          // vid_blit: 0 WriteChunkyPixels, 1 lock + copy, 2 lock + MOVE16
const char *QG_VideoInfo(void);         // mode and blit path, for reports

// 16bpp: the screen's pixel format, valid after QG_Init().  CLUT8 shows
// palette indexes; the others are 16-bit RGB in the screen's own layout (PC =
// little-endian), which the renderer's colour tables (d_rgb.c) are built in.
enum
{
	QG_PIX_CLUT8,
	QG_PIX_RGB565, QG_PIX_RGB555,
	QG_PIX_RGB565PC, QG_PIX_RGB555PC,
	QG_PIX_BGR565PC, QG_PIX_BGR555PC
};
int QG_GetPixelFormat(void);
// The screen modes the engine can use (RTG, 8-bit CLUT or 16-bit RGB, within
// its size limits), for the video menu: at most max, sorted by depth, width,
// height.  0 when headless.
typedef struct
{
	unsigned long	id;
	int				width, height, bpp;
} qgmode_t;
int QG_ListModes(qgmode_t *modes, int max);
unsigned long QG_GetMode(void);         // the mode now open
// Close the display and open it in mode id, which becomes the saved mode.
// 0 if that failed (the old mode is open again) or there is no display.
int QG_SetMode(unsigned long id);
// Sound output (src/amiga_ahi.c): a ring of 16-bit stereo frames (native,
// i.e. big-endian, samples) that the platform plays in a loop.  QG_SoundInit
// returns it, its length in frames (a power of 2) and the rate it plays at,
// or NULL if there is no sound; QG_SoundPos is the frame being played now.
void *QG_SoundInit(int *frames, int *rate);
int QG_SoundPos(void);
void QG_SoundShutdown(void);
const char *QG_SoundInfo(void);         // device, mode and rate, for reports
// The Sound Options menu: the audio modes, and the mode and rate the next
// QG_SoundInit uses (saved; mode 0 is the device's default mode).
typedef struct
{
	unsigned long	id;
	char			name[48];
} qgaudiomode_t;
int QG_SoundListModes(qgaudiomode_t *modes, int max);
void QG_SoundGetSettings(unsigned long *mode, int *freq);
void QG_SoundSetSettings(unsigned long mode, int freq);
// 16bpp: blend the frame toward r,g,b (0..255) by alpha/256 as it is shown;
// alpha 0 is off.  The damage and bonus flashes, which an 8-bit screen gets
// from its palette.
void QG_SetBlend(int r, int g, int b, int alpha);

// v1: instrumentation (wq_prof.c)
unsigned long QG_Ticks(void);           // free-running counter, wraps
unsigned long QG_TickRate(void);        // its frequency in Hz
int QG_SampleStart(void);               // the sampling profiler; 0 if it cannot run
unsigned long QG_SampleStop(const char *path);  // saves; returns the sample count
const char *QG_MemBench(void);          // memory access costs, as a text report

#endif // __QUAKEGENERIC__
