/* Metadata (language, fallback title/author) and embedded cover for EPUB and FB2, read straight from the document
 * bytes: the open FILE* is re-opened through its own handle (independent file pointer, never by name) or the kept
 * memory buffer. All XML goes through fz_parse_xml under fz_try; sizes are bounded. */
#include "eb_internal.h"

#include <fcntl.h>
#include <io.h>
#include <string.h>
#include <windows.h>

#define EB_MAX_XML_BYTES ((size_t)4 << 20)
#define EB_MAX_IMAGE_BYTES ((size_t)32 << 20)
#define EB_MAX_FB2_BYTES ((size_t)64 << 20)
#define EB_PATH_MAX 1024
#define EB_TEXT_MAX 512

static char* dup_n(const char* s, size_t n)
{
    char* d = malloc(n + 1);
    if (!d) return NULL;
    memcpy(d, s, n);
    d[n] = '\0';
    return d;
}

static char* dup_trimmed(const char* s)
{
    size_t n = 0;
    if (!s) return NULL;
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') ++s;
    n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n')) --n;
    return n && n <= EB_TEXT_MAX ? dup_n(s, n) : NULL;
}

char* eb_normalize_language(const char* s)
{
    char tag[4];
    size_t n = 0;
    if (!s) return NULL;
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') ++s;
    for (; s[n] && s[n] != '-' && s[n] != '_' && n < 4; ++n) {
        const char c = s[n];
        if (c >= 'A' && c <= 'Z') tag[n] = (char)(c - 'A' + 'a');
        else if (c >= 'a' && c <= 'z') tag[n] = c;
        else return NULL;
    }
    if (n < 2 || n > 3) return NULL;
    return dup_n(tag, n);
}

static const char* child_text(fz_xml* parent, const char* tag) { return fz_xml_text(fz_xml_down(fz_xml_find_down(parent, tag))); }

static void set_if_empty(char** slot, char* value)
{
    if (!value) return;
    if (*slot) free(value);
    else *slot = value;
}

/* Joins "dir" and a manifest href, decoding %XX; rejects anything that climbs above the archive root. */
static int resolve_path(const char* dir, const char* href, char* out, size_t cap)
{
    char raw[EB_PATH_MAX];
    size_t n = 0, len = 0;
    const char* seg = NULL;
    if (strlen(dir) + strlen(href) + 2 > sizeof raw) return 0;
    snprintf(raw, sizeof raw, "%s%s", dir, href);
    for (const char* p = raw; *p && *p != '#' && *p != '?'; ++p) {
        if (*p == '\\') return 0;
        if (*p == '%' && p[1] && p[2]) {
            const char hex[3] = {p[1], p[2], 0};
            char* end = NULL;
            const long v = strtol(hex, &end, 16);
            if (*end || v <= 0 || v == '\\') return 0;
            raw[n++] = (char)v;
            p += 2;
        } else {
            raw[n++] = *p;
        }
    }
    raw[n] = '\0';
    if (n == 0 || raw[0] == '/') return 0;
    out[0] = '\0';
    for (seg = raw; seg;) {
        const char* slash = strchr(seg, '/');
        const size_t sl = slash ? (size_t)(slash - seg) : strlen(seg);
        if (sl == 2 && seg[0] == '.' && seg[1] == '.') {
            if (len == 0) return 0;
            --len;
            while (len > 0 && out[len - 1] != '/') --len;
            out[len] = '\0';
        } else if (sl > 0 && !(sl == 1 && seg[0] == '.')) {
            if (len + sl + 2 > cap) return 0;
            memcpy(out + len, seg, sl);
            len += sl;
            out[len++] = '/';
            out[len] = '\0';
        }
        seg = slash ? slash + 1 : NULL;
    }
    if (len == 0) return 0;
    out[len - 1] = '\0';
    return 1;
}

static void dir_of(const char* path, char* out, size_t cap)
{
    const char* slash = strrchr(path, '/');
    const size_t n = slash ? (size_t)(slash - path) + 1 : 0;
    if (n >= cap) {
        out[0] = '\0';
        return;
    }
    memcpy(out, path, n);
    out[n] = '\0';
}

