/* Kindle/MOBI support: converts AZW3/KF8/MOBI buffers to an in-memory EPUB with libmobi (LGPL-3.0).
 * The EPUB assembly is adapted from create_epub() in libmobi's tools/mobitool.c (Bartek Fabiszewski, LGPL). */
#include "eb_internal.h"

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

#define EPUB_MIMETYPE "application/epub+zip"
#define EPUB_CONTAINER                                                                                               \
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"                                                                  \
    "<container version=\"1.0\" xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">\n"                       \
    "  <rootfiles>\n"                                                                                                \
    "    <rootfile full-path=\"OEBPS/content.opf\" media-type=\"application/oebps-package+xml\"/>\n"                 \
    "  </rootfiles>\n"                                                                                               \
    "</container>"

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

static int is_image(const uint8_t* d, size_t n)
{
    return n >= 8 && (memcmp(d, "\xff\xd8\xff", 3) == 0 || memcmp(d, "GIF8", 4) == 0 ||
                      memcmp(d, "\x89PNG\r\n\x1a\n", 8) == 0 || memcmp(d, "BM", 2) == 0);
}

static void read_cover(const MOBIData* m, eb_mobi_info* info)
{
    const MOBIExthHeader* exth = mobi_get_exthrecord_by_tag(m, EXTH_COVEROFFSET);
    const MOBIPdbRecord* rec = NULL;
    if (!exth) return;
    rec = mobi_get_record_by_seqnumber(m, mobi_get_first_resource_record(m) + mobi_decode_exthvalue(exth->data, exth->size));
    if (!rec || !rec->data || rec->size > EB_MOBI_MAX_COVER || !is_image(rec->data, rec->size)) return;
    info->cover = malloc(rec->size);
    if (!info->cover) return;
    memcpy(info->cover, rec->data, rec->size);
    info->cover_size = rec->size;
}

static void read_info(const MOBIData* m, eb_mobi_info* info)
{
    info->title = mobi_meta_get_title(m);
    info->author = mobi_meta_get_author(m);
    info->language = mobi_meta_get_language(m);
    info->format = mobi_is_kf8(m) ? "AZW3" : "MOBI";
    read_cover(m, info);
}

static int add_part(mz_zip_archive* zip, const char* name, const void* data, size_t size, size_t* total, size_t cap)
{
    if (size > cap || *total > cap - size) return 0;
    *total += size;
    return mz_zip_writer_add_mem(zip, name, data, size, (mz_uint)MZ_DEFAULT_COMPRESSION) != 0;
}

static int add_list(mz_zip_archive* zip, const MOBIPart* p, const char* prefix, size_t* total, size_t cap)
{
    char name[64];
    for (; p; p = p->next) {
        const MOBIFileMeta meta = mobi_get_filemeta_by_type(p->type);
        if (p->size == 0) continue;
        if (meta.type == T_OPF) snprintf(name, sizeof name, "OEBPS/content.opf");
        else snprintf(name, sizeof name, "OEBPS/%s%05zu.%s", prefix, p->uid, meta.extension);
        if (!add_part(zip, name, p->data, p->size, total, cap)) return 0;
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

static eb_status build_epub(const MOBIRawml* rawml, uint8_t** out, size_t* out_size)
{
    mz_zip_archive zip;
    size_t total = 0;
    const size_t cap = EB_MOBI_MAX_EPUB;
    void* buf = NULL;
    size_t n = 0;
    memset(&zip, 0, sizeof zip);
    if (!has_opf(rawml)) return eb_fail_msg(EB_E_FORMAT, "libmobi produced no OPF");
    if (!mz_zip_writer_init_heap(&zip, 0, 1u << 16)) return eb_fail_msg(EB_E_NOMEM, "cannot start epub");
    if (!mz_zip_writer_add_mem(&zip, "mimetype", EPUB_MIMETYPE, sizeof(EPUB_MIMETYPE) - 1, MZ_NO_COMPRESSION) ||
        !mz_zip_writer_add_mem(&zip, "META-INF/container.xml", EPUB_CONTAINER, sizeof(EPUB_CONTAINER) - 1,
                               (mz_uint)MZ_DEFAULT_COMPRESSION) ||
        !add_list(&zip, rawml->markup, "part", &total, cap) ||
        !add_list(&zip, rawml->flow ? rawml->flow->next : NULL, "flow", &total, cap) ||
        !add_list(&zip, rawml->resources, "resource", &total, cap) ||
        !mz_zip_writer_finalize_heap_archive(&zip, &buf, &n)) {
        mz_zip_writer_end(&zip);
        return total >= cap ? eb_fail_msg(EB_E_LIMIT, "converted book too large") : eb_fail_msg(EB_E_FORMAT, "epub assembly failed");
    }
    *out = buf;
    *out_size = n;
    return EB_OK;
}

static eb_status parse_to_epub(MOBIData* m, uint8_t** out, size_t* out_size)
{
    MOBIRawml* rawml = NULL;
    MOBI_RET ret = MOBI_SUCCESS;
    eb_status st = EB_OK;
    if (mobi_is_replica(m)) return eb_fail_msg(EB_E_FORMAT, "print replica books are not supported");
    rawml = mobi_init_rawml(m);
    if (!rawml) return eb_fail_msg(EB_E_NOMEM, "out of memory");
    ret = mobi_parse_rawml(rawml, m);
    if (ret == MOBI_FILE_ENCRYPTED || ret == MOBI_DRM_KEYNOTFOUND) st = eb_fail_msg(EB_E_DRM, "document is DRM protected");
    else if (ret == MOBI_MALLOC_FAILED) st = eb_fail_msg(EB_E_NOMEM, "out of memory");
    else if (ret != MOBI_SUCCESS) st = eb_fail_msg(EB_E_FORMAT, "libmobi could not parse the book");
    if (st == EB_OK) st = build_epub(rawml, out, out_size);
    mobi_free_rawml(rawml);
    return st;
}

eb_status eb_mobi_convert(const uint8_t* data, size_t size, uint8_t** epub, size_t* epub_size, eb_mobi_info* info)
{
    memfile mf = {data, size, 0};
    MOBIData* m = NULL;
    MOBI_RET ret = MOBI_SUCCESS;
    eb_status st = EB_OK;
    *epub = NULL;
    *epub_size = 0;
    if (size > EB_MOBI_MAX_INPUT) return eb_fail_msg(EB_E_LIMIT, "book too large for conversion");
    if (!pdb_valid(data, size)) return eb_fail_msg(EB_E_FORMAT, "invalid PalmDB header");
    m = mobi_init();
    if (!m) return eb_fail_msg(EB_E_NOMEM, "out of memory");
    ret = mobi_load_file(m, (FILE*)&mf);
    if (ret == MOBI_MALLOC_FAILED) st = eb_fail_msg(EB_E_NOMEM, "out of memory");
    else if (ret != MOBI_SUCCESS) st = eb_fail_msg(EB_E_FORMAT, "libmobi could not load the book");
    else if (mobi_is_encrypted(m)) st = eb_fail_msg(EB_E_DRM, "document is DRM protected");
    if (st == EB_OK) st = parse_to_epub(m, epub, epub_size);
    if (st == EB_OK) read_info(m, info);
    mobi_free(m);
    return st;
}

void eb_mobi_free(void* p) { free(p); }
