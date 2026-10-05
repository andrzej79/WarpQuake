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

// draw.c -- this is the only file outside the refresh that touches the
// vid buffer

#include "quakedef.h"

typedef struct {
	vrect_t	rect;
	int		width;
	int		height;
	byte	*ptexbytes;
	int		rowbytes;
} rectdesc_t;

static rectdesc_t	r_rectdesc;

byte		*draw_chars;				// 8*8 graphic characters
qpic_t		*draw_disc;
qpic_t		*draw_backtile;

/*
 * warpQuake 16bpp: WAD pictures (the status bar's, the backtile) as 16-bit
 * pixels, converted once and kept until the colour tables change
 * (d_rgbgeneration), so that drawing one is a copy instead of a palette
 * lookup a pixel (~0.9 ms a frame on the 060).  Only WAD lumps: they stay
 * where they were loaded, so the address names the picture, while a
 * cache_user picture (Draw_CachePic) can be freed and its memory reused.
 * NULL for anything else: the caller converts as it draws.
 */
#define PIX16_SLOTS		256			// a power of 2; the WAD has ~150 pictures

typedef struct
{
	byte			*data;
	int				size;
	int				gen;
	unsigned short	*pixels;
} pix16_t;

static pix16_t	pix16[PIX16_SLOTS];

static unsigned short *Draw_Pixels16 (byte *data, int size)
{
	unsigned	h;
	int			i;
	pix16_t		*p;

	if (data < wad_base || data >= (byte *)wad_lumps)
		return NULL;
	h = ((uintptr_t)data >> 3) & (PIX16_SLOTS-1);
	for (i = 0 ; i < PIX16_SLOTS ; i++, h = (h + 1) & (PIX16_SLOTS-1))
	{
		p = &pix16[h];
		if (p->data == data && p->size == size)
			break;
		if (!p->data)
		{
			p->pixels = malloc (size * sizeof(unsigned short));
			if (!p->pixels)
				return NULL;
			p->data = data;
			p->size = size;
			p->gen = d_rgbgeneration - 1;
			break;
		}
	}
	if (i == PIX16_SLOTS)
		return NULL;
	if (p->gen != d_rgbgeneration)
	{
		for (i = 0 ; i < size ; i++)
			p->pixels[i] = d_8to16table[data[i]];
		p->gen = d_rgbgeneration;
	}
	return p->pixels;
}

// n 16-bit pixels, two at a time (the 68060 does misaligned longwords)
static void Draw_CopyPixels16 (unsigned short *dest, const unsigned short *src, int n)
{
	unsigned long		*d = (unsigned long *)dest;
	const unsigned long	*s = (const unsigned long *)src;

	for ( ; n >= 2 ; n -= 2)
		*d++ = *s++;
	if (n)
		*(unsigned short *)d = *(const unsigned short *)s;
}

//=============================================================================
/* Support Routines */

typedef struct cachepic_s
{
	char		name[MAX_QPATH];
	cache_user_t	cache;
} cachepic_t;

#define	MAX_CACHED_PICS		128
cachepic_t	menu_cachepics[MAX_CACHED_PICS];
int			menu_numcachepics;


qpic_t	*Draw_PicFromWad (char *name)
{
	return W_GetLumpName (name);
}

/*
================
Draw_CachePic
================
*/
qpic_t	*Draw_CachePic (char *path)
{
	cachepic_t	*pic;
	int			i;
	qpic_t		*dat;
	
	for (pic=menu_cachepics, i=0 ; i<menu_numcachepics ; pic++, i++)
		if (!strcmp (path, pic->name))
			break;

	if (i == menu_numcachepics)
	{
		if (menu_numcachepics == MAX_CACHED_PICS)
			Sys_Error ("menu_numcachepics == MAX_CACHED_PICS");
		menu_numcachepics++;
		strcpy (pic->name, path);
	}

	dat = Cache_Check (&pic->cache);

	if (dat)
		return dat;

//
// load the pic from disk
//
	COM_LoadCacheFile (path, &pic->cache);
	
	dat = (qpic_t *)pic->cache.data;
	if (!dat)
	{
		Sys_Error ("Draw_CachePic: failed to load %s", path);
	}

	SwapPic (dat);

	return dat;
}



