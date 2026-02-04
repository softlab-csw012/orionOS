#ifndef ELF_H
#define ELF_H

#include <stdint.h>
#include <stdbool.h>

bool elf_load_image(const char* path,
                    uint32_t* out_entry,
                    uint32_t* out_image_base,
                    uint32_t* out_image_size,
                    uint32_t* out_load_base,
                    bool* out_is_elf);

#endif
