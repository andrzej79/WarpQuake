// d_rgb.c -- warpQuake: the colour tables of the 16-bit (RGB) renderer
//
// On a 16-bit screen (r_pixbytes 2) the renderer draws RGB pixels instead of
// palette indexes: the surface cache, the frame and the 2D layer all hold
// 16-bit pixels in the screen's own format, and everything that turned a
// texel into a palette index now turns it into a pixel through one of two
// tables built here:
//
// - vid.colormap16, the lighting: VID_GRADES rows of 256, row l the palette
//   at brightness 2 - 2l/63, the formula id's colormap.lmp was made with
//   (checked against it: qlumpy's "range 2", levels 64).  The 8-bit colormap
//   rounds each lit colour to the nearest of the 256 palette entries, which
//   is where software Quake's banding and hue shifts come from; here it keeps
//   the colour itself, to 5-6-5 bits.  The 32 fullbright colours (224-255)
//   are the same in every row, as in the 8-bit table.
// - d_8to16table, the palette itself, for what is not lit: sky, water,
//   sprites, particles and the 2D layer.
//
// The palette shifts (V_UpdatePalette), which an 8-bit screen gets for free
// from its CLUT, cannot be had that way, and are split:
//
// - The lasting ones - under water, slime or lava (contents) and the
//   powerups - and the gamma are built into both tables.  The surface cache
//   then holds shifted pixels, so a change flushes it and the next frame
//   rebuilds the visible surfaces, as at a level start: fine for something
//   that changes when the player dives in or picks up a quad.
// - The short ones - damage and bonus flashes, changing every frame for half
//   a second - are blended into the frame as the platform shows it
//   (QG_SetBlend), and cost only while they last.
//
// The 8-bit renderer does not use any of this.

#include "quakedef.h"
#include "d_local.h"
#include "quakegeneric.h"

int				r_pixbytes = 1;
unsigned short	d_8to16table[256];

static unsigned short	colormap16[VID_GRADES*256];
int				d_rgbgeneration;
unsigned short	d_rgbquartermask;

// -rgbtest: the tables hold the 8-bit renderer's palette indexes instead of
// colours (colormap16 = the 8-bit colormap, d_8to16table = identity), so a
// 16-bit frame's low bytes are exactly the 8-bit frame - and -crc, which
// then checks those bytes only (vid_null.c), must give the 8-bit CRCs.
// That checks every 16-bit drawer against the verified 8-bit renderer.
qboolean		d_rgbtest;
static int		pixformat = QG_PIX_CLUT8;

// what the tables were last built for (D_RGB_ShiftPalette)
static qboolean	built;
static cshift_t	builtshift[2];			// contents, powerup
static byte		builtgamma[256];
static byte		*builtpal;

// the flash blend last passed to the platform
static int		blendrgba[4];

extern byte		gammatable[256];		// view.c
extern cvar_t	v_rgbflash;

#define FIRST_FULLBRIGHT	224			// colormap.lmp: 32 "brights"


/*
================
D_RGB_Init

r_pixbytes and the pixel format, from the mode the platform opened.
================
*/
void D_RGB_Init (int format)
{
	pixformat = format;
	r_pixbytes = (format == QG_PIX_CLUT8) ? 1 : 2;
	// big-endian layouts only: a PC format's fields are split across bytes
	if (format == QG_PIX_RGB565)
		d_rgbquartermask = 0x39e7;
	else if (format == QG_PIX_RGB555)
		d_rgbquartermask = 0x1ce7;
	else
		d_rgbquartermask = 0;
	vid.colormap16 = colormap16;
	built = false;
	// a new mode: no blend, and the platform's set up for the new format
	memset (blendrgba, 0, sizeof(blendrgba));
	QG_SetBlend (0, 0, 0, 0);
	d_rgbblendchanged = true;
	d_rgbtest = (r_pixbytes == 2 && COM_CheckParm ("-rgbtest"));
	if (d_rgbtest)
		d_rgbquartermask = 0;			// Draw_FadeScreen's 8-bit pattern
}


/*
================
Pack

An RGB colour (0..255 each) as a pixel of the screen's format.  The 5- and
6-bit fields are rounded, not truncated, so white stays white.
================
*/
static unsigned short	q5[256], q6[256];	// value -> rounded field

static unsigned short Pack (int r, int g, int b)
{
	unsigned	p;
	int			t;

	if (pixformat == QG_PIX_BGR565PC || pixformat == QG_PIX_BGR555PC)
	{
		t = r;
		r = b;
		b = t;
	}
	if (pixformat == QG_PIX_RGB555 || pixformat == QG_PIX_RGB555PC || pixformat == QG_PIX_BGR555PC)
		p = (q5[r] << 10) | (q5[g] << 5) | q5[b];
	else
		p = (q5[r] << 11) | (q6[g] << 5) | q5[b];
	if (pixformat >= QG_PIX_RGB565PC)
		p = ((p & 0xFF) << 8) | (p >> 8);
	return (unsigned short)p;
}


/*
================
Shift

One colour channel through the lasting palette shifts and the gamma, in
V_UpdatePalette's arithmetic.
================
*/
static int Shift (int v, int ch, const cshift_t *shifts, int numshifts)
{
	int		j;

	for (j = 0 ; j < numshifts ; j++)
		v += (shifts[j].percent * (shifts[j].destcolor[ch] - v)) >> 8;
	return gammatable[v];
}