static fz_buffer* read_entry(fz_context* ctx, fz_archive* arch, const char* name, size_t limit)
{
    fz_stream* s = fz_try_open_archive_entry(ctx, arch, name);
    fz_buffer* buf = NULL;
    int truncated = 0;
    if (!s) return NULL;
    fz_try(ctx) buf = fz_read_best(ctx, s, 16384, &truncated, limit);
    fz_always(ctx) fz_drop_stream(ctx, s);
    fz_catch(ctx) fz_rethrow(ctx);
    if (truncated) {
        fz_drop_buffer(ctx, buf);
        return NULL;
    }
    return buf;
}

static int has_token(const char* list, const char* token)
{
    const size_t n = strlen(token);
    for (const char* p = list; p && *p;) {
        while (*p == ' ') ++p;
        if (strncmp(p, token, n) == 0 && (p[n] == ' ' || p[n] == '\0')) return 1;
        while (*p && *p != ' ') ++p;
    }
    return 0;
}

static int contains_nocase(const char* s, const char* needle)
{
    const size_t n = strlen(needle);
    for (; s && *s; ++s) {
        if (_strnicmp(s, needle, n) == 0) return 1;
    }
    return 0;
}

static int is_image_item(fz_xml* item)
{
    const char* type = fz_xml_att(item, "media-type");
    return type && _strnicmp(type, "image/", 6) == 0;
}

static const char* find_cover_href(fz_xml* package)
{
    fz_xml* manifest = fz_xml_find_down(package, "manifest");
    fz_xml* metadata = fz_xml_find_down(package, "metadata");
    const char* id = NULL;
    for (fz_xml* it = fz_xml_find_down(manifest, "item"); it; it = fz_xml_find_next(it, "item")) {
        if (has_token(fz_xml_att(it, "properties"), "cover-image") && fz_xml_att(it, "href")) return fz_xml_att(it, "href");
    }
    for (fz_xml* m = fz_xml_find_down(metadata, "meta"); m && !id; m = fz_xml_find_next(m, "meta")) {
        if (fz_xml_att_eq(m, "name", "cover")) id = fz_xml_att(m, "content");
    }
    for (fz_xml* it = fz_xml_find_down(manifest, "item"); id && it; it = fz_xml_find_next(it, "item")) {
        if (fz_xml_att_eq(it, "id", id) && fz_xml_att(it, "href")) return fz_xml_att(it, "href");
    }
    for (fz_xml* it = fz_xml_find_down(manifest, "item"); it; it = fz_xml_find_next(it, "item")) {
        const char* href = fz_xml_att(it, "href");
        if (href && is_image_item(it) && (contains_nocase(fz_xml_att(it, "id"), "cover") || contains_nocase(href, "cover"))) return href;
    }
    return NULL;
}

static void take_cover(fz_context* ctx, eb_mobi_info* info, fz_buffer* buf)
{
    unsigned char* data = NULL;
    const size_t n = fz_buffer_storage(ctx, buf, &data);
    if (n == 0 || n > EB_MAX_IMAGE_BYTES || info->cover) return;
    info->cover = malloc(n);
    if (!info->cover) return;
    memcpy(info->cover, data, n);
    info->cover_size = n;
}

static void epub_cover(fz_context* ctx, fz_archive* arch, fz_xml* package, const char* opf_dir, eb_mobi_info* info)
{
    const char* href = find_cover_href(package);
    char path[EB_PATH_MAX];
    fz_buffer* img = NULL;
    if (!href || !resolve_path(opf_dir, href, path, sizeof path)) return;
    img = read_entry(ctx, arch, path, EB_MAX_IMAGE_BYTES);
    if (!img) return;
    fz_try(ctx) take_cover(ctx, info, img);
    fz_always(ctx) fz_drop_buffer(ctx, img);
    fz_catch(ctx) fz_rethrow(ctx);
}