/*
===============
Draw_Init
===============
*/
void Draw_Init (void)
{
	int		i;

	draw_chars = W_GetLumpName ("conchars");
	draw_disc = W_GetLumpName ("disc");
	draw_backtile = W_GetLumpName ("backtile");

	r_rectdesc.width = draw_backtile->width;
	r_rectdesc.height = draw_backtile->height;
	r_rectdesc.ptexbytes = draw_backtile->data;
	r_rectdesc.rowbytes = draw_backtile->width;
}



/*
================
Draw_Character

Draws one 8*8 graphics character with 0 being transparent.
It can be clipped to the top of the screen to allow the console to be
smoothly scrolled off.
================
*/
void Draw_Character (int x, int y, int num)
{
	byte			*dest;
	byte			*source;
	unsigned short	*pusdest;
	int				drawline;	
	int				row, col;

	num &= 255;
	
	if (y <= -8)
		return;			// totally off screen

#ifdef PARANOID
	if (y > vid.height - 8 || x < 0 || x > vid.width - 8)
		Sys_Error ("Con_DrawCharacter: (%i, %i)", x, y);
	if (num < 0 || num > 255)
		Sys_Error ("Con_DrawCharacter: char %i", num);
#endif

	row = num>>4;
	col = num&15;
	source = draw_chars + (row<<10) + (col<<3);

	if (y < 0)
	{	// clipped
		drawline = 8 + y;
		source -= 128*y;
		y = 0;
	}
	else
		drawline = 8;

	// warpQuake 16bpp: through the palette, transparent (0) skipped
	if (r_pixbytes == 2)
	{
		unsigned short	*d16 = (unsigned short *)vid.conbuffer + y*vid.conrowbytes + x;

		while (drawline--)
		{
			for (col = 0 ; col < 8 ; col++)
				if (source[col])
					d16[col] = d_8to16table[source[col]];
			source += 128;
			d16 += vid.conrowbytes;
		}
		return;
	}

	dest = vid.conbuffer + y*vid.conrowbytes + x;

	// warpQuake: a character row is 8 bytes; its transparent ones (0) are
	// masked out with two longwords instead of tested one by one.  The
	// masks come from draw_chars once (RAM is plentiful: 16 KB).  The 68060
	// does not mind misaligned longwords.
	{
		static unsigned long	charmask[128*128/4];	// draw_chars' layout
		static qboolean			charmaskok;
		const unsigned long		*m;

		if (!charmaskok)
		{
			int		i;

			for (i = 0 ; i < 128*128 ; i++)
				((byte *)charmask)[i] = draw_chars[i] ? 0xFF : 0;
			charmaskok = true;
		}
		m = (const unsigned long *)((byte *)charmask + (source - draw_chars));
		while (drawline--)
		{
			unsigned long	*d = (unsigned long *)dest;
			const unsigned long	*src = (const unsigned long *)source;

			d[0] = (d[0] & ~m[0]) | (src[0] & m[0]);
			d[1] = (d[1] & ~m[1]) | (src[1] & m[1]);
			source += 128;
			m += 128/4;
			dest += vid.conrowbytes;
		}
	}
}

/*
================
Draw_String
================
*/
void Draw_String (int x, int y, char *str)
{
	while (*str)
	{
		Draw_Character (x, y, *str);
		str++;
		x += 8;
	}
}

/*
================
Draw_DebugChar

Draws a single character directly to the upper right corner of the screen.
This is for debugging lockups by drawing different chars in different parts
of the code.
================
*/
void Draw_DebugChar (char num)
{
	byte			*dest;
	byte			*source;
	int				drawline;	
	extern byte		*draw_chars;
	int				row, col;

	if (!vid.direct)
		return;		// don't have direct FB access, so no debugchars...

	drawline = 8;

	row = num>>4;
	col = num&15;
	source = draw_chars + (row<<10) + (col<<3);

	dest = vid.direct + 312;

	while (drawline--)
	{
		dest[0] = source[0];
		dest[1] = source[1];
		dest[2] = source[2];
		dest[3] = source[3];
		dest[4] = source[4];
		dest[5] = source[5];
		dest[6] = source[6];
		dest[7] = source[7];
		source += 128;
		dest += 320;
	}
}

