#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PSRAM_BASE ((uintptr_t)0x11000000u)
#define PSRAM_MAX_SIZE (16u * 1024u * 1024u)

#ifdef __cplusplus
extern "C" {
#endif

bool psram_init(void);
bool psram_available(void);
size_t psram_size(void);
int psram_cs1_gpio(void);
void psram_prepare_reset(void);
void psram_reclock(void); /* after every clk_sys change */

#ifdef __cplusplus
}
#endif