static void epub_meta(fz_xml* package, eb_mobi_info* info)
{
    fz_xml* metadata = fz_xml_find_down(package, "metadata");
    set_if_empty(&info->title, dup_trimmed(child_text(metadata, "title")));
    set_if_empty(&info->author, dup_trimmed(child_text(metadata, "creator")));
    set_if_empty(&info->language, eb_normalize_language(child_text(metadata, "language")));
}

static void epub_probe(fz_context* ctx, fz_archive* arch, int want_cover, eb_mobi_info* info)
{
    fz_buffer* buf = read_entry(ctx, arch, "META-INF/container.xml", EB_MAX_XML_BYTES);
    fz_xml* xml = NULL;
    char opf[EB_PATH_MAX];
    char dir[EB_PATH_MAX];
    int have = 0;
    if (!buf) return;
    fz_var(xml);
    fz_try(ctx)
    {
        const char* full = NULL;
        xml = fz_parse_xml(ctx, buf, 0);
        full = fz_xml_att(fz_xml_find_down(fz_xml_find_down(fz_xml_find(fz_xml_root(xml), "container"), "rootfiles"), "rootfile"), "full-path");
        have = full && resolve_path("", full, opf, sizeof opf);
    }
    fz_always(ctx)
    {
        fz_drop_xml(ctx, xml);
        fz_drop_buffer(ctx, buf);
    }
    fz_catch(ctx) fz_rethrow(ctx);
    if (!have) return;
    dir_of(opf, dir, sizeof dir);
    buf = read_entry(ctx, arch, opf, EB_MAX_XML_BYTES);
    if (!buf) return;
    xml = NULL;
    fz_try(ctx)
    {
        fz_xml* package = NULL;
        xml = fz_parse_xml(ctx, buf, 0);
        package = fz_xml_find(fz_xml_root(xml), "package");
        epub_meta(package, info);
        if (want_cover) epub_cover(ctx, arch, package, dir, info);
    }
    fz_always(ctx)
    {
        fz_drop_xml(ctx, xml);
        fz_drop_buffer(ctx, buf);
    }
    fz_catch(ctx) fz_rethrow(ctx);
}

static int b64_value(int c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static uint8_t* b64_decode(const char* in, size_t* out_size)
{
    const size_t cap = strlen(in) / 4 * 3 + 3;
    uint8_t* out = NULL;
    uint32_t acc = 0;
    int bits = 0;
    size_t n = 0;
    if (cap > EB_MAX_IMAGE_BYTES + 3) return NULL;
    out = malloc(cap ? cap : 1);
    if (!out) return NULL;
    for (; *in; ++in) {
        const int v = b64_value((unsigned char)*in);
        if (v < 0) {
            if (*in == '=' || *in == ' ' || *in == '\n' || *in == '\r' || *in == '\t') continue;
            free(out);
            return NULL;
        }
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[n++] = (uint8_t)((acc >> bits) & 0xFFu);
        }
    }
    *out_size = n;
    return out;
}

static char* fb2_author(fz_xml* ti)
{
    fz_xml* a = fz_xml_find_down(ti, "author");
    const char* first = child_text(a, "first-name");
    const char* last = child_text(a, "last-name");
    char joined[EB_TEXT_MAX + 2];
    snprintf(joined, sizeof joined, "%s%s%s", first ? first : "", first && last ? " " : "", last ? last : "");
    return dup_trimmed(joined);
}

static void fb2_cover(fz_xml* root, fz_xml* ti, eb_mobi_info* info)
{
    fz_xml* img = fz_xml_find_down(fz_xml_find_down(ti, "coverpage"), "image");
    const char* href = fz_xml_att_alt(img, "l:href", "xlink:href");
    if (!href) href = fz_xml_att(img, "href");
    if (!href || href[0] != '#') return;
    for (fz_xml* b = fz_xml_find_down(root, "binary"); b; b = fz_xml_find_next(b, "binary")) {
        const char* text = NULL;
        if (!fz_xml_att_eq(b, "id", href + 1)) continue;
        text = fz_xml_text(fz_xml_down(b));
        if (text) info->cover = b64_decode(text, &info->cover_size);
        return;
    }
}