/*
=============
Draw_Pic
=============
*/
void Draw_Pic (int x, int y, qpic_t *pic)
{
	byte			*dest, *source;
	unsigned short	*pusdest;
	int				v, u;

	if ((x < 0) ||
		(x + pic->width > vid.width) ||
		(y < 0) ||
		(y + pic->height > vid.height))
	{
		Sys_Error ("Draw_Pic: bad coordinates");
	}

	source = pic->data;

	// warpQuake 16bpp
	if (r_pixbytes == 2)
	{
		unsigned short	*px = Draw_Pixels16 (source, pic->width * pic->height);

		pusdest = (unsigned short *)vid.buffer + y * vid.rowbytes + x;
		for (v=0 ; v<pic->height ; v++)
		{
			if (px)
			{
				Draw_CopyPixels16 (pusdest, px, pic->width);
				px += pic->width;
			}
			else
			{
				for (u=0 ; u<pic->width ; u++)
					pusdest[u] = d_8to16table[source[u]];
			}
			pusdest += vid.rowbytes;
			source += pic->width;
		}
		return;
	}

	dest = vid.buffer + y * vid.rowbytes + x;

	// warpQuake: longwords whatever the alignment (the 68060 does misaligned
	// accesses in hardware); Q_memcpy went byte by byte unless both ends were
	// aligned, and the status bar pictures (redrawn every frame while an item
	// icon flashes) never are.  The same bytes.
	for (v=0 ; v<pic->height ; v++)
	{
		unsigned long	*d = (unsigned long *)dest;
		unsigned long	*sl = (unsigned long *)source;
		int				n;

		for (n = pic->width >> 2 ; n > 0 ; n--)
			*d++ = *sl++;
		for (n = 0 ; n < (pic->width & 3) ; n++)
			((byte *)d)[n] = ((byte *)sl)[n];
		dest += vid.rowbytes;
		source += pic->width;
	}
}


/*
=============
Draw_TransPic
=============
*/
void Draw_TransPic (int x, int y, qpic_t *pic)
{
	byte	*dest, *source, tbyte;
	unsigned short	*pusdest;
	int				v, u;

	if (x < 0 || (unsigned)(x + pic->width) > vid.width || y < 0 ||
		 (unsigned)(y + pic->height) > vid.height)
	{
		Sys_Error ("Draw_TransPic: bad coordinates");
	}
		
	source = pic->data;

	// warpQuake 16bpp
	if (r_pixbytes == 2)
	{
		pusdest = (unsigned short *)vid.buffer + y * vid.rowbytes + x;
		for (v=0 ; v<pic->height ; v++)
		{
			for (u=0 ; u<pic->width ; u++)
				if ( (tbyte=source[u]) != TRANSPARENT_COLOR)
					pusdest[u] = d_8to16table[tbyte];
			pusdest += vid.rowbytes;
			source += pic->width;
		}
		return;
	}

	dest = vid.buffer + y * vid.rowbytes + x;

	if (pic->width & 7)
	{	// general
		for (v=0 ; v<pic->height ; v++)
		{
			for (u=0 ; u<pic->width ; u++)
				if ( (tbyte=source[u]) != TRANSPARENT_COLOR)
					dest[u] = tbyte;

			dest += vid.rowbytes;
			source += pic->width;
		}
	}
	else
	{	// unwound
		for (v=0 ; v<pic->height ; v++)
		{
			for (u=0 ; u<pic->width ; u+=8)
			{
				if ( (tbyte=source[u]) != TRANSPARENT_COLOR)
					dest[u] = tbyte;
				if ( (tbyte=source[u+1]) != TRANSPARENT_COLOR)
					dest[u+1] = tbyte;
				if ( (tbyte=source[u+2]) != TRANSPARENT_COLOR)
					dest[u+2] = tbyte;
				if ( (tbyte=source[u+3]) != TRANSPARENT_COLOR)
					dest[u+3] = tbyte;
				if ( (tbyte=source[u+4]) != TRANSPARENT_COLOR)
					dest[u+4] = tbyte;
				if ( (tbyte=source[u+5]) != TRANSPARENT_COLOR)
					dest[u+5] = tbyte;
				if ( (tbyte=source[u+6]) != TRANSPARENT_COLOR)
					dest[u+6] = tbyte;
				if ( (tbyte=source[u+7]) != TRANSPARENT_COLOR)
					dest[u+7] = tbyte;
			}
			dest += vid.rowbytes;
			source += pic->width;
		}
	}

}


