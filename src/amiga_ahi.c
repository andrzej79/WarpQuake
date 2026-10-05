// WarpQuake - sound output through AHI (the QG_Sound* hooks, snd_qg.c).
//
// Quake mixes into a ring buffer and asks where the hardware is reading
// (SNDDMA_GetDMAPos), as with a DMA sound card.  Here AHI plays that ring as
// one looping dynamic sample (AHIST_DYNAMICSAMPLE: AHI reads the memory as
// it mixes, so what Quake writes ahead of the play position is heard).  The
// play position is not something AHI reports, so it is measured: AHI calls
// the sound hook each time the sample (re)starts, the hook notes the E-clock,
// and the position is the time since then times the mixing rate.
//
// The audio mode and mixing rate are chosen with -ahireq (AHI's requester,
// on the game's screen), -ahimode <id> / -ahifreq <Hz>, or the in-game
// Sound Options menu, and saved in PROGDIR:WarpQuake.audio.  The default is
// AHI's default mode (its preferences, unit 0) at 22050 Hz.

#ifndef __VBCC__
#define __reg(x)
#define __saveds
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <exec/types.h>
#include <exec/memory.h>
#include <devices/timer.h>
#include <devices/ahi.h>
#include <utility/hooks.h>
#include <proto/exec.h>
#include <proto/timer.h>
#include <proto/ahi.h>

#include "quakegeneric.h"
#include "amiga_qg.h"

#define AUDIO_FILE "PROGDIR:WarpQuake.audio"
#define DEFAULT_FREQ 22050
#define RING_SECONDS_MIN 0.3      // ring length: at least this, a power of 2 frames

struct Library *AHIBase = NULL;
extern struct Device *TimerBase;    // amiga_main.c, the E-clock

static struct MsgPort *ahiPort = NULL;
static struct AHIRequest *ahiReq = NULL;
static BOOL ahiDeviceOpen = FALSE;
static struct AHIAudioCtrl *ahiCtrl = NULL;
static BOOL soundLoaded = FALSE;

static WORD *ring = NULL;           // 16-bit stereo, big-endian (AHIST_S16S)
static ULONG ringFrames;
static ULONG mixFreq;               // what AHI actually mixes at
static double framesPerTick;        // mixFreq / E-clock rate

static struct Hook soundHook;
static volatile ULONG loopStart;    // E-clock (low word) when the ring last started
static volatile ULONG loopCount;

static ULONG wantMode = AHI_DEFAULT_ID;
static ULONG wantFreq = DEFAULT_FREQ;
static char soundInfo[96];

static const char *argValue(const char *name)
{
  extern int com_argc;
  extern char **com_argv;
  int i;

  for(i = 1; i < com_argc; i++) {
    if(strcmp(com_argv[i], name) == 0) {
      return (i < com_argc - 1) ? com_argv[i + 1] : "";
    }
  }
  return NULL;
}

/*
===============================================================================
SETTINGS
===============================================================================
*/

static void loadSettings(void)
{
  FILE *f = fopen(AUDIO_FILE, "r");
  char line[64];

  if(f != NULL) {
    if(fgets(line, sizeof(line), f) != NULL) {
      char *p;
      wantMode = strtoul(line, &p, 0);
      wantFreq = strtoul(p, NULL, 0);
    }
    fclose(f);
  }
  if(wantFreq < 4000 || wantFreq > 96000) {
    wantFreq = DEFAULT_FREQ;
  }
}

static void saveSettings(void)
{
  FILE *f = fopen(AUDIO_FILE, "w");

  if(f != NULL) {
    fprintf(f, "0x%08lx %lu\n", wantMode, wantFreq);
    fclose(f);
  }
}

void QG_SoundGetSettings(unsigned long *mode, int *freq)
{
  *mode = wantMode;
  *freq = (int)wantFreq;
}

void QG_SoundSetSettings(unsigned long mode, int freq)
{
  wantMode = mode;
  wantFreq = (ULONG)freq;
  saveSettings();
}

/*
===============================================================================
AHI DEVICE (the library interface comes with it)
===============================================================================
*/