static void fb2_probe(fz_context* ctx, fz_buffer* buf, int want_cover, eb_mobi_info* info)
{
    fz_xml* xml = NULL;
    fz_var(xml);
    fz_try(ctx)
    {
        fz_xml* root = NULL;
        fz_xml* ti = NULL;
        xml = fz_parse_xml(ctx, buf, 0);
        root = fz_xml_find(fz_xml_root(xml), "FictionBook");
        ti = fz_xml_find_down(fz_xml_find_down(root, "description"), "title-info");
        set_if_empty(&info->title, dup_trimmed(child_text(ti, "book-title")));
        set_if_empty(&info->author, fb2_author(ti));
        set_if_empty(&info->language, eb_normalize_language(child_text(ti, "lang")));
        if (want_cover) fb2_cover(root, ti, info);
    }
    fz_always(ctx) fz_drop_xml(ctx, xml);
    fz_catch(ctx) fz_rethrow(ctx);
}

static fz_stream* open_source(eb_doc* d, FILE** aux)
{
    fz_context* ctx = d->ctx;
    *aux = NULL;
    if (d->file) {
        const HANDLE h = (HANDLE)_get_osfhandle(_fileno(d->file));
        const HANDLE again = h == INVALID_HANDLE_VALUE ? INVALID_HANDLE_VALUE : ReOpenFile(h, GENERIC_READ, FILE_SHARE_READ, 0);
        int fd = -1;
        if (again == INVALID_HANDLE_VALUE) return NULL;
        fd = _open_osfhandle((intptr_t)again, _O_RDONLY | _O_BINARY);
        if (fd < 0) {
            CloseHandle(again);
            return NULL;
        }
        *aux = _fdopen(fd, "rb");
        if (!*aux) {
            _close(fd);
            return NULL;
        }
        return fz_open_file_ptr_no_close(ctx, *aux);
    }
    return d->src ? fz_open_buffer(ctx, d->src) : NULL;
}

static void probe_stream(fz_context* ctx, fz_stream* stm, int want_cover, eb_mobi_info* info)
{
    fz_archive* arch = NULL;
    fz_buffer* buf = NULL;
    int truncated = 0;
    uint8_t head[4] = {0};
    fz_var(arch);
    fz_var(buf);
    fz_try(ctx)
    {
        const int zip = fz_is_zip_archive(ctx, stm);
        fz_seek(ctx, stm, 0, SEEK_SET);
        if (zip) {
            arch = fz_open_archive_with_stream(ctx, stm);
            epub_probe(ctx, arch, want_cover, info);
        } else {
            fz_read(ctx, stm, head, sizeof head);
            fz_seek(ctx, stm, 0, SEEK_SET);
            if (head[0] == '<' || (head[0] == 0xEF && head[1] == 0xBB && head[2] == 0xBF)) {
                buf = fz_read_best(ctx, stm, 1u << 20, &truncated, EB_MAX_FB2_BYTES);
                if (!truncated) fb2_probe(ctx, buf, want_cover, info);
            }
        }
    }
    fz_always(ctx)
    {
        fz_drop_buffer(ctx, buf);
        fz_drop_archive(ctx, arch);
    }
    fz_catch(ctx) fz_ignore_error(ctx);
}

/* Failures are silent by design: callers fall back to MuPDF's own metadata and to rendering page 0. */
void eb_embed_probe(eb_doc* d, int want_cover)
{
    FILE* aux = NULL;
    fz_stream* stm = NULL;
    if ((d->meta_probed && !want_cover) || d->cover_probed) return;
    d->meta_probed = 1;
    if (want_cover) d->cover_probed = 1;
    stm = open_source(d, &aux);
    if (!stm) return;
    probe_stream(d->ctx, stm, want_cover, &d->mobi);
    fz_drop_stream(d->ctx, stm);
    if (aux) fclose(aux);
}
