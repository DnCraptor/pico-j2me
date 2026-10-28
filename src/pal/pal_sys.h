/*
 * Nano Java - native memory and file access, part of the platform layer.
 *
 * The portable runtime never calls malloc or stdio directly: on boards
 * like the RP2350 the native heap lives in QSPI PSRAM and the files on a
 * FAT card behind FatFs, so each platform provides these functions.
 */
#ifndef NJ_PAL_SYS_H
#define NJ_PAL_SYS_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Native heap (class metadata, thread stacks, decoded resources...). */
void *pal_malloc(size_t n);
void *pal_calloc(size_t n, size_t size);
void *pal_realloc(void *p, size_t n);
void  pal_free(void *p);

/* The Java heap arena, allocated once per VM run (heap_init) and released
 * by heap_shutdown. Kept apart from pal_malloc so a platform can place it
 * in its biggest memory. */
void *pal_heap_alloc(size_t n);
void  pal_heap_free(void *p);

/* Files. Paths use '/'. A file is opened for reading, or created (and
 * truncated) for writing. */
typedef struct PalFile PalFile;

PalFile *pal_fopen(const char *path, bool write);
uint32_t pal_fread(PalFile *f, void *dst, uint32_t len);       /* bytes read */
uint32_t pal_fwrite(PalFile *f, const void *src, uint32_t len); /* bytes written */
bool     pal_fseek(PalFile *f, uint32_t off);
uint32_t pal_fsize(PalFile *f);
bool     pal_fclose(PalFile *f);                               /* false: write error */

bool pal_remove(const char *path);
bool pal_rename(const char *from, const char *to);              /* `to` must not exist */

/* Directory listing: the names (not paths) of the files in dir, without
 * "." and "..". The returned name is valid until the next call. */
typedef struct PalDir PalDir;

PalDir     *pal_opendir(const char *dir);
const char *pal_readdir(PalDir *d);
void        pal_closedir(PalDir *d);

#endif
