/*
 * natives_storage.c - nanojava.Storage: flat key/value files in the
 * platform data directory, used by javax.microedition.rms.
 *
 * Keys are arbitrary strings; they are hex-encoded into file names so any
 * record store name maps to a valid, unique file on FAT and POSIX alike.
 */
#include "midp.h"
#include "../vm/vm.h"

#include <stdio.h>
#include <stdlib.h>

#define EXT ".rms"

static bool key_path(Object *key, char *out, size_t size)
{
    char   name[200];
    size_t n = strlen(str_to_utf8(key, name, sizeof name));
    const char *dir = pal_data_dir();
    if (!dir)
        return false;
    if (strlen(dir) + 2 * n + sizeof EXT + 2 > size)
        return false;
    char *p = out + sprintf(out, "%s/", dir);
    for (size_t i = 0; i < n; i++)
        p += sprintf(p, "%02x", (unsigned char)name[i]);
    strcpy(p, EXT);
    return true;
}

static int Storage_read(Thread *t, Slot *args, Slot *ret)
{
    char path[512];
    if (!args[0].ref) {
        NPE(t);
        return NATIVE_OK;
    }
    if (!key_path(args[0].ref, path, sizeof path))
        return NATIVE_OK;
    PalFile *f = pal_fopen(path, false);
    if (!f)
        return NATIVE_OK;
    uint32_t size = pal_fsize(f);
    Array *a = vm_new_prim_array(t, 'B', (jint)size);
    if (a && pal_fread(f, ARRAY_DATA(a, uint8_t), size) != size)
        a = NULL;
    pal_fclose(f);
    ret->ref = (Object *)a;
    return NATIVE_OK;
}

static int Storage_write(Thread *t, Slot *args, Slot *ret)
{
    char path[512], tmp[520];
    Object *data = args[1].ref;
    if (!args[0].ref || !data) {
        NPE(t);
        return NATIVE_OK;
    }
    if (!key_path(args[0].ref, path, sizeof path))
        return NATIVE_OK;
    /* Write a temporary file first so a crash never leaves half a store. */
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    PalFile *f = pal_fopen(tmp, true);
    if (!f)
        return NATIVE_OK;
    uint32_t n = (uint32_t)ARRAY_LEN(data);
    bool     ok = pal_fwrite(f, ARRAY_DATA(data, uint8_t), n) == n;
    ok = pal_fclose(f) && ok;
    if (ok) {
        pal_remove(path);
        ok = pal_rename(tmp, path);
    }
    if (!ok)
        pal_remove(tmp);
    ret->i = ok;
    return NATIVE_OK;
}

static int Storage_delete(Thread *t, Slot *args, Slot *ret)
{
    char path[512];
    if (!args[0].ref) {
        NPE(t);
        return NATIVE_OK;
    }
    ret->i = key_path(args[0].ref, path, sizeof path) && pal_remove(path);
    return NATIVE_OK;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* Decodes a hex file name (without extension) into out; false if it is
 * not one of ours. */
static bool decode_name(const char *hex, size_t n, char *out)
{
    if (n % 2)
        return false;
    for (size_t i = 0; i < n; i += 2) {
        int hi = hexval(hex[i]), lo = hexval(hex[i + 1]);
        if (hi < 0 || lo < 0)
            return false;
        out[i / 2] = (char)(hi * 16 + lo);
    }
    out[n / 2] = 0;
    return true;
}

/* static String[] list(): all keys */
static int Storage_list(Thread *t, Slot *args, Slot *ret)
{
    char **names = NULL;
    int    n = 0, cap = 0;
    const char *dir = pal_data_dir();
    PalDir *d = dir ? pal_opendir(dir) : NULL;
    if (d) {
        const char *e;
        while ((e = pal_readdir(d))) {
            size_t len = strlen(e), ext = strlen(EXT);
            if (len <= ext || strcmp(e + len - ext, EXT) != 0)
                continue;
            char *name = pal_malloc((len - ext) / 2 + 1);
            if (!name || !decode_name(e, len - ext, name)) {
                pal_free(name);
                continue;
            }
            if (n == cap) {
                cap = cap ? cap * 2 : 16;
                char **grown = pal_realloc(names, sizeof(char *) * (size_t)cap);
                if (!grown) {
                    pal_free(name);
                    break;
                }
                names = grown;
            }
            names[n++] = name;
        }
        pal_closedir(d);
    }
    Array *a = vm_new_array(t, class_array_of(vm.c_String), n);
    for (int i = 0; i < n && a; i++) {
        Object *s = str_new_utf8(t, names[i]);
        if (!s)
            a = NULL;
        else
            ARRAY_DATA(a, Object *)[i] = s;
    }
    for (int i = 0; i < n; i++)
        pal_free(names[i]);
    pal_free(names);
    ret->ref = (Object *)a;
    return NATIVE_OK;
}

static const NativeEntry storage_natives[] = {
    {"nanojava/Storage", "read", NULL, Storage_read},
    {"nanojava/Storage", "write", NULL, Storage_write},
    {"nanojava/Storage", "delete", NULL, Storage_delete},
    {"nanojava/Storage", "list", NULL, Storage_list},
    {NULL, NULL, NULL, NULL}
};

void midp_storage_init(void)
{
    native_register(storage_natives);
}
