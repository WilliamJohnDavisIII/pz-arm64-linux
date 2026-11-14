#pragma once
// Load an Android arm64 shared object that glibc cannot map on a 16 KB-page kernel.
// Returns a handle, or NULL (the reason is printed to stderr).
void *pzldr_open(const char *path);
// Look up a symbol the library defines.
void *pzldr_sym(void *handle, const char *name);
