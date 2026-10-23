/* Kindle/MOBI support: converts AZW3/KF8/MOBI buffers to an in-memory EPUB with libmobi (LGPL-3.0).
 * The EPUB assembly is adapted from create_epub() in libmobi's tools/mobitool.c (Bartek Fabiszewski, LGPL). */
#include "eb_internal.h"

#include <assert.h>
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>

#define MINIZ_HEADER_FILE_ONLY
#define MINIZ_NO_STDIO
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include <miniz.c>

#include <mobi.h>

#define PDB_HEADER_BYTES 78u
#define PDB_RECORD_BYTES 8u
#define EB_MOBI_MAX_COVER (32u << 20)
#define EB_MOBI_MAX_BUDGET ((uint64_t)768 << 20)
#define ARENA_HEADER 32u

#define EPUB_MIMETYPE "application/epub+zip"
#define EPUB_CONTAINER                                                                                               \
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"                                                                  \
    "<container version=\"1.0\" xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">\n"                       \
    "  <rootfiles>\n"                                                                                                \
    "    <rootfile full-path=\"OEBPS/content.opf\" media-type=\"application/oebps-package+xml\"/>\n"                 \
    "  </rootfiles>\n"                                                                                               \
    "</container>"

typedef struct arena_block {
    struct arena_block *prev, *next;
    size_t size;
    size_t pad;
} arena_block;

static __declspec(thread) struct {
    arena_block* head;
    size_t used, limit;
    jmp_buf* env;
} t_arena;

/* libmobi may only run between arena_begin and arena_end (eb_mobi_convert). Allocating outside that window is a
 * programming error: there is no jump target, so abort() (keeping the crash dump) rather than return NULL. */
static void arena_bail(void)
{
    assert(t_arena.env != NULL && "libmobi called outside eb_mobi_convert");
    if (t_arena.env) longjmp(*t_arena.env, 1);
    abort();
}

void* eb_mobi_malloc(size_t size)
{
    arena_block* b = NULL;
    if (size > t_arena.limit - t_arena.used) arena_bail();
    b = malloc(ARENA_HEADER + size);
    if (!b) arena_bail();
    b->size = size;
    b->prev = NULL;
    b->next = t_arena.head;
    if (t_arena.head) t_arena.head->prev = b;
    t_arena.head = b;
    t_arena.used += size;
    return (char*)b + ARENA_HEADER;
}

void eb_mobi_mfree(void* ptr)
{
    arena_block* b = NULL;
    if (!ptr) return;
    b = (arena_block*)((char*)ptr - ARENA_HEADER);
    if (b->prev) b->prev->next = b->next;
    else t_arena.head = b->next;
    if (b->next) b->next->prev = b->prev;
    t_arena.used -= b->size;
    free(b);
}

void* eb_mobi_calloc(size_t count, size_t size)
{
    void* p = NULL;
    if (size && count > SIZE_MAX / size) arena_bail();
    p = eb_mobi_malloc(count * size);
    memset(p, 0, count * size);
    return p;
}

void* eb_mobi_realloc(void* ptr, size_t size)
{
    void* fresh = NULL;
    size_t old = 0;
    if (!ptr) return eb_mobi_malloc(size);
    old = ((arena_block*)((char*)ptr - ARENA_HEADER))->size;
    fresh = eb_mobi_malloc(size);
    memcpy(fresh, ptr, old < size ? old : size);
    eb_mobi_mfree(ptr);
    return fresh;
}

char* eb_mobi_strdup(const char* s)
{
    const size_t n = strlen(s) + 1;
    return memcpy(eb_mobi_malloc(n), s, n);
}

static void arena_begin(jmp_buf* env, size_t limit)
{
    t_arena.head = NULL;
    t_arena.used = 0;
    t_arena.limit = limit;
    t_arena.env = env;
}

static void arena_end(void)
{
    arena_block* b = t_arena.head;
    while (b) {
        arena_block* next = b->next;
        free(b);
        b = next;
    }
    memset(&t_arena, 0, sizeof t_arena);
}

typedef struct memfile {
    const uint8_t* data;
    size_t size, pos;
} memfile;

size_t eb_mobi_fread(void* buf, size_t size, size_t count, FILE* file)
{
    memfile* m = (memfile*)file;
    size_t avail = 0;
    if (!size || !count) return 0;
    avail = (m->size - m->pos) / size;
    if (count > avail) count = avail;
    memcpy(buf, m->data + m->pos, count * size);
    m->pos += count * size;
    return count;
}

int eb_mobi_fseek(FILE* file, long offset, int whence)
{
    memfile* m = (memfile*)file;
    long long base = whence == SEEK_SET ? 0 : whence == SEEK_END ? (long long)m->size : (long long)m->pos;
    long long target = base + offset;
    if (whence != SEEK_SET && whence != SEEK_END && whence != SEEK_CUR) return -1;
    if (target < 0 || (unsigned long long)target > m->size) return -1;
    m->pos = (size_t)target;
    return 0;
}

long eb_mobi_ftell(FILE* file) { return (long)((memfile*)file)->pos; }

