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

typedef int (MB_GUEST_ABI *intfn)(void);
typedef void (MB_GUEST_ABI *framefn)(uint64_t);
typedef uintptr_t (MB_GUEST_ABI *ptrfn)(void);
typedef uintptr_t (MB_GUEST_ABI *ptrfn_i)(int);
typedef int64_t (MB_GUEST_ABI *i64fn_i)(int);

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
	const char *core = NULL, *game = NULL, *sysdir = NULL, *ramOut = NULL;
	long frames = 60, report = 10;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atol(argv[++i]);
		else if (!strcmp(argv[i], "--report") && i + 1 < argc) report = atol(argv[++i]);
		else if (!strcmp(argv[i], "--sys") && i + 1 < argc) sysdir = argv[++i];
		else if (!strcmp(argv[i], "--ram-out") && i + 1 < argc) ramOut = argv[++i];
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

	wbx_activate_host(h, &r);
	intfn Init = (intfn)proc(h, "Init");
	if (Init() != 1) {
		ptrfn GetLoadError = (ptrfn)proc(h, "GetLoadError");
		fprintf(stderr, "Init failed: %s\n", (const char *)GetLoadError());
		return 1;
	}

	framefn FrameAdvance = (framefn)proc(h, "FrameAdvance");
	ptrfn_i GetMemoryDomainPtr = (ptrfn_i)proc(h, "GetMemoryDomainPtr");
	i64fn_i GetMemoryDomainSize = (i64fn_i)proc(h, "GetMemoryDomainSize");

	const uint8_t *ram = (const uint8_t *)GetMemoryDomainPtr(0);
	int64_t ramSize = GetMemoryDomainSize(0);
	printf("booted paused; ram %lld bytes\n", (long long)ramSize);
	fflush(stdout);

	for (long f = 1; f <= frames; f++) {
		FrameAdvance(0);
		if (f % report == 0 || f == frames) {
			printf("frame %5ld ram %016llx\n", f, (unsigned long long)fnv(0, ram, (size_t)ramSize));
			fflush(stdout);
		}
	}
	if (ramOut) {
		FILE *rf2 = fopen(ramOut, "wb");
		fwrite(ram, 1, (size_t)ramSize, rf2);
		fclose(rf2);
	}
	wbx_deactivate_host(h, &r);
	wbx_destroy_host(h, &r);
	printf("done\n");
	return 0;
}
