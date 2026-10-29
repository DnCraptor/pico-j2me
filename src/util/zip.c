/*
 * zip.c - ZIP central directory reader. Many J2ME jars were produced by
 * obfuscators and hand-rolled packers, so the reader trusts only the central
 * directory and skips anything it does not understand.
 */
#include "zip.h"
#include "../pal/pal_sys.h"
#include "inflate.h"

#include <stdlib.h>
#include <string.h>

struct Zip {
    const uint8_t *mem;      /* in-memory archive, or NULL */
    PalFile       *file;
    uint32_t       size;
    ZipEntry      *entries;
    uint32_t       n;
    ZipEntry     **buckets;
    uint32_t       nbuckets;
    char          *names;
    bool           rom;     /* XIP classlib: directory stays in flash */
    uint32_t       rom_cd_off, rom_cd_size;
    ZipEntry       rom_entry; /* one temporary lookup result, not reentrant */
};

static uint32_t name_hash(const char *s)
{
    uint32_t h = 2166136261u;
    while (*s)
        h = (h ^ (uint8_t)*s++) * 16777619u;
    return h;
}

static int read_at(Zip *z, uint32_t off, void *dst, uint32_t len)
{
    if (off > z->size || len > z->size - off)
        return -1;
    if (z->mem) {
        memcpy(dst, z->mem + off, len);
        return 0;
    }
    if (!pal_fseek(z->file, off))
        return -1;
    return pal_fread(z->file, dst, len) == len ? 0 : -1;
}

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t le32(const uint8_t *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int parse(Zip *z)
{
    /* Search the last 64 KiB for EOCD with a 1 KiB stack buffer instead
     * of a 65 KiB malloc. Overlap chunks by 21 bytes for split headers. */
    uint8_t buf[1024];
    uint8_t eocd[22];
    uint32_t lower = z->size > 65557u ? z->size - 65557u : 0;
    uint32_t upper = z->size;
    bool found = false;
    while (upper > lower && !found) {
        uint32_t first = upper - lower > sizeof buf ? upper - sizeof buf : lower;
        uint32_t n = upper - first;
        if (read_at(z, first, buf, n) != 0)
            return -1;
        for (int i = (int)n - 22; i >= 0; --i) {
            if (le32(buf + i) == 0x06054b50u) {
                memcpy(eocd, buf + i, sizeof eocd);
                found = true;
                break;
            }
        }
        if (first == lower)
            break;
        upper = first + 21;
    }
    if (!found)
        return -1;

    uint32_t count = le16(eocd + 10);
    uint32_t cd_size = le32(eocd + 12);
    uint32_t cd_off = le32(eocd + 16);
    if (cd_off > z->size || cd_size > z->size - cd_off)
        return -1;
    if (z->rom) {
        z->rom_cd_off = cd_off;
        z->rom_cd_size = cd_size;
        z->n = count;
        return 0; /* no malloc; all ZIP metadata stays in XIP flash */
    }

    uint8_t *cd = pal_malloc(cd_size ? cd_size : 1);
    if (!cd || read_at(z, cd_off, cd, cd_size) != 0) {
        pal_free(cd);
        return -1;
    }

    z->entries = pal_calloc(count ? count : 1, sizeof(ZipEntry));
    z->names = pal_malloc(cd_size + 1);
    z->nbuckets = 16;
    while (z->nbuckets < count)
        z->nbuckets <<= 1;
    z->buckets = pal_calloc(z->nbuckets, sizeof(ZipEntry *));
    if (!z->entries || !z->names || !z->buckets) {
        pal_free(cd);
        return -1;
    }

    char    *names = z->names;
    uint32_t p = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (p + 46 > cd_size || le32(cd + p) != 0x02014b50)
            break;
        uint16_t nlen = le16(cd + p + 28);
        uint16_t xlen = le16(cd + p + 30);
        uint16_t clen = le16(cd + p + 32);
        if (p + 46 + nlen > cd_size)
            break;
        ZipEntry *e = &z->entries[z->n];
        e->method = le16(cd + p + 10);
        e->csize = le32(cd + p + 20);
        e->usize = le32(cd + p + 24);
        e->local_off = le32(cd + p + 42);
        memcpy(names, cd + p + 46, nlen);
        names[nlen] = '\0';
        e->name = names;
        names += nlen + 1;
        e->hash = name_hash(e->name);
        uint32_t b = e->hash & (z->nbuckets - 1);
        e->next = z->buckets[b];
        z->buckets[b] = e;
        z->n++;
        p += 46 + nlen + xlen + clen;
    }
    pal_free(cd);
    return 0;
}

