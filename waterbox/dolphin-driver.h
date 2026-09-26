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
void chimera_dolphin_state_loaded(void);
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
void chimera_dolphin_set_machine(const char* name);
void chimera_dolphin_set_memcard_a(int present);
// the Wii's own "Screen: Widescreen" system setting (SYSCONF IPL.AR)
void chimera_dolphin_set_widescreen(int on);
// one of the emulation options a project pins (chimera#149); 0 = unknown name or value
int chimera_dolphin_set_option(const char* name, const char* value);
// the options as dolphin holds them after boot, one line (the gate reads it)
int chimera_dolphin_options_report(char* out, int size);
// a Triforce panel's Test (0), Service (1) and Coin (2) switches, per player
void chimera_dolphin_set_switch(int pad, int index, int state);
// nonzero when the project is a Triforce cabinet
int chimera_dolphin_triforce(void);
// a Wii project's saves to start from: the .zip Export Save Data wrote
void chimera_dolphin_set_wii_savedata(const char* zip_path);
void chimera_dolphin_set_cpu_core(const char* name);
void chimera_dolphin_set_port(int port, int present);
int chimera_dolphin_port_present(int port);
void chimera_dolphin_set_renderer(const char* name);
int chimera_dolphin_savedata_count(void);
int chimera_dolphin_domain_count(void);
uint8_t* chimera_dolphin_domain_ptr(int i);
int64_t chimera_dolphin_domain_size(int i);
const char* chimera_dolphin_domain_name(int i);
const char* chimera_dolphin_savedata_name(int i);
int64_t chimera_dolphin_savedata_size(int i);
const uint8_t* chimera_dolphin_savedata_buffer(int i);

#ifdef __cplusplus
}
#endif
