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

#include "quakedef.h"
#include "quakegeneric.h"

void QG_Tick(double duration)
{
	Host_Frame(duration);
}

void QG_Create(int argc, char *argv[])
{
	static quakeparms_t    parms;
	int i;

	COM_InitArgv (argc, argv);

	// -mem <MB>, as in the DOS and Linux builds; 16 MB by default (the
	// engine refuses less than MINIMUM_MEMORY, about 5.3 MB)
	parms.memsize = 16*1024*1024;
	i = COM_CheckParm ("-mem");
	if (i && i < com_argc-1)
		parms.memsize = (int)(Q_atof (com_argv[i+1]) * 1024 * 1024);
	parms.membase = calloc (parms.memsize, 1);	// zeroed, as for -crc in vid_null.c
	if (!parms.membase)
		Sys_Error ("Not enough memory free for a %d KB hunk (-mem <MB>)", parms.memsize / 1024);
	parms.basedir = (char *)QG_BaseDir ();

	parms.argc = com_argc;
	parms.argv = com_argv;

	printf ("Host_Init\n");
	Host_Init (&parms);
}
