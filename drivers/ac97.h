#ifndef AC97_H
#define AC97_H

#include <stdbool.h>
#include <stdint.h>

bool ac97_is_present(void);
bool ac97_pci_attach(uint8_t bus, uint8_t device, uint8_t function);

void ac97_stop(void);
int  ac97_play_tone(uint32_t freq_hz, uint32_t duration_ms);
int  ac97_play_wav(const uint8_t* wav, uint32_t wav_size);
int  ac97_play_wav_file(const char* path);
void ac97_dump(void);

#endif
