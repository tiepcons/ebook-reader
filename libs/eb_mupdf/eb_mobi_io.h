/* Force-included into libmobi's read.c only: redirects its FILE* reads to an in-memory buffer (eb_mobi.c), so
 * documents are parsed without temp files. The FILE* handed to libmobi is really an eb_mobi memfile. */
#ifndef EB_MOBI_IO_H
#define EB_MOBI_IO_H

#include <stdio.h>

size_t eb_mobi_fread(void* buf, size_t size, size_t count, FILE* file);
int eb_mobi_fseek(FILE* file, long offset, int whence);
long eb_mobi_ftell(FILE* file);

#define fread eb_mobi_fread
#define fseek eb_mobi_fseek
#define ftell eb_mobi_ftell

#endif