static Zip *finish_open(Zip *z)
{
    if (parse(z) != 0) {
        zip_close(z);
        return NULL;
    }
    return z;
}

Zip *zip_open_mem(const uint8_t *data, uint32_t size)
{
    Zip *z = pal_calloc(1, sizeof *z);
    if (!z)
        return NULL;
    z->mem = data;
    z->size = size;
    return finish_open(z);
}

Zip *zip_open_rom(const uint8_t *data, uint32_t size)
{
    /* One embedded classlib per VM. No native allocation even on startup. */
    static Zip z;
    memset(&z, 0, sizeof z);
    z.mem = data;
    z.size = size;
    z.rom = true;
    return parse(&z) == 0 ? &z : NULL;
}

Zip *zip_open_file(const char *path)
{
    PalFile *f = pal_fopen(path, false);
    if (!f)
        return NULL;
    Zip *z = pal_calloc(1, sizeof *z);
    if (!z) {
        pal_fclose(f);
        return NULL;
    }
    z->file = f;
    z->size = pal_fsize(f);
    return finish_open(z);
}

void zip_close(Zip *z)
{
    if (!z || z->rom) /* ROM archive has static storage and no allocations */
        return;
    if (z->file)
        pal_fclose(z->file);
    pal_free(z->entries);
    pal_free(z->names);
    pal_free(z->buckets);
    pal_free(z);
}

ZipEntry *zip_find(Zip *z, const char *name)
{
    if (name[0] == '/')
        name++;
    if (z->rom) {
        /* The embedded JAR's central directory is already in flash.
         * Parse one entry at a time; do not copy its index into SRAM. */
        const uint8_t *cd = z->mem + z->rom_cd_off;
        uint32_t pos = 0;
        size_t want = strlen(name);
        for (uint32_t i = 0; i < z->n; ++i) {
            if (pos > z->rom_cd_size || z->rom_cd_size - pos < 46 ||
                le32(cd + pos) != 0x02014b50u)
                return NULL;
            uint32_t nlen = le16(cd + pos + 28);
            uint32_t xlen = le16(cd + pos + 30);
            uint32_t clen = le16(cd + pos + 32);
            uint32_t remain = z->rom_cd_size - pos - 46;
            if (nlen > remain || xlen > remain - nlen ||
                clen > remain - nlen - xlen)
                return NULL;
            if (nlen == want && memcmp(cd + pos + 46, name, nlen) == 0) {
                ZipEntry *e = &z->rom_entry;
                e->name = name;
                e->hash = name_hash(name);
                e->method = le16(cd + pos + 10);
                e->csize = le32(cd + pos + 20);
                e->usize = le32(cd + pos + 24);
                e->local_off = le32(cd + pos + 42);
                e->next = NULL;
                return e;
            }
            pos += 46 + nlen + xlen + clen;
        }
        return NULL;
    }
    uint32_t h = name_hash(name);
    for (ZipEntry *e = z->buckets[h & (z->nbuckets - 1)]; e; e = e->next)
        if (e->hash == h && strcmp(e->name, name) == 0)
            return e;
    return NULL;
}

uint32_t zip_count(Zip *z) { return z->n; }

ZipEntry *zip_entry(Zip *z, uint32_t index)
{
    /* The ROM classlib is looked up by name, not enumerated. */
    return !z->rom && index < z->n ? &z->entries[index] : NULL;
}

uint8_t *zip_read(Zip *z, ZipEntry *e, uint32_t *len)
{
    uint8_t hdr[30];
    if (read_at(z, e->local_off, hdr, 30) != 0 || le32(hdr) != 0x04034b50)
        return NULL;
    uint32_t data_off = e->local_off + 30 + le16(hdr + 26) + le16(hdr + 28);

    uint8_t *out = pal_malloc(e->usize + 1);
    if (!out)
        return NULL;
    if (e->method == 0) {
        if (read_at(z, data_off, out, e->usize) != 0)
            goto fail;
    } else if (e->method == 8) {
        uint8_t *in = pal_malloc(e->csize ? e->csize : 1);
        if (!in)
            goto fail;
        if (read_at(z, data_off, in, e->csize) != 0 ||
            nj_inflate(out, e->usize, in, e->csize) != (int32_t)e->usize) {
            pal_free(in);
            goto fail;
        }
        pal_free(in);
    } else {
        goto fail;
    }
    out[e->usize] = 0;
    if (len)
        *len = e->usize;
    return out;
fail:
    pal_free(out);
    return NULL;
}

uint8_t *zip_read_name(Zip *z, const char *name, uint32_t *len)
{
    ZipEntry *e = zip_find(z, name);
    return e ? zip_read(z, e, len) : NULL;
}
