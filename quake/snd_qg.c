// snd_qg.c -- warpQuake: Quake's "DMA" sound interface over the platform's
// sound ring (QG_Sound*, src/amiga_ahi.c)
//
// snd_dma.c mixes into shm->buffer ahead of the play position, which it
// reads from SNDDMA_GetDMAPos; the platform plays the buffer in a loop.

#include "quakedef.h"
#include "quakegeneric.h"

static void SND_InitMenu (void);

// -sndtest: no device; a ring of our own whose play position follows the
// game's frame count, so that a fixed host_framerate mixes the same sound on any
// machine - with -crc, a check of the mixer under vamos (wq_prof.c).
static qboolean	sndtest;
static int		sndtestframes;

qboolean SNDDMA_Init (void)
{
	int		frames, rate;
	void	*buffer;

	SND_InitMenu ();					// even without a device: to choose another
	sndtest = COM_CheckParm ("-sndtest") != 0;
	if (sndtest)
	{
		static short	ring[8192 * 2];

		frames = sndtestframes = 8192;
		rate = 22050;
		buffer = ring;
	}
	else
		buffer = QG_SoundInit (&frames, &rate);

	if (!buffer)
		return false;

	shm = &sn;
	shm->splitbuffer = 0;
	shm->channels = 2;
	shm->samplebits = 16;
	shm->speed = rate;
	shm->samples = frames * 2;			// mono samples
	shm->samplepos = 0;
	shm->submission_chunk = 1;
	shm->soundalive = true;
	shm->gamealive = true;
	shm->buffer = buffer;
	return true;
}

int SNDDMA_GetDMAPos (void)
{
	if (sndtest)
		// frames * host_frametime: fixed with host_framerate, unlike realtime
		shm->samplepos = ((int)(host_framecount * host_frametime * shm->speed) & (sndtestframes - 1))
				* shm->channels;
	else
		shm->samplepos = QG_SoundPos () * shm->channels;
	return shm->samplepos;
}

void SNDDMA_Submit (void)
{
}

void SNDDMA_Shutdown (void)
{
	if (!sndtest)
		QG_SoundShutdown ();
	shm = NULL;
}

/*
===============================================================================
SOUND OPTIONS (menu and console)

The audio mode and mixing rate: Options / Sound Options, or snd_modelist,
snd_mode <0xid | default> [rate], snd_rate <Hz>, snd_restart.  A change
restarts the device at once (S_Restart) and is saved for the next start.
===============================================================================
*/

extern void M_Print (int cx, int cy, char *str);
extern void M_PrintWhite (int cx, int cy, char *str);
extern void M_DrawCharacter (int cx, int line, int num);
extern void M_DrawPic (int x, int y, qpic_t *pic);
extern void M_Menu_Options_f (void);
extern void (*snd_menudrawfn)(void);
extern void (*snd_menukeyfn)(int key);

#define SM_MAXMODES		64
#define SM_ROWS			13

static const int	sm_rates[] = { 11025, 16000, 22050, 32000, 44100 };
#define SM_NUMRATES		((int)(sizeof(sm_rates) / sizeof(sm_rates[0])))

static qgaudiomode_t	sm_modes[SM_MAXMODES + 1];	// [0]: the default mode
static int		sm_count = -1;			// -1: not read yet
static int		sm_col, sm_row, sm_top;

// Apply a mode and rate: saved, and the device restarted with them.
static void SND_Apply (unsigned long mode, int rate)
{
	QG_SoundSetSettings (mode, rate);
	S_Restart ();
	if (shm)
		Con_Printf ("Sound: %s\n", QG_SoundInfo ());
}

static void SND_ReadModes (void)
{
	unsigned long	mode;
	int				rate, i;

	sm_modes[0].id = 0;
	strcpy (sm_modes[0].name, "Default (AHI prefs)");
	sm_count = 1 + QG_SoundListModes (sm_modes + 1, SM_MAXMODES);
	QG_SoundGetSettings (&mode, &rate);
	sm_col = sm_row = 0;
	for (i = 0 ; i < sm_count ; i++)
		if (sm_modes[i].id == mode)
			sm_row = i;
}

static void SND_ModeList_f (void)
{
	unsigned long	mode;
	int				rate, i;

	SND_ReadModes ();
	QG_SoundGetSettings (&mode, &rate);
	for (i = 0 ; i < sm_count ; i++)
		Con_Printf ("%c 0x%08lx  %s\n", sm_modes[i].id == mode ? '*' : ' ', sm_modes[i].id, sm_modes[i].name);
	Con_Printf ("rate %i Hz; now playing: %s\n", rate, QG_SoundInfo ());
}

