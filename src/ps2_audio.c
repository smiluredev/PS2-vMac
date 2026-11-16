/*
	ps2_audio.c

	audsrv backend for the Macintosh Plus sound system. The core produces
	8-bit unsigned mono samples; this adapter outputs signed 16-bit stereo
	PCM at the Macintosh sample rate for broad audsrv compatibility.
*/

#include <tamtypes.h>
#include <audsrv.h>
#include <stdio.h>

#include "ps2_audio.h"

#define MAC_SAMPLE_RATE 22255
#define PS2_SAMPLE_RATE 22050
#define MAC_AUDIO_CHUNK_SAMPLES 370

static int AudioReady = 0;
static unsigned int ResamplePhase = 0;
static char AudioError[96] = "not initialized";

int PS2Audio_Init(void)
{
	struct audsrv_fmt_t format;
	int ret;

	AudioReady = 0;
	ResamplePhase = 0;
	AudioError[0] = 0;
	ret = audsrv_init();
	if (ret != 0) {
		snprintf(AudioError, sizeof(AudioError), "audsrv_init=%d: %s",
			ret, audsrv_get_error_string());
		return 0;
	}

	format.freq = PS2_SAMPLE_RATE;
	format.bits = 16;
	format.channels = 2;

	ret = audsrv_set_format(&format);
	if (ret != 0) {
		snprintf(AudioError, sizeof(AudioError),
			"set_format=%d: %s", ret, audsrv_get_error_string());
		audsrv_quit();
		return 0;
	}

	ret = audsrv_set_volume(MAX_VOLUME);
	if (ret != 0) {
		snprintf(AudioError, sizeof(AudioError), "set_volume=%d", ret);
		audsrv_quit();
		return 0;
	}

	AudioReady = 1;
	return 1;
}

const char *PS2Audio_GetError(void)
{
	return AudioError;
}

int PS2Audio_IsReady(void)
{
	return AudioReady;
}

int PS2Audio_Available(void)
{
	int available;

	if (! AudioReady) {
		return 0;
	}
	available = audsrv_available();
	if (available < 0) {
		snprintf(AudioError, sizeof(AudioError), "available=%d: %s",
			available, audsrv_get_error_string());
		return 0;
	}
	return available;
}

int PS2Audio_Write(const void *samples, int bytes)
{
	const u8 *mono = (const u8 *)samples;
	short stereo[MAC_AUDIO_CHUNK_SAMPLES * 2];
	int written = 0;
	unsigned int phase = ResamplePhase;

	if (! AudioReady || samples == NULL || bytes <= 0) {
		return 0;
	}

	while (written < bytes) {
		int count = bytes - written;
		int outputFrames = 0;
		int i;
		int result;

		if (count > MAC_AUDIO_CHUNK_SAMPLES) {
			count = MAC_AUDIO_CHUNK_SAMPLES;
		}

		/* Convert ~22.255 kHz core samples to audsrv's standard
		   22.050 kHz format without allowing the queue to drift. */
		for (i = 0; i < count; ++i) {
			phase += PS2_SAMPLE_RATE;
			if (phase >= MAC_SAMPLE_RATE) {
				short sample = (short)(((int)mono[written + i] - 128) * 256);
				phase -= MAC_SAMPLE_RATE;
				stereo[outputFrames * 2] = sample;
				stereo[outputFrames * 2 + 1] = sample;
				++outputFrames;
			}
		}

		if (outputFrames > 0) {
			int outputBytes = outputFrames * 4;
			int available = audsrv_available();

			/* Never block the emulation tick waiting for SPU2 queue space.
			   If audio falls behind, drop this chunk instead of freezing Mac OS. */
			if (available < 0) {
				snprintf(AudioError, sizeof(AudioError),
					"available=%d: %s", available,
					audsrv_get_error_string());
				break;
			}
			if (available < outputBytes) {
				AudioError[0] = 0;
				ResamplePhase = phase;
				written += count;
				continue;
			}
			result = audsrv_play_audio((const char *)stereo,
				outputBytes);
			if (result < outputBytes) {
				snprintf(AudioError, sizeof(AudioError),
					"play_audio=%d/%d", result, outputBytes);
				break;
			}
		}
		ResamplePhase = phase;
		written += count;
	}

	return written;
}

void PS2Audio_Stop(void)
{
	if (AudioReady) {
		audsrv_stop_audio();
	}
}

void PS2Audio_Quit(void)
{
	if (AudioReady) {
		audsrv_stop_audio();
		audsrv_quit();
		AudioReady = 0;
		ResamplePhase = 0;
	}
}
