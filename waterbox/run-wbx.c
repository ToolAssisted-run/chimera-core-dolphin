/* Standalone driver for the waterboxed Dolphin core: runs core.wbx through
 * the miniBox host over a GC executable/disc and reports per-frame RAM
 * digests in run-native's exact format, so the sandboxed build can be
 * diffed against the native reference.
 *
 * usage: run-wbx <core.wbx> --sys <Data/Sys dir> [--frames N] [--report N]
 *        [--ram-out FILE] <game.dol|iso>
 *
 * The Sys directory is mounted file by file under "/sys/<relative path>";
 * the game under its own basename (extension drives type detection) with
 * "rom.name" carrying that name, exactly the frontend shape.
 */
#include "minibox.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static uint64_t fnv(uint64_t h, const void *p, size_t n)
{
	const uint8_t *b = (const uint8_t *)p;
	if (!h) h = 1469598103934665603ULL;
	for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ULL; }
	return h;
}

typedef struct { FILE *f; } freader;
static intptr_t file_read(uintptr_t ud, uint8_t *d, uintptr_t s) { return (intptr_t)fread(d, 1, s, ((freader *)ud)->f); }
typedef struct { const uint8_t *p; size_t n, pos; } memreader;
static intptr_t mem_reader(uintptr_t ud, uint8_t *d, uintptr_t s)
{
	memreader *m = (memreader *)ud;
	size_t take = s < (m->n - m->pos) ? s : (m->n - m->pos);
	memcpy(d, m->p + m->pos, take); m->pos += take; return (intptr_t)take;
}
typedef struct { uint8_t *b; size_t len, cap, pos; } membuf;
static int32_t mem_write(uintptr_t ud, const uint8_t *d, uintptr_t n)
{
	membuf *m = (membuf *)ud;
	if (m->len + n > m->cap) { m->cap = (m->len + n) * 2 + 64; m->b = realloc(m->b, m->cap); }
	memcpy(m->b + m->len, d, n); m->len += n; return 0;
}
static intptr_t mem_read(uintptr_t ud, uint8_t *d, uintptr_t n)
{
	membuf *m = (membuf *)ud;
	uintptr_t avail = m->len - m->pos; if (n > avail) n = avail;
	memcpy(d, m->b + m->pos, n); m->pos += n; return (intptr_t)n;
}

typedef int (MB_GUEST_ABI *intfn)(void);
typedef void (MB_GUEST_ABI *framefn)(uint64_t);
typedef void (MB_GUEST_ABI *btnfn)(int32_t, int32_t);
typedef uintptr_t (MB_GUEST_ABI *ptrfn)(void);
typedef uintptr_t (MB_GUEST_ABI *ptrfn_i)(int);
typedef int64_t (MB_GUEST_ABI *i64fn_i)(int);
typedef int32_t (MB_GUEST_ABI *i32fn)(void);

static uintptr_t proc(mb_host *h, const char *n)
{
	mb_return r; wbx_get_proc_addr(h, n, &r);
	if (r.error_message[0]) { fprintf(stderr, "proc %s: %s\n", n, r.error_message); exit(2); }
	if (!r.data) { fprintf(stderr, "missing required export %s\n", n); exit(2); }
	return r.data;
}

/* mount one host file read-only under the given guest name */
static int mountFile(mb_host *h, const char *guestName, const char *hostPath)
{
	mb_return r;
	FILE *f = fopen(hostPath, "rb");
	if (!f) { fprintf(stderr, "cannot open %s\n", hostPath); return -1; }
	freader fr = { f };
	wbx_mount_file(h, guestName, file_read, (uintptr_t)&fr, false, &r);
	fclose(f);
	if (r.error_message[0]) { fprintf(stderr, "mount %s: %s\n", guestName, r.error_message); return -1; }
	return 0;
}

/* mount every regular file under hostDir as "<guestPrefix>/<relative>" */
static int mountTree(mb_host *h, const char *guestPrefix, const char *hostDir)
{
	DIR *d = opendir(hostDir);
	if (!d) { fprintf(stderr, "cannot open dir %s\n", hostDir); return -1; }
	struct dirent *e;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.') continue;
		char hp[2048], gp[2048];
		snprintf(hp, sizeof hp, "%s/%s", hostDir, e->d_name);
		snprintf(gp, sizeof gp, "%s/%s", guestPrefix, e->d_name);
		struct stat st;
		if (stat(hp, &st) != 0) continue;
		if (S_ISDIR(st.st_mode)) {
			if (mountTree(h, gp, hp) != 0) { closedir(d); return -1; }
		} else if (S_ISREG(st.st_mode)) {
			if (mountFile(h, gp, hp) != 0) { closedir(d); return -1; }
		}
	}
	closedir(d);
	return 0;
}

