// The adapter's C surface. Grown milestone by milestone; the gate harness
// and the waterbox ABI shim are its two callers.
// SPDX-License-Identifier: MIT
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

const char* chimera_dolphin_error(void);
int chimera_dolphin_init(const char* user_dir, const char* sys_dir, const char* game_path);
void chimera_dolphin_frame(void);
uint8_t* chimera_dolphin_ram_ptr(void);
int64_t chimera_dolphin_ram_size(void);
void chimera_dolphin_shutdown(void);
void chimera_dolphin_set_button(int pad, int index, int state);
void chimera_dolphin_set_axis(int pad, int index, int value);
int chimera_dolphin_input_was_read(void);
const uint32_t* chimera_dolphin_video(int* w, int* h);
const int16_t* chimera_dolphin_audio(int* frames);
int chimera_dolphin_vsync_numerator(void);
int chimera_dolphin_vsync_denominator(void);

#ifdef __cplusplus
}
#endif
