#ifndef HDA_H
#define HDA_H

#include <stdbool.h>
#include <stdint.h>

bool hda_is_present(void);
bool hda_pci_attach(uint8_t bus, uint8_t device, uint8_t function);
bool hda_pci_attach_force(uint8_t bus, uint8_t device, uint8_t function);
int  hda_get_count(void);
int  hda_get_active_index(void);
bool hda_select(int index);
void hda_set_forced_pin(uint8_t nid);
uint8_t hda_get_forced_pin(void);
void hda_list(void);

void hda_dump(void);

// Debug/bring-up helper: send an HDA verb (uses Immediate Command interface).
// Returns 0 on success, non-zero on failure.
int hda_send_verb(uint8_t cad, uint8_t nid, uint16_t verb, uint8_t payload, uint32_t* out_resp);

// Minimal playback helpers (polled DMA stream).
void hda_stop(void);
int  hda_play_tone(uint32_t freq_hz, uint32_t duration_ms);
int  hda_play_wav(const uint8_t* wav, uint32_t wav_size);

#endif