static uint32_t be32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

int eb_mobi_sniff(const uint8_t* head, size_t size)
{
    return size >= 68 && memcmp(head + 60, "BOOKMOBI", 8) == 0;
}

/* libmobi derives record sizes from offset differences; reject tables that would make them wrap or exceed the file. */
static int pdb_valid(const uint8_t* d, size_t size)
{
    size_t count = 0;
    uint32_t prev = 0;
    if (size < PDB_HEADER_BYTES || !eb_mobi_sniff(d, size)) return 0;
    count = ((size_t)d[76] << 8) | d[77];
    if (count == 0 || PDB_HEADER_BYTES + count * PDB_RECORD_BYTES > size) return 0;
    prev = (uint32_t)(PDB_HEADER_BYTES + count * PDB_RECORD_BYTES);
    for (size_t i = 0; i < count; ++i) {
        uint32_t off = be32(d + PDB_HEADER_BYTES + i * PDB_RECORD_BYTES);
        if (off < prev || off >= size) return 0;
        prev = off;
    }
    return 1;
}

void eb_mobi_info_free(eb_mobi_info* info)
{
    if (!info) return;
    free(info->title);
    free(info->author);
    free(info->language);
    free(info->cover);
    memset(info, 0, sizeof *info);
}

static char* dup_plain(const char* s)
{
    size_t n = 0;
    char* d = NULL;
    if (!s || !*s) return NULL;
    n = strlen(s) + 1;
    d = malloc(n);
    return d ? memcpy(d, s, n) : NULL;
}

static int is_image(const uint8_t* d, size_t n)
{
    return n >= 8 && (memcmp(d, "\xff\xd8\xff", 3) == 0 || memcmp(d, "GIF8", 4) == 0 ||
                      memcmp(d, "\x89PNG\r\n\x1a\n", 8) == 0 || memcmp(d, "BM", 2) == 0);
}

static void read_cover(const MOBIData* m, eb_mobi_info* info)
{
    const MOBIExthHeader* exth = mobi_get_exthrecord_by_tag(m, EXTH_COVEROFFSET);
    const MOBIPdbRecord* rec = NULL;
    size_t first = 0;
    uint32_t off = 0;
    if (!exth || !m->ph) return;
    first = mobi_get_first_resource_record(m);
    off = mobi_decode_exthvalue(exth->data, exth->size);
    if (first == MOBI_NOTSET || off == MOBI_NOTSET || first + off >= m->ph->rec_count) return;
    rec = mobi_get_record_by_seqnumber(m, first + off);
    if (!rec || !rec->data || rec->size > EB_MOBI_MAX_COVER || !is_image(rec->data, rec->size)) return;
    info->cover = malloc(rec->size);
    if (!info->cover) return;
    memcpy(info->cover, rec->data, rec->size);
    info->cover_size = rec->size;
}

static void read_info(const MOBIData* m, eb_mobi_info* info)
{
    char* s = mobi_meta_get_title(m);
    info->title = dup_plain(s);
    s = mobi_meta_get_author(m);
    info->author = dup_plain(s);
    s = mobi_meta_get_language(m);
    info->language = eb_normalize_language(s);
    info->format = mobi_is_kf8(m) ? "AZW3" : "MOBI";
    read_cover(m, info);
}

static void* zip_alloc(void* opaque, size_t items, size_t size)
{
    return size && items > SIZE_MAX / size ? NULL : fz_malloc_no_throw((fz_context*)opaque, items * size);
}

static void zip_free(void* opaque, void* address) { fz_free((fz_context*)opaque, address); }

static void* zip_realloc(void* opaque, void* address, size_t items, size_t size)
{
    return size && items > SIZE_MAX / size ? NULL : fz_realloc_no_throw((fz_context*)opaque, address, items * size);
}

typedef struct epub_writer {
    mz_zip_archive zip;
    size_t total, cap;
    int limit_hit;
} epub_writer;

static int add_part(epub_writer* w, const char* name, const void* data, size_t size)
{
    if (size > w->cap || w->total > w->cap - size) {
        w->limit_hit = 1;
        return 0;
    }
    w->total += size;
    return mz_zip_writer_add_mem(&w->zip, name, data, size, MZ_NO_COMPRESSION) != 0;
}

static int add_list(epub_writer* w, const MOBIPart* p, const char* prefix)
{
    char name[64];
    for (; p; p = p->next) {
        const MOBIFileMeta meta = mobi_get_filemeta_by_type(p->type);
        if (p->size == 0) continue;
        if (meta.type == T_OPF) snprintf(name, sizeof name, "OEBPS/content.opf");
        else snprintf(name, sizeof name, "OEBPS/%s%05zu.%s", prefix, p->uid, meta.extension);
        if (!add_part(w, name, p->data, p->size)) return 0;
    }
    return 1;
}

static int has_opf(const MOBIRawml* rawml)
{
    for (const MOBIPart* p = rawml->resources; p; p = p->next) {
        if (p->size > 0 && mobi_get_filemeta_by_type(p->type).type == T_OPF) return 1;
    }
    return 0;
}

