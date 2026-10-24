#ifndef EB_SFNT_H
#define EB_SFNT_H

#include <stddef.h>
#include <stdint.h>

#define EB_SFNT_MAX_FACES 16
#define EB_SFNT_FAMILY_BYTES 96

/* Reads len bytes at byte offset off into dst; returns 1 on success. The parser never asks for bytes at or past size. */
typedef int (*eb_sfnt_read_fn)(void* user, uint64_t off, void* dst, size_t len);

typedef struct eb_sfnt_face {
    char family[EB_SFNT_FAMILY_BYTES]; /* UTF-8, NUL-terminated */
    int bold, italic;
} eb_sfnt_face;

/* Number of faces in a font file: 1 for a plain font, 1..EB_SFNT_MAX_FACES for a collection, 0 if unreadable. */
int eb_sfnt_face_count(eb_sfnt_read_fn rd, void* user, uint64_t size);

/* Family and style of one face, read from the table directory plus the name, OS/2 and head tables only. 1 on success. */
int eb_sfnt_probe(eb_sfnt_read_fn rd, void* user, uint64_t size, int index, eb_sfnt_face* out);

#endif
