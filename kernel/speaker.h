#ifndef SPEAKER_H
#define SPEAKER_H

#include <stdint.h>

void speaker_play(uint32_t nFrequence);
void speaker_stop();
void speaker_beep(uint32_t freq, uint32_t duration);

#endif