static eb_status build_epub(fz_context* ctx, const MOBIRawml* rawml, fz_buffer** out)
{
    epub_writer w;
    void* buf = NULL;
    size_t n = 0;
    memset(&w, 0, sizeof w);
    w.cap = EB_MOBI_MAX_EPUB;
    if (!has_opf(rawml)) return eb_fail_msg(EB_E_FORMAT, "libmobi produced no OPF");
    w.zip.m_pAlloc = zip_alloc;
    w.zip.m_pFree = zip_free;
    w.zip.m_pRealloc = zip_realloc;
    w.zip.m_pAlloc_opaque = ctx;
    if (!mz_zip_writer_init_heap(&w.zip, 0, 1u << 16)) return eb_fail_msg(EB_E_NOMEM, "cannot start epub");
    if (!add_part(&w, "mimetype", EPUB_MIMETYPE, sizeof(EPUB_MIMETYPE) - 1) ||
        !add_part(&w, "META-INF/container.xml", EPUB_CONTAINER, sizeof(EPUB_CONTAINER) - 1) ||
        !add_list(&w, rawml->markup, "part") || !add_list(&w, rawml->flow ? rawml->flow->next : NULL, "flow") ||
        !add_list(&w, rawml->resources, "resource") || !mz_zip_writer_finalize_heap_archive(&w.zip, &buf, &n)) {
        mz_zip_writer_end(&w.zip);
        return w.limit_hit ? eb_fail_msg(EB_E_LIMIT, "converted book too large") : eb_fail_msg(EB_E_FORMAT, "epub assembly failed");
    }
    mz_zip_writer_end(&w.zip);
    fz_try(ctx) * out = fz_new_buffer_from_data(ctx, buf, n);
    fz_catch(ctx) return eb_fail(ctx);
    return EB_OK;
}

static eb_status parse_to_epub(fz_context* ctx, MOBIData* m, fz_buffer** out)
{
    MOBIRawml* rawml = NULL;
    MOBI_RET ret = MOBI_SUCCESS;
    if (mobi_is_replica(m)) return eb_fail_msg(EB_E_FORMAT, "print replica books are not supported");
    rawml = mobi_init_rawml(m);
    if (!rawml) return eb_fail_msg(EB_E_NOMEM, "out of memory");
    ret = mobi_parse_rawml(rawml, m);
    if (ret == MOBI_FILE_ENCRYPTED || ret == MOBI_DRM_KEYNOTFOUND) return eb_fail_msg(EB_E_DRM, "document is DRM protected");
    if (ret == MOBI_MALLOC_FAILED) return eb_fail_msg(EB_E_NOMEM, "out of memory");
    if (ret != MOBI_SUCCESS) return eb_fail_msg(EB_E_FORMAT, "libmobi could not parse the book");
    return build_epub(ctx, rawml, out);
}

static eb_status convert_inner(fz_context* ctx, const uint8_t* data, size_t size, fz_buffer** epub, eb_mobi_info* info)
{
    memfile mf = {data, size, 0};
    MOBIData* m = mobi_init();
    MOBI_RET ret = MOBI_SUCCESS;
    eb_status st = EB_OK;
    if (!m) return eb_fail_msg(EB_E_NOMEM, "out of memory");
    ret = mobi_load_file(m, (FILE*)&mf);
    if (ret == MOBI_MALLOC_FAILED) return eb_fail_msg(EB_E_NOMEM, "out of memory");
    if (ret != MOBI_SUCCESS) return eb_fail_msg(EB_E_FORMAT, "libmobi could not load the book");
    if (mobi_is_encrypted(m)) return eb_fail_msg(EB_E_DRM, "document is DRM protected");
    st = eb_mobi_precheck(m, size);
    if (st != EB_OK) {
        info->rejected = 1;
        return st;
    }
    st = parse_to_epub(ctx, m, epub);
    if (st == EB_OK) read_info(m, info);
    return st;
}

/* Everything libmobi allocates lives in the arena and is released by arena_end, so no mobi_free_* calls are made. */
eb_status eb_mobi_convert(fz_context* ctx, const uint8_t* data, size_t size, fz_buffer** epub, eb_mobi_info* info)
{
    jmp_buf env;
    eb_status st = EB_OK;
    uint64_t budget = eb_g_limits.max_heap_bytes < EB_MOBI_MAX_BUDGET ? eb_g_limits.max_heap_bytes : EB_MOBI_MAX_BUDGET;
    *epub = NULL;
    if (size > EB_MOBI_MAX_INPUT) return eb_fail_msg(EB_E_LIMIT, "book too large for conversion");
    if (!pdb_valid(data, size)) return eb_fail_msg(EB_E_FORMAT, "invalid PalmDB header");
    arena_begin(&env, (size_t)budget);
    if (setjmp(env) == 0) st = convert_inner(ctx, data, size, epub, info);
    else st = eb_fail_msg(EB_E_NOMEM, "libmobi memory budget exceeded");
    arena_end();
    if (st != EB_OK && *epub) {
        fz_drop_buffer(ctx, *epub);
        *epub = NULL;
    }
    return st;
}
