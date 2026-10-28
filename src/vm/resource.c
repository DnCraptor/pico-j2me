/*
 * resource.c - where classes and resources come from.
 *
 * Classes are looked up in the system class library first and then in the
 * application (jar or directory). Resources (getResourceAsStream) only come
 * from the application.
 */
#include "vm.h"
#include "../util/zip.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_APP 4

static Zip        *system_zip;
static Zip        *app_zips[MAX_APP];
static int         n_app_zips;
static const char *app_dirs[MAX_APP];
static int         n_app_dirs;

static uint8_t *read_file(const char *dir, const char *path, uint32_t *len)
{
    char full[512];
    snprintf(full, sizeof full, "%s/%s", dir, path[0] == '/' ? path + 1 : path);
    PalFile *f = pal_fopen(full, false);
    if (!f)
        return NULL;
    uint32_t size = pal_fsize(f);
    uint8_t *buf = pal_malloc((size_t)size + 1);
    if (buf && pal_fread(f, buf, size) != size) {
        pal_free(buf);
        buf = NULL;
    }
    pal_fclose(f);
    if (buf) {
        buf[size] = 0;
        *len = (uint32_t)size;
    }
    return buf;
}

static uint8_t *system_source(const char *path, uint32_t *len)
{
    return system_zip ? zip_read_name(system_zip, path, len) : NULL;
}

uint8_t *res_read_app(const char *path, uint32_t *len)
{
    for (int i = 0; i < n_app_zips; i++) {
        uint8_t *d = zip_read_name(app_zips[i], path, len);
        if (d)
            return d;
    }
    for (int i = 0; i < n_app_dirs; i++) {
        uint8_t *d = read_file(app_dirs[i], path, len);
        if (d)
            return d;
    }
    return NULL;
}

static uint8_t *jad_data;
static uint32_t jad_len;

void res_set_jad(const uint8_t *data, uint32_t len)
{
    pal_free(jad_data);
    jad_data = pal_malloc(len + 1);
    memcpy(jad_data, data, len);
    jad_len = len;
}

const uint8_t *res_jad(uint32_t *len)
{
    *len = jad_len;
    return jad_data;
}

static bool sources_added;

static void add_sources(void)
{
    if (!sources_added) {
        class_add_source(system_source);
        class_add_source(res_read_app);
        sources_added = true;
    }
}

void res_reset(void)
{
    zip_close(system_zip);
    for (int i = 0; i < n_app_zips; i++)
        zip_close(app_zips[i]);
    system_zip = NULL;
    n_app_zips = n_app_dirs = 0;
    pal_free(jad_data);
    jad_data = NULL;
    jad_len = 0;
    sources_added = false;
}

void res_set_system(Zip *z)
{
    system_zip = z;
    add_sources();
}

void res_add_app_zip(Zip *z)
{
    if (n_app_zips < MAX_APP)
        app_zips[n_app_zips++] = z;
    add_sources();
}

void res_add_app_dir(const char *dir)
{
    if (n_app_dirs < MAX_APP)
        app_dirs[n_app_dirs++] = dir;
    add_sources();
}
