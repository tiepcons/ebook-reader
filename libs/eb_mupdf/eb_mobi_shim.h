/* Force-included into every libmobi translation unit (never into our own sources).
 *
 * 1. Memory: malloc/calloc/realloc/free/strdup go to a per-conversion arena with a byte budget (eb_mobi.c). When the
 *    budget is exceeded the arena longjmps out of libmobi, so a failed allocation can never reach libmobi's
 *    unchecked dereferences, and everything libmobi still holds is released in one sweep.
 *    libmobi may only be called between arena_begin and arena_end in eb_mobi_convert; anything else is a bug.
 * 2. Input: fread/fseek/ftell read from an in-memory buffer. The FILE* given to mobi_load_file is really an
 *    eb_mobi memfile; mobi_load_filename and any real FILE* path must never be called.
 */
#ifndef EB_MOBI_SHIM_H
#define EB_MOBI_SHIM_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void* eb_mobi_malloc(size_t size);
void* eb_mobi_calloc(size_t count, size_t size);
void* eb_mobi_realloc(void* ptr, size_t size);
void eb_mobi_mfree(void* ptr);
char* eb_mobi_strdup(const char* s);

size_t eb_mobi_fread(void* buf, size_t size, size_t count, FILE* file);
int eb_mobi_fseek(FILE* file, long offset, int whence);
long eb_mobi_ftell(FILE* file);

#define malloc(n) eb_mobi_malloc(n)
#define calloc(c, n) eb_mobi_calloc(c, n)
#define realloc(p, n) eb_mobi_realloc(p, n)
#define free(p) eb_mobi_mfree(p)
#define strdup(s) eb_mobi_strdup(s)
#define fread eb_mobi_fread
#define fseek eb_mobi_fseek
#define ftell eb_mobi_ftell

#endif
