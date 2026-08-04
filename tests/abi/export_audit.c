#include <stdio.h>
#include <stdint.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
typedef HMODULE library_handle;
static library_handle open_library(const char* path) { return LoadLibraryA(path); }
static void* find_symbol(library_handle library, const char* name) {
    return (void*)(uintptr_t)GetProcAddress(library, name);
}
static void close_library(library_handle library) { FreeLibrary(library); }
#else
#include <dlfcn.h>
typedef void* library_handle;
static library_handle open_library(const char* path) {
    return dlopen(path, RTLD_NOW | RTLD_LOCAL);
}
static void* find_symbol(library_handle library, const char* name) {
    return dlsym(library, name);
}
static void close_library(library_handle library) { dlclose(library); }
#endif

int main(int argc, char** argv) {
    char symbol[256];
    size_t checked = 0;
    FILE* manifest;
    library_handle library;

    if (argc != 3) {
        fprintf(stderr, "usage: export_audit <library> <manifest>\n");
        return 2;
    }
    library = open_library(argv[1]);
    if (!library) {
        fprintf(stderr, "cannot load ABI library: %s\n", argv[1]);
        return 3;
    }
    manifest = fopen(argv[2], "rb");
    if (!manifest) {
        fprintf(stderr, "cannot read ABI export manifest: %s\n", argv[2]);
        close_library(library);
        return 4;
    }
    while (fgets(symbol, (int)sizeof(symbol), manifest)) {
        size_t length = strlen(symbol);
        while (length && (symbol[length - 1] == '\r' || symbol[length - 1] == '\n')) {
            symbol[--length] = '\0';
        }
        if (!length || symbol[0] == '#') continue;
        ++checked;
        if (!find_symbol(library, symbol)) {
            fprintf(stderr, "missing required C ABI export: %s\n", symbol);
            fclose(manifest);
            close_library(library);
            return 5;
        }
    }
    fclose(manifest);
    close_library(library);
    if (!checked) {
        fprintf(stderr, "ABI export manifest is empty\n");
        return 6;
    }
    printf("verified %lu C ABI exports\n", (unsigned long)checked);
    return 0;
}
