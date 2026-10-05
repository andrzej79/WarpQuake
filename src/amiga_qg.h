// WarpQuake - what the AmigaOS platform files share with each other.
// Never included by the engine: quake/ sees only quakegeneric.h.
#ifndef AMIGA_QG_H
#define AMIGA_QG_H

#include <exec/types.h>
#include <intuition/intuition.h>

extern struct Window *qgWindow;   // NULL until QG_Init()

// amiga_main.c
void qgPrintf(const char *fmt, ...);

// amiga_input.c
void qgInputReset(void);
void qgInputHandlerStart(void);
void qgInputHandlerStop(void);

// move16.s: both pointers 16-byte aligned
void qgMove16Copy(const void *src, void *dst, ULONG bytes);       // bytes % 64 == 0
void qgMove16Rows(const UBYTE *src, UBYTE *dst, ULONG width, ULONG height, ULONG dstBytesPerRow);
void qgBlend16(const UWORD *src, UWORD *dst, ULONG pixels, const ULONG *params);  // blend16.s; pixels even

// The two engine entry points the platform layer calls back into (sys.h);
// declared here because quakedef.h cannot be included next to the OS headers.
void Sys_Quit(void);
void Sys_Error(char *error, ...);

#endif
