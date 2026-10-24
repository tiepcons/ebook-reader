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

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Percent-decodes an href (exactly two hex digits per escape) up to any '#' or '?'. */
static int decode_href(const char* href, char* out, size_t cap)
{
    size_t n = 0;
    for (const char* p = href; *p && *p != '#' && *p != '?'; ++p) {
        char c = *p;
        if (n + 1 >= cap || c == '\\') return 0;
        if (c == '%') {
            const int hi = p[1] ? hex_value(p[1]) : -1;
            const int lo = hi >= 0 ? hex_value(p[2]) : -1;
            if (hi < 0 || lo < 0 || (hi == 0 && lo == 0) || (hi * 16 + lo) == '\\') return 0;
            c = (char)(hi * 16 + lo);
            p += 2;
        }
        out[n++] = c;
    }
    out[n] = '\0';
    return n > 0;
}

/* Joins the already-resolved "dir" and a manifest href; rejects anything that climbs above the archive root. */
static int resolve_path(const char* dir, const char* href, char* out, size_t cap)
{
    char dec[EB_PATH_MAX];
    char raw[EB_PATH_MAX];
    size_t len = 0;
    if (!decode_href(href, dec, sizeof dec) || dec[0] == '/' || strlen(dir) + strlen(dec) + 1 > sizeof raw) return 0;
    snprintf(raw, sizeof raw, "%s%s", dir, dec);
    out[0] = '\0';
    for (const char* seg = raw; seg;) {
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

/* Reads at most "limit" bytes, growing to min(2*cap, limit+1); NULL when the stream is longer. */
static fz_buffer* read_bounded(fz_context* ctx, fz_stream* stm, size_t limit)
{
    uint8_t chunk[16384];
    fz_buffer* buf = fz_new_buffer(ctx, 16384);
    fz_try(ctx)
    {
        for (;;) {
            const size_t n = fz_read(ctx, stm, chunk, sizeof chunk);
            if (n == 0) break;
            if (buf->len + n > limit) {
                fz_drop_buffer(ctx, buf);
                buf = NULL;
                break;
            }
            if (buf->len + n > buf->cap) {
                size_t want = buf->cap * 2 > buf->len + n ? buf->cap * 2 : buf->len + n;
                if (want > limit + 1) want = limit + 1;
                fz_resize_buffer(ctx, buf, want);
            }
            fz_append_data(ctx, buf, chunk, n);
        }
    }
    fz_catch(ctx)
    {
        fz_drop_buffer(ctx, buf);
        fz_rethrow(ctx);
    }
    return buf;
}

static fz_buffer* read_entry(fz_context* ctx, fz_archive* arch, const char* name, size_t limit)
{
    fz_stream* s = fz_try_open_archive_entry(ctx, arch, name);
    fz_buffer* buf = NULL;
    if (!s) return NULL;
    fz_try(ctx) buf = read_bounded(ctx, s, limit);
    fz_always(ctx) fz_drop_stream(ctx, s);
    fz_catch(ctx) fz_rethrow(ctx);
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

static void epub_cover(eb_doc* d, fz_archive* arch, fz_xml* package, const char* opf_dir)
{
    const char* href = find_cover_href(package);
    char path[EB_PATH_MAX];
    if (!href || !resolve_path(opf_dir, href, path, sizeof path)) return;
    eb_check_abort(d);
    d->cover_buf = read_entry(d->ctx, arch, path, EB_MAX_IMAGE_BYTES);
}

static void epub_meta(fz_xml* package, eb_mobi_info* info)
{
    fz_xml* metadata = fz_xml_find_down(package, "metadata");
    set_if_empty(&info->title, dup_trimmed(child_text(metadata, "title")));
    set_if_empty(&info->author, dup_trimmed(child_text(metadata, "creator")));
    set_if_empty(&info->language, eb_normalize_language(child_text(metadata, "language")));
}

static void epub_probe(eb_doc* d, fz_archive* arch, int want_cover)
{
    fz_context* ctx = d->ctx;
    eb_mobi_info* info = &d->mobi;
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
    if (want_cover) eb_check_abort(d);
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
        if (want_cover) epub_cover(d, arch, package, dir);
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

static fz_buffer* b64_decode(fz_context* ctx, const char* in)
{
    const size_t cap = strlen(in) / 4 * 3 + 3;
    fz_buffer* out = NULL;
    uint32_t acc = 0;
    int bits = 0;
    size_t n = 0;
    if (cap > EB_MAX_IMAGE_BYTES + 3) return NULL;
    out = fz_new_buffer(ctx, cap);
    fz_try(ctx)
    {
        for (; *in; ++in) {
            const int v = b64_value((unsigned char)*in);
            if (v < 0) {
                if (*in == '=' || *in == ' ' || *in == '\n' || *in == '\r' || *in == '\t') continue;
                fz_throw(ctx, FZ_ERROR_FORMAT, "bad base64");
            }
            acc = (acc << 6) | (uint32_t)v;
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                fz_append_byte(ctx, out, (int)((acc >> bits) & 0xFFu));
                ++n;
            }
        }
        if (n > EB_MAX_IMAGE_BYTES) fz_throw(ctx, FZ_ERROR_LIMIT, "cover too large");
    }
    fz_catch(ctx)
    {
        fz_drop_buffer(ctx, out);
        fz_rethrow(ctx);
    }
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

static void fb2_cover(eb_doc* d, fz_xml* root, fz_xml* ti)
{
    fz_xml* img = fz_xml_find_down(fz_xml_find_down(ti, "coverpage"), "image");
    const char* href = fz_xml_att_alt(img, "l:href", "xlink:href");
    if (!href) href = fz_xml_att(img, "href");
    if (!href || href[0] != '#') return;
    for (fz_xml* b = fz_xml_find_down(root, "binary"); b; b = fz_xml_find_next(b, "binary")) {
        const char* text = NULL;
        if (!fz_xml_att_eq(b, "id", href + 1)) continue;
        text = fz_xml_text(fz_xml_down(b));
        if (text) d->cover_buf = b64_decode(d->ctx, text);
        return;
    }
}

static void fb2_probe(eb_doc* d, fz_buffer* buf, int want_cover)
{
    fz_context* ctx = d->ctx;
    eb_mobi_info* info = &d->mobi;
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
        if (want_cover) fb2_cover(d, root, ti);
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

static int format_contains(eb_doc* d, const char* needle)
{
    char name[64] = {0};
    int n = -1;
    fz_try(d->ctx) n = fz_lookup_metadata(d->ctx, d->doc, FZ_META_FORMAT, name, sizeof name);
    fz_catch(d->ctx) fz_ignore_error(d->ctx);
    name[sizeof name - 1] = '\0';
    return n > 1 && contains_nocase(name, needle);
}

/* FB2 is parsed straight from the kept buffer of memory docs, or from a bounded read of the file. */
static void probe_fb2(eb_doc* d, fz_stream* stm, int want_cover)
{
    fz_context* ctx = d->ctx;
    fz_buffer* buf = NULL;
    if (d->src) {
        if (d->src->len <= EB_MAX_FB2_BYTES) fb2_probe(d, d->src, want_cover);
        return;
    }
    fz_var(buf);
    fz_try(ctx)
    {
        fz_seek(ctx, stm, 0, SEEK_SET);
        buf = read_bounded(ctx, stm, EB_MAX_FB2_BYTES);
        if (buf) fb2_probe(d, buf, want_cover);
    }
    fz_always(ctx) fz_drop_buffer(ctx, buf);
    fz_catch(ctx) fz_rethrow(ctx);
}

static int probe_stream(eb_doc* d, fz_stream* stm, int want_cover)
{
    fz_context* ctx = d->ctx;
    fz_archive* arch = NULL;
    int aborted = 0;
    fz_var(arch);
    fz_try(ctx)
    {
        if (format_contains(d, "epub") && fz_is_zip_archive(ctx, stm)) {
            fz_seek(ctx, stm, 0, SEEK_SET);
            arch = fz_open_archive_with_stream(ctx, stm);
            epub_probe(d, arch, want_cover);
        } else if (format_contains(d, "fiction") || format_contains(d, "fb2")) {
            probe_fb2(d, stm, want_cover);
        }
    }
    fz_always(ctx) fz_drop_archive(ctx, arch);
    fz_catch(ctx)
    {
        aborted = fz_caught(ctx) == FZ_ERROR_ABORT;
        fz_ignore_error(ctx);
    }
    return aborted;
}

/* Other failures are silent by design: callers fall back to MuPDF's own metadata and to rendering page 0.
 * Returns 1 only when eb_abort cancelled a cover probe (want_cover); the probe then runs again next time. */
int eb_embed_probe(eb_doc* d, int want_cover)
{
    FILE* aux = NULL;
    fz_stream* stm = NULL;
    int aborted = 0;
    if ((d->meta_probed && !want_cover) || d->cover_probed) return 0;
    stm = open_source(d, &aux);
    if (!stm) {
        d->meta_probed = 1;
        d->cover_probed |= want_cover;
        return 0;
    }
    if (want_cover) eb_begin_call(d);
    aborted = probe_stream(d, stm, want_cover);
    fz_drop_stream(d->ctx, stm);
    if (aux) fclose(aux);
    if (aborted && want_cover) return 1;
    d->meta_probed = 1;
    d->cover_probed |= want_cover;
    return 0;
}