/*
=============
Draw_TransPicTranslate
=============
*/
void Draw_TransPicTranslate (int x, int y, qpic_t *pic, byte *translation)
{
	byte	*dest, *source, tbyte;
	unsigned short	*pusdest;
	int				v, u;

	if (x < 0 || (unsigned)(x + pic->width) > vid.width || y < 0 ||
		 (unsigned)(y + pic->height) > vid.height)
	{
		Sys_Error ("Draw_TransPic: bad coordinates");
	}
		
	source = pic->data;

	// warpQuake 16bpp
	if (r_pixbytes == 2)
	{
		pusdest = (unsigned short *)vid.buffer + y * vid.rowbytes + x;
		for (v=0 ; v<pic->height ; v++)
		{
			for (u=0 ; u<pic->width ; u++)
				if ( (tbyte=source[u]) != TRANSPARENT_COLOR)
					pusdest[u] = d_8to16table[translation[tbyte]];
			pusdest += vid.rowbytes;
			source += pic->width;
		}
		return;
	}

	dest = vid.buffer + y * vid.rowbytes + x;

	if (pic->width & 7)
	{	// general
		for (v=0 ; v<pic->height ; v++)
		{
			for (u=0 ; u<pic->width ; u++)
				if ( (tbyte=source[u]) != TRANSPARENT_COLOR)
					dest[u] = translation[tbyte];

			dest += vid.rowbytes;
			source += pic->width;
		}
	}
	else
	{	// unwound
		for (v=0 ; v<pic->height ; v++)
		{
			for (u=0 ; u<pic->width ; u+=8)
			{
				if ( (tbyte=source[u]) != TRANSPARENT_COLOR)
					dest[u] = translation[tbyte];
				if ( (tbyte=source[u+1]) != TRANSPARENT_COLOR)
					dest[u+1] = translation[tbyte];
				if ( (tbyte=source[u+2]) != TRANSPARENT_COLOR)
					dest[u+2] = translation[tbyte];
				if ( (tbyte=source[u+3]) != TRANSPARENT_COLOR)
					dest[u+3] = translation[tbyte];
				if ( (tbyte=source[u+4]) != TRANSPARENT_COLOR)
					dest[u+4] = translation[tbyte];
				if ( (tbyte=source[u+5]) != TRANSPARENT_COLOR)
					dest[u+5] = translation[tbyte];
				if ( (tbyte=source[u+6]) != TRANSPARENT_COLOR)
					dest[u+6] = translation[tbyte];
				if ( (tbyte=source[u+7]) != TRANSPARENT_COLOR)
					dest[u+7] = translation[tbyte];
			}
			dest += vid.rowbytes;
			source += pic->width;
		}
	}
}


void Draw_CharToConback (int num, byte *dest)
{
	int		row, col;
	byte	*source;
	int		drawline;
	int		x;

	row = num>>4;
	col = num&15;
	source = draw_chars + (row<<10) + (col<<3);

	drawline = 8;

	while (drawline--)
	{
		for (x=0 ; x<8 ; x++)
			if (source[x])
				dest[x] = 0x60 + source[x];
		source += 128;
		dest += 320;
	}

}

