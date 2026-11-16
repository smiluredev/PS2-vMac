#ifndef PS2_AUDIO_H
#define PS2_AUDIO_H

/* Initialize audsrv for 22050 Hz, 16-bit stereo PCM. */
int PS2Audio_Init(void);
const char *PS2Audio_GetError(void);

/* Free space in the audsrv ring buffer, in bytes. */
int PS2Audio_Available(void);
int PS2Audio_IsReady(void);

/* Queue unsigned 8-bit mono core samples, converted to 16-bit stereo.
   Returns the number of input mono samples accepted. */
int PS2Audio_Write(const void *samples, int bytes);

/* Stop playback without quitting the service. */
void PS2Audio_Stop(void);

void PS2Audio_Quit(void);

#endif /* PS2_AUDIO_H */
