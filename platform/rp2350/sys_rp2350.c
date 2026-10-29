/*
 * sys_rp2350.c - pal_sys.h on the RP2350: the native heap in QSPI PSRAM
 * (TLSF allocator), files on the SD card through FatFs.
 *
 * Without PSRAM the native heap falls back to the SRAM malloc, which only
 * leaves room for small MIDlets.
 */
#include "rp2350.h"
#include "psram.h"
#include "tlsf.h"

#include <stdlib.h>
#include <string.h>

#include "ff.h"

/* ------------------------------------------------------------------------ */
/* Memory                                                                   */

static tlsf_t pool;          /* NULL: no PSRAM, use the SRAM malloc */
static size_t reserved;      /* bytes kept at the top of the PSRAM (gm_bank.bin) */

void sys_mem_init(void)
{
    sys_mem_reset();
}

void sys_mem_reset(void)
{
    if (!psram_available()) {
        pool = NULL;
        return;
    }
    /* Cached window: the XIP cache keeps the hot data (class metadata,
     * thread stacks, the Java heap) close to the core. */
    pool = tlsf_create_with_pool((void *)PSRAM_BASE, psram_size() - reserved);
}

bool sys_psram(void)
{
    return pool != NULL;
}

size_t sys_psram_size(void)
{
    return pool ? psram_size() - reserved : 0;
}

void *sys_mem_reserve_top(size_t n)
{
    if (!pool)
        return NULL;
    n = (n + 4095) & ~(size_t)4095;
    if (n > psram_size() - reserved)
        return NULL;
    reserved += n;
    sys_mem_reset();
    return (void *)(PSRAM_BASE + psram_size() - reserved);
}

void sys_mem_unreserve_top(size_t n)
{
    n = (n + 4095) & ~(size_t)4095;
    reserved = n > reserved ? 0 : reserved - n;
    sys_mem_reset();
}

typedef struct {
    size_t largest;
} WalkState;

static void walker(void *ptr, size_t size, int used, void *user)
{
    (void)ptr;
    WalkState *w = user;
    if (!used && size > w->largest)
        w->largest = size;
}

size_t sys_mem_free(void)
{
    if (!pool)
        return 0;
    WalkState w = {0};
    tlsf_walk_pool(tlsf_get_pool(pool), walker, &w);
    return w.largest;
}

/* 8-byte aligned like the newlib malloc (TLSF alone gives 4 on 32 bits):
 * metadata blocks hold doubles and 64-bit integers. */
void *pal_malloc(size_t n)
{
    return pool ? tlsf_memalign(pool, 8, n ? n : 1) : malloc(n);
}

void *pal_calloc(size_t n, size_t size)
{
    if (size && n > (size_t)-1 / size)
        return NULL;
    size_t total = n * size;
    void  *p = pal_malloc(total);
    if (p)
        memset(p, 0, total);
    return p;
}

void *pal_realloc(void *p, size_t n)
{
    return pool ? tlsf_realloc(pool, p, n) : realloc(p, n);
}

void pal_free(void *p)
{
    if (!p)
        return;
    if (pool)
        tlsf_free(pool, p);
    else
        free(p);
}

void *pal_heap_alloc(size_t n)
{
    return pal_malloc(n);
}

void pal_heap_free(void *p)
{
    pal_free(p);
}

/* ------------------------------------------------------------------------ */
/* Files                                                                    */

/* FIL objects (and their sector buffers) stay in SRAM. */
struct PalFile {
    FIL  fil;
    bool write;
};

PalFile *pal_fopen(const char *path, bool write)
{
    if (!nj_sd_ok)
        return NULL;
    PalFile *f = malloc(sizeof *f);
    if (!f)
        return NULL;
    BYTE mode = write ? (FA_WRITE | FA_CREATE_ALWAYS) : FA_READ;
    if (f_open(&f->fil, path, mode) != FR_OK) {
        free(f);
        return NULL;
    }
    f->write = write;
    return f;
}

uint32_t pal_fread(PalFile *f, void *dst, uint32_t len)
{
    UINT got = 0;
    if (f_read(&f->fil, dst, len, &got) != FR_OK)
        return 0;
    return got;
}

uint32_t pal_fwrite(PalFile *f, const void *src, uint32_t len)
{
    UINT put = 0;
    if (f_write(&f->fil, src, len, &put) != FR_OK)
        return 0;
    return put;
}

bool pal_fseek(PalFile *f, uint32_t off)
{
    return f_lseek(&f->fil, off) == FR_OK && f_tell(&f->fil) == off;
}

uint32_t pal_fsize(PalFile *f)
{
    return (uint32_t)f_size(&f->fil);
}

bool pal_fclose(PalFile *f)
{
    bool ok = f_close(&f->fil) == FR_OK;
    free(f);
    return ok;
}

bool pal_remove(const char *path)
{
    return nj_sd_ok && f_unlink(path) == FR_OK;
}

bool pal_rename(const char *from, const char *to)
{
    return nj_sd_ok && f_rename(from, to) == FR_OK;
}

struct PalDir {
    DIR     dir;
    FILINFO info;
};

PalDir *pal_opendir(const char *dir)
{
    if (!nj_sd_ok)
        return NULL;
    PalDir *d = malloc(sizeof *d);
    if (!d)
        return NULL;
    if (f_opendir(&d->dir, dir) != FR_OK) {
        free(d);
        return NULL;
    }
    return d;
}

const char *pal_readdir(PalDir *d)
{
    for (;;) {
        if (f_readdir(&d->dir, &d->info) != FR_OK || !d->info.fname[0])
            return NULL;
        const char *n = d->info.fname;
        if (strcmp(n, ".") != 0 && strcmp(n, "..") != 0)
            return n;
    }
}

void pal_closedir(PalDir *d)
{
    f_closedir(&d->dir);
    free(d);
}

bool sys_mkdirs(const char *dir)
{
    if (!nj_sd_ok)
        return false;
    char path[256];
    size_t n = strlen(dir);
    if (n == 0 || n >= sizeof path)
        return false;
    memcpy(path, dir, n + 1);
    for (size_t i = 1; i <= n; i++) {
        if (path[i] == '/' || path[i] == 0) {
            char c = path[i];
            path[i] = 0;
            FRESULT fr = f_mkdir(path);
            if (fr != FR_OK && fr != FR_EXIST)
                return false;
            path[i] = c;
        }
    }
    return true;
}
