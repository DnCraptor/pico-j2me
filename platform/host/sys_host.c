/*
 * sys_host.c - pal_sys.h for the host build: libc memory and stdio files.
 */
#include "../../src/pal/pal_sys.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *pal_malloc(size_t n)              { return malloc(n); }
void *pal_calloc(size_t n, size_t size) { return calloc(n, size); }
void *pal_realloc(void *p, size_t n)    { return realloc(p, n); }
void  pal_free(void *p)                 { free(p); }

void *pal_heap_alloc(size_t n)          { return malloc(n); }
void  pal_heap_free(void *p)            { free(p); }

struct PalFile {
    FILE *f;
};

PalFile *pal_fopen(const char *path, bool write)
{
    FILE *f = fopen(path, write ? "wb" : "rb");
    if (!f)
        return NULL;
    PalFile *pf = malloc(sizeof *pf);
    if (!pf) {
        fclose(f);
        return NULL;
    }
    pf->f = f;
    return pf;
}

uint32_t pal_fread(PalFile *f, void *dst, uint32_t len)
{
    return (uint32_t)fread(dst, 1, len, f->f);
}

uint32_t pal_fwrite(PalFile *f, const void *src, uint32_t len)
{
    return (uint32_t)fwrite(src, 1, len, f->f);
}

bool pal_fseek(PalFile *f, uint32_t off)
{
    return fseek(f->f, (long)off, SEEK_SET) == 0;
}

uint32_t pal_fsize(PalFile *f)
{
    long cur = ftell(f->f);
    fseek(f->f, 0, SEEK_END);
    long size = ftell(f->f);
    fseek(f->f, cur, SEEK_SET);
    return size < 0 ? 0 : (uint32_t)size;
}

bool pal_fclose(PalFile *f)
{
    bool ok = fclose(f->f) == 0;
    free(f);
    return ok;
}

bool pal_remove(const char *path)                 { return remove(path) == 0; }
bool pal_rename(const char *from, const char *to) { return rename(from, to) == 0; }

struct PalDir {
    DIR *d;
};

PalDir *pal_opendir(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d)
        return NULL;
    PalDir *pd = malloc(sizeof *pd);
    if (!pd) {
        closedir(d);
        return NULL;
    }
    pd->d = d;
    return pd;
}

const char *pal_readdir(PalDir *d)
{
    struct dirent *e;
    while ((e = readdir(d->d)))
        if (strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0)
            return e->d_name;
    return NULL;
}

void pal_closedir(PalDir *d)
{
    closedir(d->d);
    free(d);
}