static BOOL ahiOpen(void)
{
  if(AHIBase != NULL) {
    return TRUE;
  }
  ahiPort = CreateMsgPort();
  if(ahiPort == NULL) {
    return FALSE;
  }
  ahiReq = (struct AHIRequest *)CreateIORequest(ahiPort, sizeof(struct AHIRequest));
  if(ahiReq == NULL) {
    return FALSE;
  }
  ahiReq->ahir_Version = 4;
  if(OpenDevice((STRPTR)AHINAME, AHI_NO_UNIT, (struct IORequest *)ahiReq, 0) != 0) {
    return FALSE;
  }
  ahiDeviceOpen = TRUE;
  AHIBase = (struct Library *)ahiReq->ahir_Std.io_Device;
  return TRUE;
}

static void ahiClose(void)
{
  if(ahiDeviceOpen) {
    CloseDevice((struct IORequest *)ahiReq);
    ahiDeviceOpen = FALSE;
  }
  AHIBase = NULL;
  if(ahiReq != NULL) {
    DeleteIORequest((struct IORequest *)ahiReq);
    ahiReq = NULL;
  }
  if(ahiPort != NULL) {
    DeleteMsgPort(ahiPort);
    ahiPort = NULL;
  }
}

// AHI's mode requester, on the game's screen (it is exclusive).  FALSE if
// cancelled.
static BOOL askMode(void)
{
  struct AHIAudioModeRequester *req;
  BOOL ok = FALSE;

  req = AHI_AllocAudioRequest(AHIR_TitleText, (ULONG) "WarpQuake: sound mode", AHIR_InitialAudioID, wantMode,
                              AHIR_InitialMixFreq, wantFreq, AHIR_DoMixFreq, TRUE, AHIR_DoDefaultMode, TRUE,
                              AHIR_Screen, (ULONG)qgGetScreen(), TAG_DONE);
  if(req != NULL) {
    if(AHI_AudioRequest(req, TAG_DONE)) {
      wantMode = req->ahiam_AudioID;
      wantFreq = req->ahiam_MixFreq;
      ok = TRUE;
    }
    AHI_FreeAudioRequest(req);
  }
  return ok;
}

/*
===============================================================================
PLAYBACK
===============================================================================
*/

// Called by AHI each time the ring sample starts over (also the first time).
// From AHI's mixing context: only notes the time.
static ULONG __saveds soundFunc(__reg("a0") struct Hook *hook, __reg("a2") struct AHIAudioCtrl *ctrl,
                                __reg("a1") struct AHISoundMessage *msg)
{
  struct EClockVal ev;

  (void)hook;
  (void)ctrl;
  (void)msg;
  ReadEClock(&ev);
  loopStart = ev.ev_lo;
  loopCount++;
  return 0;
}