/*
================
Draw_ConsoleBackground

================
*/
void Draw_ConsoleBackground (int lines)
{
	int				x, y, v;
	byte			*src, *dest;
	unsigned short	*pusdest;
	int				f, fstep;
	qpic_t			*conback;
	char			ver[100];

	conback = Draw_CachePic ("gfx/conback.lmp");

// hack the version number directly into the pic
	dest = conback->data + 320 - 43 + 320*186;
	sprintf (ver, "%4.2f", VERSION);

	for (x=0 ; x<strlen(ver) ; x++)
		Draw_CharToConback (ver[x], dest+(x<<3));
	
// draw the pic
	// warpQuake 16bpp
	if (r_pixbytes == 2)
	{
		pusdest = (unsigned short *)vid.conbuffer;
		fstep = 320*0x10000/vid.conwidth;
		for (y=0 ; y<lines ; y++, pusdest += vid.conrowbytes)
		{
			v = (vid.conheight - lines + y)*200/vid.conheight;
			src = conback->data + v*320;
			f = 0;
			for (x=0 ; x<vid.conwidth ; x++, f += fstep)
				pusdest[x] = d_8to16table[src[f>>16]];
		}
		return;
	}

	dest = vid.conbuffer;

	for (y=0 ; y<lines ; y++, dest += vid.conrowbytes)
	{
		v = (vid.conheight - lines + y)*200/vid.conheight;
		src = conback->data + v*320;
		if (vid.conwidth == 320)
			memcpy (dest, src, vid.conwidth);
		else
		{
			f = 0;
			fstep = 320*0x10000/vid.conwidth;
			for (x=0 ; x<vid.conwidth ; x+=4)
			{
				dest[x] = src[f>>16];
				f += fstep;
				dest[x+1] = src[f>>16];
				f += fstep;
				dest[x+2] = src[f>>16];
				f += fstep;
				dest[x+3] = src[f>>16];
				f += fstep;
			}
		}
	}
}


/*
==============
R_DrawRect
==============
*/
void R_DrawRect (vrect_t *prect, int rowbytes, byte *psrc,
	int transparent)
{
	byte	t;
	int		i, j, srcdelta, destdelta;
	byte	*pdest;

	// warpQuake 16bpp: the backtile from its converted copy
	if (r_pixbytes == 2)
	{
		unsigned short	*pd = (unsigned short *)vid.buffer + (prect->y * vid.rowbytes) + prect->x;
		unsigned short	*px = NULL;

		if (!transparent && psrc >= r_rectdesc.ptexbytes)
			px = Draw_Pixels16 (r_rectdesc.ptexbytes, r_rectdesc.rowbytes * r_rectdesc.height);
		if (px)
		{
			px += psrc - r_rectdesc.ptexbytes;
			for (i=0 ; i<prect->height ; i++, px += rowbytes, pd += vid.rowbytes)
				Draw_CopyPixels16 (pd, px, prect->width);
			return;
		}
		for (i=0 ; i<prect->height ; i++, psrc += rowbytes, pd += vid.rowbytes)
			for (j=0 ; j<prect->width ; j++)
				if (!transparent || psrc[j] != TRANSPARENT_COLOR)
					pd[j] = d_8to16table[psrc[j]];
		return;
	}

	pdest = vid.buffer + (prect->y * vid.rowbytes) + prect->x;

	srcdelta = rowbytes - prect->width;
	destdelta = vid.rowbytes - prect->width;

	if (transparent)
	{
		for (i=0 ; i<prect->height ; i++)
		{
			for (j=0 ; j<prect->width ; j++)
			{
				t = *psrc;
				if (t != TRANSPARENT_COLOR)
				{
					*pdest = t;
				}

				psrc++;
				pdest++;
			}

			psrc += srcdelta;
			pdest += destdelta;
		}
	}
	else
	{
		for (i=0 ; i<prect->height ; i++)
		{
			memcpy (pdest, psrc, prect->width);
			psrc += rowbytes;
			pdest += vid.rowbytes;
		}
	}
}