static void SND_Mode_f (void)
{
	unsigned long	mode;
	int				rate;

	QG_SoundGetSettings (&mode, &rate);
	if (Cmd_Argc () < 2)
	{
		Con_Printf ("snd_mode <0xid | default> [rate]: see snd_modelist\n");
		return;
	}
	mode = Q_strcasecmp (Cmd_Argv (1), "default") ? strtoul (Cmd_Argv (1), NULL, 0) : 0;
	if (Cmd_Argc () > 2)
		rate = Q_atoi (Cmd_Argv (2));
	SND_Apply (mode, rate);
	sm_count = -1;
}

static void SND_Rate_f (void)
{
	unsigned long	mode;
	int				rate;

	QG_SoundGetSettings (&mode, &rate);
	if (Cmd_Argc () != 2)
	{
		Con_Printf ("snd_rate <Hz>: now %i\n", rate);
		return;
	}
	SND_Apply (mode, Q_atoi (Cmd_Argv (1)));
}

static void SND_Restart_f (void)
{
	S_Restart ();
}

static void SND_MenuDraw (void)
{
	unsigned long	mode;
	int				rate, r, y;
	char			buf[40];

	if (sm_count < 0)
		SND_ReadModes ();
	QG_SoundGetSettings (&mode, &rate);

	M_DrawPic ((320 - 144) / 2, 4, Draw_CachePic ("gfx/p_option.lmp"));
	M_PrintWhite (16, 36, "Sound mode (AHI)");
	M_PrintWhite (248, 36, "Rate");

	if (sm_row < sm_top)
		sm_top = sm_row;
	if (sm_row >= sm_top + SM_ROWS)
		sm_top = sm_row - SM_ROWS + 1;
	for (r = sm_top ; r < sm_count && r < sm_top + SM_ROWS ; r++)
	{
		y = 48 + (r - sm_top) * 8;
		Q_strncpy (buf, sm_modes[r].name, 27);
		buf[27] = 0;
		if (sm_modes[r].id == mode)
			M_PrintWhite (24, y, buf);
		else
			M_Print (24, y, buf);
		if (sm_col == 0 && r == sm_row)
			M_DrawCharacter (12, y, 12 + ((int)(realtime * 4) & 1));
	}
	for (r = 0 ; r < SM_NUMRATES ; r++)
	{
		y = 48 + r * 8;
		sprintf (buf, "%i", sm_rates[r]);
		if (sm_rates[r] == rate)
			M_PrintWhite (256, y, buf);
		else
			M_Print (256, y, buf);
		if (sm_col == 1 && r == sm_row)
			M_DrawCharacter (244, y, 12 + ((int)(realtime * 4) & 1));
	}

	Q_strncpy (buf, shm ? (char *)QG_SoundInfo () : "no sound", 39);
	buf[39] = 0;
	M_Print (8, 160, "Now:");
	M_PrintWhite (48, 160, buf);
	M_Print (8, 176, "Enter: use       Esc: back");
}

static void SND_MenuKey (int key)
{
	unsigned long	mode;
	int				rate, n = sm_col ? SM_NUMRATES : sm_count;

	QG_SoundGetSettings (&mode, &rate);
	switch (key)
	{
	case K_ESCAPE:
		S_LocalSound ("misc/menu1.wav");
		M_Menu_Options_f ();
		break;

	case K_UPARROW:
		S_LocalSound ("misc/menu1.wav");
		sm_row = (sm_row > 0) ? sm_row - 1 : n - 1;
		break;

	case K_DOWNARROW:
		S_LocalSound ("misc/menu1.wav");
		sm_row = (sm_row < n - 1) ? sm_row + 1 : 0;
		break;

	case K_LEFTARROW:
	case K_RIGHTARROW:
		S_LocalSound ("misc/menu1.wav");
		sm_col = !sm_col;
		n = sm_col ? SM_NUMRATES : sm_count;
		if (sm_row >= n)
			sm_row = n - 1;
		break;

	case K_ENTER:
		if (sm_col == 0)
			SND_Apply (sm_modes[sm_row].id, rate);
		else
			SND_Apply (mode, sm_rates[sm_row]);
		S_LocalSound ("misc/menu2.wav");		// heard through the new mode
		break;
	}
}

static void SND_InitMenu (void)
{
	static qboolean	done;

	if (done)
		return;
	done = true;
	snd_menudrawfn = SND_MenuDraw;
	snd_menukeyfn = SND_MenuKey;
	Cmd_AddCommand ("snd_modelist", SND_ModeList_f);
	Cmd_AddCommand ("snd_mode", SND_Mode_f);
	Cmd_AddCommand ("snd_rate", SND_Rate_f);
	Cmd_AddCommand ("snd_restart", SND_Restart_f);
}
