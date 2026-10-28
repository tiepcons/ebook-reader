#include "eb_sfnt.h"

#include <stdlib.h>
#include <string.h>

#define MAX_TABLES 256
#define MAX_TABLE_BYTES 65536
#define OS2_BYTES 64
#define HEAD_BYTES 48

static unsigned be16(const uint8_t* p) { return ((unsigned)p[0] << 8) | p[1]; }
static uint32_t be32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

typedef struct sfnt {
    eb_sfnt_read_fn rd;
    void* user;
    uint64_t size;
    uint8_t dir[MAX_TABLES * 16];
    unsigned ntab;
} sfnt;

int eb_sfnt_face_count(eb_sfnt_read_fn rd, void* user, uint64_t size)
{
    uint8_t h[12];
    uint32_t n = 0;
    if (size < sizeof h || !rd(user, 0, h, sizeof h)) return 0;
    if (memcmp(h, "ttcf", 4) != 0) return 1;
    n = be32(h + 8);
    return n > EB_SFNT_MAX_FACES ? EB_SFNT_MAX_FACES : (int)n;
}

static int face_offset(const sfnt* s, int index, uint64_t* off)
{
    uint8_t h[4];
    if (index < 0 || index >= EB_SFNT_MAX_FACES || s->size < sizeof h) return 0;
    if (!s->rd(s->user, 0, h, 4)) return 0;
    if (memcmp(h, "ttcf", 4) != 0) {
        *off = 0;
        return index == 0;
    }
    if (s->size < 16u + 4u * (uint64_t)index || !s->rd(s->user, 12u + 4u * (uint64_t)index, h, 4)) return 0;
    *off = be32(h);
    return 1;
}

static int load_directory(sfnt* s, uint64_t off)
{
    uint8_t h[12];
    if (off > s->size || s->size - off < sizeof h || !s->rd(s->user, off, h, sizeof h)) return 0;
    if (be32(h) != 0x00010000u && memcmp(h, "OTTO", 4) != 0 && memcmp(h, "true", 4) != 0) return 0;
    s->ntab = be16(h + 4);
    if (s->ntab == 0 || s->ntab > MAX_TABLES) return 0;
    if (s->size - off - sizeof h < (uint64_t)s->ntab * 16u) return 0;
    return s->rd(s->user, off + sizeof h, s->dir, (size_t)s->ntab * 16u);
}

/* Reads up to cap bytes of table tag into dst; returns the byte count, 0 if missing or out of bounds. */
static size_t read_table(const sfnt* s, const char* tag, uint8_t* dst, size_t cap)
{
    for (unsigned i = 0; i < s->ntab; ++i) {
        const uint8_t* e = s->dir + (size_t)i * 16u;
        uint32_t off = be32(e + 8), len = be32(e + 12);
        if (memcmp(e, tag, 4) != 0) continue;
        if (off > s->size || len > s->size - off || len == 0) return 0;
        if (len > cap) len = (uint32_t)cap;
        return s->rd(s->user, off, dst, len) ? len : 0;
    }
    return 0;
}

static int put_utf8(char* out, size_t cap, size_t* n, uint32_t cp)
{
    uint8_t b[4];
    size_t k = cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
    if (cp > 0x10FFFF || *n + k >= cap) return 0;
    if (k == 1) b[0] = (uint8_t)cp;
    else {
        for (size_t i = k - 1; i > 0; --i, cp >>= 6) b[i] = (uint8_t)(0x80 | (cp & 0x3F));
        b[0] = (uint8_t)((k == 2 ? 0xC0 : k == 3 ? 0xE0 : 0xF0) | cp);
    }
    memcpy(out + *n, b, k);
    *n += k;
    return 1;
}

static int decode_name(const uint8_t* p, size_t len, int utf16, char* out, size_t cap)
{
    size_t n = 0;
    if (utf16) {
        for (size_t i = 0; i + 1 < len; i += 2) {
            uint32_t cp = be16(p + i);
            if (cp >= 0xD800 && cp < 0xDC00 && i + 3 < len && be16(p + i + 2) >= 0xDC00 && be16(p + i + 2) < 0xE000) {
                cp = 0x10000u + ((cp - 0xD800u) << 10) + (be16(p + i + 2) - 0xDC00u);
                i += 2;
            } else if (cp >= 0xD800 && cp < 0xE000) return 0;
            if (cp < 0x20 || !put_utf8(out, cap, &n, cp)) return 0;
        }
    } else {
        for (size_t i = 0; i < len; ++i) {
            if (p[i] < 0x20 || p[i] >= 0x80 || n + 1 >= cap) return 0;
            out[n++] = (char)p[i];
        }
    }
    out[n] = '\0';
    return n > 0;
}

/* Prefers typographic family (ID 16) over family (ID 1), then Windows, Unicode and Macintosh platforms in that order. */
static int family_name(const uint8_t* t, size_t n, char* out, size_t cap)
{
    unsigned count = 0, str = 0;
    int best = -1;
    size_t best_off = 0, best_len = 0;
    int best_utf16 = 0;
    if (n < 6) return 0;
    count = be16(t + 2);
    str = be16(t + 4);
    for (unsigned i = 0; i < count && 6u + 12u * (size_t)(i + 1) <= n; ++i) {
        const uint8_t* r = t + 6 + 12u * (size_t)i;
        unsigned plat = be16(r), enc = be16(r + 2), id = be16(r + 6), len = be16(r + 8), off = be16(r + 10);
        int score = 0;
        if ((id != 16 && id != 1) || plat > 3 || plat == 2 || (plat == 3 && enc != 1 && enc != 10)) continue;
        if ((size_t)str + off + len > n || len == 0) continue;
        score = (id == 16 ? 4 : 0) + (plat == 3 ? 2 : plat == 0 ? 1 : 0);
        if (score > best) {
            best = score;
            best_off = (size_t)str + off;
            best_len = len;
            best_utf16 = plat != 1;
        }
    }
    return best >= 0 && decode_name(t + best_off, best_len, best_utf16, out, cap);
}

static void read_style(const sfnt* s, uint8_t* buf, eb_sfnt_face* out)
{
    size_t n = read_table(s, "OS/2", buf, OS2_BYTES);
    out->bold = out->italic = 0;
    if (n >= OS2_BYTES) {
        unsigned fs = be16(buf + 62);
        if (!(fs & 0x40)) {
            out->bold = (fs & 0x20) != 0;
            out->italic = (fs & 0x201) != 0;
        }
        return;
    }
    if (read_table(s, "head", buf, HEAD_BYTES) >= HEAD_BYTES) {
        unsigned mac = be16(buf + 44);
        out->bold = (mac & 1) != 0;
        out->italic = (mac & 2) != 0;
    }
}

int eb_sfnt_probe(eb_sfnt_read_fn rd, void* user, uint64_t size, int index, eb_sfnt_face* out)
{
    sfnt* s = calloc(1, sizeof *s);
    uint8_t* buf = malloc(MAX_TABLE_BYTES);
    uint64_t off = 0;
    int ok = 0;
    if (s && buf) {
        s->rd = rd;
        s->user = user;
        s->size = size;
        if (face_offset(s, index, &off) && load_directory(s, off)) {
            size_t n = read_table(s, "name", buf, MAX_TABLE_BYTES);
            ok = n > 0 && family_name(buf, n, out->family, sizeof out->family);
            if (ok) read_style(s, buf, out);
        }
    }
    free(buf);
    free(s);
    return ok;
}
