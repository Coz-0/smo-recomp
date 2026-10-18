/* nn::audio device functions: the host's answers to two of the game's imports. The lifted SDK code normally replaces
 * them (nnservices.c install_lifted); they serve a run with SMO_LIFT_INSTALL=0 or SMO_LIFT_SKIP. The sound itself is
 * the AudioRenderer's (smo_audio.c). */
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include "runtime.h"
#include "hostrt.h"

#define A0 (c->x[0])

static const char g_device_name[] = "AudioTvOutput";

static void audio_GetActiveAudioDeviceName(Ctx *c) {  /* (AudioDeviceName *): char[0x100] */
    memset((void *)(uintptr_t)A0, 0, 0x100);
    memcpy((void *)(uintptr_t)A0, g_device_name, sizeof g_device_name);
}

static void audio_SetAudioDeviceOutputVolume(Ctx *c) {   /* (const AudioDeviceName *, float): traced, not applied */
    trace_printf("nn::audio: SetAudioDeviceOutputVolume(%s, %g)\n", (const char *)(uintptr_t)A0, c->v[0].f[0]);
}

#define AU(n) "_ZN2nn5audio" n
static const ImportDef audio_imports[] = {
    {AU("24GetActiveAudioDeviceNameEPNS0_15AudioDeviceNameE"), audio_GetActiveAudioDeviceName},
    {AU("26SetAudioDeviceOutputVolumeEPKNS0_15AudioDeviceNameEf"), audio_SetAudioDeviceOutputVolume},
};

void register_audio_imports(void) {
    register_imports(audio_imports, (int)(sizeof audio_imports / sizeof audio_imports[0]));
}