/*
=============
Draw_TileClear

This repeats a 64*64 tile graphic to fill the screen around a sized down
refresh window.
=============
*/
void Draw_TileClear (int x, int y, int w, int h)
{
	int				width, height, tileoffsetx, tileoffsety;
	byte			*psrc;
	vrect_t			vr;

	r_rectdesc.rect.x = x;
	r_rectdesc.rect.y = y;
	r_rectdesc.rect.width = w;
	r_rectdesc.rect.height = h;

	vr.y = r_rectdesc.rect.y;
	height = r_rectdesc.rect.height;

	tileoffsety = vr.y % r_rectdesc.height;

	while (height > 0)
	{
		vr.x = r_rectdesc.rect.x;
		width = r_rectdesc.rect.width;

		if (tileoffsety != 0)
			vr.height = r_rectdesc.height - tileoffsety;
		else
			vr.height = r_rectdesc.height;

		if (vr.height > height)
			vr.height = height;

		tileoffsetx = vr.x % r_rectdesc.width;

		while (width > 0)
		{
			if (tileoffsetx != 0)
				vr.width = r_rectdesc.width - tileoffsetx;
			else
				vr.width = r_rectdesc.width;

			if (vr.width > width)
				vr.width = width;

			psrc = r_rectdesc.ptexbytes +
					(tileoffsety * r_rectdesc.rowbytes) + tileoffsetx;

			R_DrawRect (&vr, r_rectdesc.rowbytes, psrc, 0);

			vr.x += vr.width;
			width -= vr.width;
			tileoffsetx = 0;	// only the left tile can be left-clipped
		}

		vr.y += vr.height;
		height -= vr.height;
		tileoffsety = 0;		// only the top tile can be top-clipped
	}
}


/*
=============
Draw_Fill

Fills a box of pixels with a single color
=============
*/
void Draw_Fill (int x, int y, int w, int h, int c)
{
	byte			*dest;
	unsigned short	*pusdest;
	unsigned		uc;
	int				u, v;

	// warpQuake 16bpp
	if (r_pixbytes == 2)
	{
		uc = d_8to16table[c & 255];
		pusdest = (unsigned short *)vid.buffer + y*vid.rowbytes + x;
		for (v=0 ; v<h ; v++, pusdest += vid.rowbytes)
			for (u=0 ; u<w ; u++)
				pusdest[u] = uc;
		return;
	}

	dest = vid.buffer + y*vid.rowbytes + x;
	for (v=0 ; v<h ; v++, dest += vid.rowbytes)
		for (u=0 ; u<w ; u++)
			dest[u] = c;
}
//=============================================================================

/*
================
Draw_FadeScreen

================
*/
void Draw_FadeScreen (void)
{
	int			x,y;
	byte		*pbuf;

	VID_UnlockBuffer ();
	S_ExtraUpdate ();
	VID_LockBuffer ();

	// warpQuake 16bpp: a quarter of the brightness, which is what the 8-bit
	// pattern (three pixels in four black) averages to - smoothly, where
	// the format allows a shift (d_rgbquartermask), else the same pattern
	if (r_pixbytes == 2)
	{
		for (y=0 ; y<vid.height ; y++)
		{
			unsigned short	*p16 = (unsigned short *)vid.buffer + vid.rowbytes*y;
			int				t = (y & 1) << 1;

			for (x=0 ; x<vid.width ; x++)
			{
				if (d_rgbquartermask)
					p16[x] = (p16[x] >> 2) & d_rgbquartermask;
				else if ((x & 3) != t)
					p16[x] = d_8to16table[0];
			}
		}
		VID_UnlockBuffer ();
		S_ExtraUpdate ();
		VID_LockBuffer ();
		return;
	}

	for (y=0 ; y<vid.height ; y++)
	{
		int	t;

		pbuf = (byte *)(vid.buffer + vid.rowbytes*y);
		t = (y & 1) << 1;

		for (x=0 ; x<vid.width ; x++)
		{
			if ((x & 3) != t)
				pbuf[x] = 0;
		}
	}

	VID_UnlockBuffer ();
	S_ExtraUpdate ();
	VID_LockBuffer ();
}

//=============================================================================

/*
================
Draw_BeginDisc

Draws the little blue disc in the corner of the screen.
Call before beginning any disc IO.
================
*/
void Draw_BeginDisc (void)
{

	D_BeginDirectRect (vid.width - 24, 0, draw_disc->data, 24, 24);
}


/*
================
Draw_EndDisc

Erases the disc icon.
Call after completing any disc IO
================
*/
void Draw_EndDisc (void)
{

	D_EndDirectRect (vid.width - 24, 0, 24, 24);
}