void *QG_SoundInit(int *frames, int *rate)
{
  struct EClockVal ev;
  struct AHISampleInfo sample;
  const char *arg;
  ULONG eclock;

  loadSettings();
  if((arg = argValue("-ahimode")) != NULL) {
    wantMode = strtoul(arg, NULL, 0);
  }
  if((arg = argValue("-ahifreq")) != NULL) {
    wantFreq = strtoul(arg, NULL, 0);
  }
  if(argValue("-headless") != NULL || TimerBase == NULL) {
    return NULL;
  }
  if(!ahiOpen()) {
    qgPrintf("Sound: no ahi.device V4\n");
    QG_SoundShutdown();
    return NULL;
  }
  if(argValue("-ahireq") != NULL && askMode()) {
    saveSettings();
  }

  memset(&soundHook, 0, sizeof(soundHook));
  soundHook.h_Entry = (HOOKFUNC)soundFunc;
  ahiCtrl = AHI_AllocAudio(AHIA_AudioID, wantMode, AHIA_MixFreq, wantFreq, AHIA_Channels, 1, AHIA_Sounds, 1,
                           AHIA_SoundFunc, (ULONG)&soundHook, TAG_DONE);
  if(ahiCtrl == NULL) {
    qgPrintf("Sound: cannot allocate AHI mode 0x%08lx at %lu Hz\n", wantMode, wantFreq);
    QG_SoundShutdown();
    return NULL;
  }
  mixFreq = 0;
  AHI_ControlAudio(ahiCtrl, AHIC_MixFreq_Query, (ULONG)&mixFreq, TAG_DONE);
  if(mixFreq == 0) {
    mixFreq = wantFreq;
  }

  // the ring: a power of 2 frames (Quake masks positions), >= RING_SECONDS_MIN
  ringFrames = 1024;
  while(ringFrames < (ULONG)(mixFreq * RING_SECONDS_MIN)) {
    ringFrames <<= 1;
  }
  ring = AllocVec(ringFrames * 4, MEMF_PUBLIC | MEMF_CLEAR);
  if(ring == NULL) {
    QG_SoundShutdown();
    return NULL;
  }
  sample.ahisi_Type = AHIST_S16S;
  sample.ahisi_Address = ring;
  sample.ahisi_Length = ringFrames;
  if(AHI_LoadSound(0, AHIST_DYNAMICSAMPLE, &sample, ahiCtrl) != AHIE_OK) {
    qgPrintf("Sound: AHI_LoadSound failed\n");
    QG_SoundShutdown();
    return NULL;
  }
  soundLoaded = TRUE;

  eclock = ReadEClock(&ev);
  framesPerTick = (double)mixFreq / (double)eclock;
  loopStart = ev.ev_lo;
  loopCount = 0;

  if(AHI_ControlAudio(ahiCtrl, AHIC_Play, TRUE, TAG_DONE) != AHIE_OK) {
    qgPrintf("Sound: AHI won't play\n");
    QG_SoundShutdown();
    return NULL;
  }
  AHI_Play(ahiCtrl, AHIP_BeginChannel, 0, AHIP_Freq, mixFreq, AHIP_Vol, 0x10000, AHIP_Pan, 0x8000, AHIP_Sound, 0,
           AHIP_EndChannel, 0, TAG_DONE);

  {
    // the mode the control really got (the default mode resolves to one)
    char name[64];
    ULONG id = AHI_INVALID_ID;

    name[0] = 0;
    AHI_GetAudioAttrs(AHI_INVALID_ID, ahiCtrl, AHIDB_AudioID, (ULONG)&id, TAG_DONE);
    AHI_GetAudioAttrs(id, NULL, AHIDB_Name, (ULONG)name, AHIDB_BufferLen, sizeof(name), TAG_DONE);
    sprintf(soundInfo, "AHI %s%s, %lu Hz", wantMode == AHI_DEFAULT_ID ? "default: " : "", name[0] ? name : "?",
            mixFreq);
  }
  qgPrintf("Sound: %s, %lu-frame ring\n", soundInfo, ringFrames);
  *frames = (int)ringFrames;
  *rate = (int)mixFreq;
  return ring;
}

// The frame AHI is playing now: the time since the ring last started.
int QG_SoundPos(void)
{
  struct EClockVal ev;
  ULONG start, count;
  double f;

  if(ahiCtrl == NULL) {
    return 0;
  }
  do {                                  // the hook may run between the reads
    count = loopCount;
    start = loopStart;
    ReadEClock(&ev);
  } while(count != loopCount);
  f = (double)(ULONG)(ev.ev_lo - start) * framesPerTick;
  if(f >= (double)(ringFrames - 1)) {
    return (int)ringFrames - 1;          // the hook is due: hold at the end
  }
  return (int)f;
}

const char *QG_SoundInfo(void)
{
  return ahiCtrl != NULL ? soundInfo : "none";
}

void QG_SoundShutdown(void)
{
  if(ahiCtrl != NULL) {
    AHI_ControlAudio(ahiCtrl, AHIC_Play, FALSE, TAG_DONE);
    if(soundLoaded) {
      AHI_UnloadSound(0, ahiCtrl);
      soundLoaded = FALSE;
    }
    AHI_FreeAudio(ahiCtrl);
    ahiCtrl = NULL;
  }
  if(ring != NULL) {
    FreeVec(ring);
    ring = NULL;
  }
  ahiClose();
}

/*
===============================================================================
MODE LIST (the Sound Options menu)
===============================================================================
*/

int QG_SoundListModes(qgaudiomode_t *modes, int max)
{
  ULONG id = AHI_INVALID_ID;
  int n = 0;
  BOOL opened = (AHIBase == NULL);

  if(argValue("-headless") != NULL || !ahiOpen()) {
    return 0;
  }
  while(n < max && (id = AHI_NextAudioID(id)) != AHI_INVALID_ID) {
    modes[n].id = id;
    modes[n].name[0] = 0;
    AHI_GetAudioAttrs(id, NULL, AHIDB_Name, (ULONG)modes[n].name, AHIDB_BufferLen, sizeof(modes[n].name), TAG_DONE);
    n++;
  }
  if(opened && ahiCtrl == NULL) {
    ahiClose();                         // opened just for the list
  }
  return n;
}