/*
================
D_RGB_Build

Both tables from the base palette, with the given lasting shifts.
================
*/
static void D_RGB_Build (const byte *pal, const cshift_t *shifts, int numshifts)
{
	int				l, c, ch, v;
	int				lit[3];
	unsigned		scale;
	unsigned short	*dest;
	static qboolean	quantok;

	if (d_rgbtest)
	{
		for (c = 0 ; c < 256 ; c++)
			d_8to16table[c] = c;
		for (c = 0 ; c < VID_GRADES*256 ; c++)
			colormap16[c] = vid.colormap[c];
		return;
	}

	if (!quantok)
	{
		for (v = 0 ; v < 256 ; v++)
		{
			q5[v] = (v * 31 + 127) / 255;
			q6[v] = (v * 63 + 127) / 255;
		}
		quantok = true;
	}

	for (c = 0 ; c < 256 ; c++)
		d_8to16table[c] = Pack (Shift (pal[c*3], 0, shifts, numshifts),
				Shift (pal[c*3+1], 1, shifts, numshifts),
				Shift (pal[c*3+2], 2, shifts, numshifts));

	dest = colormap16;
	for (l = 0 ; l < VID_GRADES ; l++)
	{
		// brightness 2 - 2l/63 as 16.16, applied with rounding
		scale = ((2 * (VID_GRADES - 1 - l)) << 16) / (VID_GRADES - 1);
		for (c = 0 ; c < FIRST_FULLBRIGHT ; c++)
		{
			for (ch = 0 ; ch < 3 ; ch++)
			{
				v = (pal[c*3+ch] * scale + 0x8000) >> 16;
				if (v > 255)
					v = 255;
				lit[ch] = Shift (v, ch, shifts, numshifts);
			}
			*dest++ = Pack (lit[0], lit[1], lit[2]);
		}
		for ( ; c < 256 ; c++)
			*dest++ = d_8to16table[c];
	}
}


/*
================
D_RGB_SetBlend

The flash blend, passed on only when it changes.  vid_null.c reads
d_rgbblendchanged to show the whole frame again (the status bar is not
redrawn every frame, but has to be blended or unblended with the rest).
================
*/
qboolean	d_rgbblendchanged;

static void D_RGB_SetBlend (int r, int g, int b, int alpha)
{
	if (alpha <= 0)
		r = g = b = alpha = 0;
	if (r == blendrgba[0] && g == blendrgba[1] && b == blendrgba[2] && alpha == blendrgba[3])
		return;
	blendrgba[0] = r;
	blendrgba[1] = g;
	blendrgba[2] = b;
	blendrgba[3] = alpha;
	d_rgbblendchanged = true;
	QG_SetBlend (r, g, b, alpha);
}


qboolean D_RGB_Blending (void)
{
	return blendrgba[3] != 0;
}


/*
================
D_RGB_ShiftPalette

V_UpdatePalette's work on a 16-bit screen: shifts is cl.cshifts (or NULL
for none, VID_SetPalette's case).  Rebuilds the tables if a lasting shift
or the gamma changed, and sets the flash blend from the short ones.
================
*/
void D_RGB_ShiftPalette (byte *pal, cshift_t *shifts)
{
	cshift_t	lasting[2];
	int			i, ch;

	memset (lasting, 0, sizeof(lasting));
	if (shifts)
	{
		lasting[0] = shifts[CSHIFT_CONTENTS];
		lasting[1] = shifts[CSHIFT_POWERUP];
	}
	for (i = 0 ; i < 2 ; i++)
		if (lasting[i].percent <= 0)
			memset (&lasting[i], 0, sizeof(lasting[i]));

	if (!built || pal != builtpal || memcmp (lasting, builtshift, sizeof(lasting))
			|| memcmp (gammatable, builtgamma, sizeof(builtgamma)))
	{
		D_RGB_Build (pal, lasting, 2);
		memcpy (builtshift, lasting, sizeof(lasting));
		memcpy (builtgamma, gammatable, sizeof(builtgamma));
		builtpal = pal;
		d_rgbgeneration++;
		if (built)
		{
			// the cached surfaces and the 2D layer are in the old colours
			D_FlushCaches ();
			scr_fullupdate = 0;
			Sbar_Changed ();
		}
		built = true;
	}

	// The flashes: V_UpdatePalette's two lerps, damage then bonus, folded
	// into one toward a mixed colour (a1 (1-a2) d1 + a2 d2) / A with
	// A = 1 - (1-a1)(1-a2), after the gamma rather than before it.
	if (shifts && v_rgbflash.value)
	{
		float	a1 = shifts[CSHIFT_DAMAGE].percent / 256.0f;
		float	a2 = shifts[CSHIFT_BONUS].percent / 256.0f;
		float	a, c[3];

		if (a1 < 0)
			a1 = 0;
		if (a2 < 0)
			a2 = 0;
		a = 1.0f - (1.0f - a1) * (1.0f - a2);
		if (a > 0.0f)
		{
			for (ch = 0 ; ch < 3 ; ch++)
			{
				c[ch] = (a1 * (1.0f - a2) * shifts[CSHIFT_DAMAGE].destcolor[ch]
						+ a2 * shifts[CSHIFT_BONUS].destcolor[ch]) / a;
				if (c[ch] > 255.0f)
					c[ch] = 255.0f;
				if (c[ch] < 0.0f)
					c[ch] = 0.0f;
			}
			D_RGB_SetBlend (gammatable[(int)c[0]], gammatable[(int)c[1]],
					gammatable[(int)c[2]], (int)(a * 256.0f + 0.5f));
			return;
		}
	}
	D_RGB_SetBlend (0, 0, 0, 0);
}