int main(int argc, char **argv)
{
	const char *core = NULL, *game = NULL, *sysdir = NULL, *ramOut = NULL, *savedataOut = NULL;
	const char *saves[8]; int nsaves = 0;
	const char *settingsJson = NULL;
	long frames = 60, report = 10;
	int rewind = 0, rerecord = 0;
	struct { long first, count; int index; } press[32];
	int presses = 0;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atol(argv[++i]);
		else if (!strcmp(argv[i], "--report") && i + 1 < argc) report = atol(argv[++i]);
		else if (!strcmp(argv[i], "--sys") && i + 1 < argc) sysdir = argv[++i];
		else if (!strcmp(argv[i], "--ram-out") && i + 1 < argc) ramOut = argv[++i];
		else if (!strcmp(argv[i], "--savedata-out") && i + 1 < argc) savedataOut = argv[++i];
		else if (!strcmp(argv[i], "--save") && i + 1 < argc && nsaves < 8) saves[nsaves++] = argv[++i];
		else if (!strcmp(argv[i], "--settings") && i + 1 < argc) settingsJson = argv[++i];
		else if (!strcmp(argv[i], "--rewind")) rewind = 1;
		else if (!strcmp(argv[i], "--rerecord")) rerecord = 1;
		else if (!strcmp(argv[i], "--press") && i + 1 < argc && presses < 32) {
			long a, b; int c;
			if (sscanf(argv[++i], "%ld:%ld:%d", &a, &b, &c) == 3) {
				press[presses].first = a; press[presses].count = b; press[presses].index = c; presses++;
			}
		}
		else if (!core) core = argv[i];
		else game = argv[i];
	}
	if (!core || !game || !sysdir) {
		fprintf(stderr, "usage: run-wbx <core.wbx> --sys <dir> [--frames N] [--report N] [--ram-out F] <game>\n");
		return 2;
	}

	FILE *wf = fopen(core, "rb");
	if (!wf) { fprintf(stderr, "cannot open %s\n", core); return 1; }

	/* Dolphin is a big machine: 24MB GC RAM + 16MB ARAM as anonymous mmaps,
	 * plus dolphin's own heap (texture/vertex managers, DiscIO buffers). */
	mb_memory_layout_template layout = { 256u << 20, 16u << 20, 64u << 20, 256u << 20, 2560u << 20 };
	freader fr = { wf };
	mb_return r;
	wbx_create_host(&layout, "core.wbx", file_read, (uintptr_t)&fr, &r);
	fclose(wf);
	if (r.error_message[0]) { fprintf(stderr, "create: %s\n", r.error_message); return 1; }
	mb_host *h = (mb_host *)r.data;

	/* the game under its original basename + rom.name, the frontend shape */
	const char *base = strrchr(game, '/');
	base = base ? base + 1 : game;
	char vfsname[512];
	snprintf(vfsname, sizeof vfsname, "/%s", base);
	/* zero-copy: a disc is gigabytes and the guest reads it lazily */
	wbx_mount_file_path(h, vfsname, game, &r);
	if (r.error_message[0]) { fprintf(stderr, "mount %s: %s\n", vfsname, r.error_message); return 1; }
	memreader nr = { (const uint8_t *)vfsname, strlen(vfsname), 0 };
	wbx_mount_file(h, "rom.name", mem_reader, (uintptr_t)&nr, false, &r);
	if (r.error_message[0]) { fprintf(stderr, "mount rom.name: %s\n", r.error_message); return 1; }

	if (mountTree(h, "/sys", sysdir) != 0) return 1;

	/* the settings channel, exactly as the frontend mounts it */
	if (settingsJson) {
		memreader sr = { (const uint8_t *)settingsJson, strlen(settingsJson), 0 };
		wbx_mount_file(h, "settings", mem_reader, (uintptr_t)&sr, false, &r);
		if (r.error_message[0]) { fprintf(stderr, "mount settings: %s\n", r.error_message); return 1; }
	}

	/* prior saves, mounted at exactly the path the machine opens */
	for (int i = 0; i < nsaves; i++) {
		char id[256]; const char *eq = strchr(saves[i], '=');
		if (!eq || eq == saves[i] || (size_t)(eq - saves[i]) >= sizeof id - 10) { fprintf(stderr, "bad --save %s (want name=path)\n", saves[i]); return 2; }
		char gp[300];
		memcpy(id, saves[i], eq - saves[i]); id[eq - saves[i]] = 0;
		snprintf(gp, sizeof gp, "savedata/%s", id);
		if (mountFile(h, gp, eq + 1) != 0) return 1;
	}

	wbx_activate_host(h, &r);
	intfn Init = (intfn)proc(h, "Init");
	if (Init() != 1) {
		ptrfn GetLoadError = (ptrfn)proc(h, "GetLoadError");
		fprintf(stderr, "Init failed: %s\n", (const char *)GetLoadError());
		return 1;
	}

	framefn FrameAdvance = (framefn)proc(h, "FrameAdvance");
	btnfn SetButton = (btnfn)proc(h, "SetButton");
	intfn InputWasRead = (intfn)proc(h, "InputWasRead");
	ptrfn GetVideoBgra = (ptrfn)proc(h, "GetVideoBgra");
	intfn GetVideoWidth = (intfn)proc(h, "GetVideoWidth");
	intfn GetVideoHeight = (intfn)proc(h, "GetVideoHeight");
	ptrfn GetAudio = (ptrfn)proc(h, "GetAudio");
	intfn GetAudioSampleCount = (intfn)proc(h, "GetAudioSampleCount");
	ptrfn_i GetMemoryDomainPtr = (ptrfn_i)proc(h, "GetMemoryDomainPtr");
	i64fn_i GetMemoryDomainSize = (i64fn_i)proc(h, "GetMemoryDomainSize");

	/* seal: the post-boot machine is the savestate baseline */
	wbx_deactivate_host(h, &r);
	wbx_seal(h, &r);
	if (r.error_message[0]) { fprintf(stderr, "seal: %s\n", r.error_message); return 1; }
	wbx_activate_host(h, &r);

	const uint8_t *ram = (const uint8_t *)GetMemoryDomainPtr(0);
	int64_t ramSize = GetMemoryDomainSize(0);
	printf("booted paused; ram %lld bytes\n", (long long)ramSize);
	fflush(stdout);

	if (rewind) {
		/* the TAS shape: run half, save, finish recording digests, load the
		 * mid-state and finish again - the two second halves must agree */
		long half = frames / 2;
		for (long f = 1; f <= half; f++) FrameAdvance(0);
		membuf st = {0};
		wbx_save_state(h, mem_write, (uintptr_t)&st, &r);
		if (r.error_message[0]) { fprintf(stderr, "save: %s\n", r.error_message); return 1; }
		uint64_t pass1 = 0, pass2 = 0;
		for (long f = half + 1; f <= frames; f++) { FrameAdvance(0); pass1 = fnv(pass1, ram, (size_t)ramSize); }
		st.pos = 0;
		wbx_load_state(h, mem_read, (uintptr_t)&st, &r);
		if (r.error_message[0]) { fprintf(stderr, "load: %s\n", r.error_message); return 1; }
		for (long f = half + 1; f <= frames; f++) { FrameAdvance(0); pass2 = fnv(pass2, ram, (size_t)ramSize); }
		printf("rewind: pass1 %016llx pass2 %016llx -> %s\n", (unsigned long long)pass1,
		       (unsigned long long)pass2, pass1 == pass2 ? "EQUAL" : "DIFFERENT");
		free(st.b);
		wbx_deactivate_host(h, &r); wbx_destroy_host(h, &r);
		return pass1 == pass2 ? 0 : 1;
	}

	long lag = 0;
	for (long f = 1; f <= frames; f++) {
		for (int pi = 0; pi < presses; pi++)
			SetButton(press[pi].index, f >= press[pi].first && f < press[pi].first + press[pi].count);
		if (rerecord) {
			/* save+load around every frame; the digests must match a plain run */
			membuf st = {0};
			wbx_save_state(h, mem_write, (uintptr_t)&st, &r);
			if (r.error_message[0]) { fprintf(stderr, "save@%ld: %s\n", f, r.error_message); return 1; }
			st.pos = 0;
			wbx_load_state(h, mem_read, (uintptr_t)&st, &r);
			if (r.error_message[0]) { fprintf(stderr, "load@%ld: %s\n", f, r.error_message); return 1; }
			free(st.b);
		}
		FrameAdvance(0);
		if (!InputWasRead()) lag++;
		if (f % report == 0 || f == frames) {
			int vw = GetVideoWidth(), vh = GetVideoHeight(), an = GetAudioSampleCount();
			const uint8_t *vid = (const uint8_t *)GetVideoBgra();
			const uint8_t *aud = (const uint8_t *)GetAudio();
			printf("frame %5ld ram %016llx vid %dx%d %016llx aud %d %016llx lag %ld\n",
			       f, (unsigned long long)fnv(0, ram, (size_t)ramSize), vw, vh,
			       (unsigned long long)fnv(0, vid, (size_t)vw * vh * 4), an,
			       (unsigned long long)fnv(0, aud, (size_t)an * 4), lag);
			fflush(stdout);
		}
	}
	if (ramOut) {
		FILE *rf2 = fopen(ramOut, "wb");
		fwrite(ram, 1, (size_t)ramSize, rf2);
		fclose(rf2);
	}
	if (savedataOut) {
		i32fn SdCount = (i32fn)proc(h, "GetSaveDataFileCount");
		ptrfn_i SdName = (ptrfn_i)proc(h, "GetSaveDataFileName");
		i64fn_i SdSize = (i64fn_i)proc(h, "GetSaveDataFileSize");
		ptrfn_i SdBuf = (ptrfn_i)proc(h, "GetSaveDataFileBuffer");
		for (int i = 0; i < SdCount(); i++) {
			char path[1024];
			snprintf(path, sizeof path, "%s/%s", savedataOut, (const char *)SdName(i));
			FILE *f = fopen(path, "wb");
			if (!f) { fprintf(stderr, "cannot write %s\n", path); continue; }
			fwrite((const void *)SdBuf(i), 1, (size_t)SdSize(i), f);
			fclose(f);
			printf("savedata %s %lld bytes\n", (const char *)SdName(i), (long long)SdSize(i));
		}
	}
	wbx_deactivate_host(h, &r);
	wbx_destroy_host(h, &r);
	printf("done\n");
	return 0;
}
