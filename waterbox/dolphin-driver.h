// The adapter's C surface. Grown milestone by milestone; the gate harness
// and the waterbox ABI shim are its two callers.
// SPDX-License-Identifier: MIT
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

const char* chimera_dolphin_error(void);
int chimera_dolphin_init(const char* user_dir, const char* sys_dir, const char* game_path);
void chimera_dolphin_frame(void);
void chimera_dolphin_state_loaded(void);
// Before a state is taken, and where the copy it makes of the EFB goes (see
// dolphin-driver.cpp). The blocks are given once, before chimera_dolphin_init.
void chimera_dolphin_state_saving(void);
void chimera_dolphin_efb_blocks(void* block, size_t block_bytes, void* scratch,
                                size_t scratch_bytes);
uint8_t* chimera_dolphin_ram_ptr(void);
int64_t chimera_dolphin_ram_size(void);
void chimera_dolphin_shutdown(void);
void chimera_dolphin_set_button(int pad, int index, int state);
void chimera_dolphin_set_axis(int pad, int index, int value);
int chimera_dolphin_input_was_read(void);
// what a lag frame is: "picture" (no new picture, the default) or "controller"
// (the first controller not read); 0 for anything else
int chimera_dolphin_set_lag_frames(const char* name);
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
// The Triforce's SegaBoot firmware, by the name it can be opened under; null
// or empty for a cabinet without it (no test menu). Before chimera_dolphin_init.
void chimera_dolphin_set_sega_boot(const char* path);
void chimera_dolphin_set_cpu_core(const char* name);
void chimera_dolphin_set_port(int port, int present);
// What a port holds (chimera#285): "none", "gc-controller", "wii-remote",
// "wii-remote-nunchuk" or "classic-controller"; 0 for another name. Before
// chimera_dolphin_init.
int chimera_dolphin_set_port_device(int port, const char* name);
// 0 none, 1 GameCube controller, 2 Wii Remote, 3 with a Nunchuk, 4 Classic
int chimera_dolphin_port_device(int port);
// A Wii port's wire (waterbox.config's Wii machine): the GameCube controller's
// twelve buttons, then 1, 2, Plus, Minus, Home, Shake, C, ZL, ZR; its six axes,
// then Pointer X and Y (-32768..32767 across the screen, down positive) and
// Tilt X and Y (-128..127, right and forward positive). Values as declared.
void chimera_dolphin_set_wii_button(int port, int index, int state);
void chimera_dolphin_set_wii_axis(int port, int index, int value);
// The project's discs after the first, in the Game slot's order, before
// chimera_dolphin_init (chimera#286); and how many discs there are in all.
void chimera_dolphin_add_disc(const char* path);
int chimera_dolphin_disc_count(void);
// The console's own controls after the pads: 0 Previous Disc, 1 Next Disc,
// 2 Disc Swap (the lid, up while held).
void chimera_dolphin_set_console(int index, int state);
int chimera_dolphin_port_present(int port);
void chimera_dolphin_set_renderer(const char* name);
const char* chimera_dolphin_renderer_refusal(void);
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
