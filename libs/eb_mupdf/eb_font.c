#include "eb_internal.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include <mupdf/ucdn.h>

#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>

#define EB_FONT_MAX_DIRS 32
#define EB_FONT_MAX_FILES 512
#define EB_FONT_MAX_FACES 16
#define EB_FONT_MAX_BYTES ((DWORD)32 << 20)
#define EB_FONT_CACHE_BYTES ((size_t)128 << 20)
#define EB_FONT_MAX_PATH_CHARS 32000
#define EB_VI_PROBE_A 0x1EA1
#define EB_VI_PROBE_O 0x01A1

typedef struct face {
    wchar_t* path;
    char* family;
    int bold, italic, index;
    size_t size;
    fz_font* font;
} face;

typedef struct builder {
    face* v;
    int n, cap, files;
} builder;

static SRWLOCK g_font_lock = SRWLOCK_INIT;
static face* g_faces;
static int g_nfaces;
static size_t g_cached;
static int g_installed;

static const char* const k_serif[] = {"Literata", "Georgia", "Times New Roman", "Cambria", "Palatino Linotype"};
static const char* const k_sans[] = {"Be Vietnam Pro", "Segoe UI", "Arial", "Tahoma", "Calibri"};
static const wchar_t* const k_any[] = {L"*.ttf", L"*.otf", L"*.ttc"};
static const wchar_t* const k_system[] = {L"georgia*.ttf", L"cambria*.ttc", L"segoeui*.ttf", L"times*.ttf",
                                          L"arial*.ttf",   L"tahoma*.ttf",  L"pala*.ttf",    L"calibri*.ttf"};

static void free_faces(fz_context* ctx, face* v, int n)
{
    for (int i = 0; i < n; ++i) {
        fz_drop_font(ctx, v[i].font);
        free(v[i].path);
        free(v[i].family);
    }
    free(v);
}

static int is_sep(wchar_t c) { return c == L'\\' || c == L'/'; }

static int has_font_ext(const wchar_t* name)
{
    const wchar_t* dot = wcsrchr(name, L'.');
    return dot && (_wcsicmp(dot, L".ttf") == 0 || _wcsicmp(dot, L".otf") == 0 || _wcsicmp(dot, L".ttc") == 0);
}

static int same_dir(const wchar_t* a, const wchar_t* b)
{
    size_t la = wcslen(a), lb = wcslen(b);
    while (la > 0 && is_sep(a[la - 1])) --la;
    while (lb > 0 && is_sep(b[lb - 1])) --lb;
    if (la != lb) return 0;
    for (size_t i = 0; i < la; ++i) {
        if (is_sep(a[i]) && is_sep(b[i])) continue;
        if (towlower(a[i]) != towlower(b[i])) return 0;
    }
    return 1;
}

static int is_system_fonts_dir(const wchar_t* dir)
{
    wchar_t sys[MAX_PATH + 8];
    UINT n = GetSystemWindowsDirectoryW(sys, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return 0;
    wcscat_s(sys, sizeof sys / sizeof sys[0], L"\\Fonts");
    return same_dir(dir, sys);
}

static int dir_usable(const wchar_t* dir)
{
    DWORD attr = 0;
    size_t n = wcsnlen(dir, EB_FONT_MAX_PATH_CHARS + 1);
    if (n < 3 || n > EB_FONT_MAX_PATH_CHARS) return 0;
    if (is_sep(dir[0]) && is_sep(dir[1]) && dir[2] == L'.') return 0;
    attr = GetFileAttributesW(dir);
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) && !(attr & FILE_ATTRIBUTE_REPARSE_POINT);
}

static fz_buffer* read_font_file(fz_context* ctx, const wchar_t* path)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    fz_buffer* buf = NULL;
    fz_var(buf);
    if (h == INVALID_HANDLE_VALUE) fz_throw(ctx, FZ_ERROR_LIBRARY, "cannot open font file");
    fz_try(ctx)
    {
        BY_HANDLE_FILE_INFORMATION info;
        DWORD got = 0;
        if (!GetFileInformationByHandle(h, &info) || (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
            info.nFileSizeHigh != 0 || info.nFileSizeLow == 0 || info.nFileSizeLow > EB_FONT_MAX_BYTES)
            fz_throw(ctx, FZ_ERROR_LIBRARY, "font file rejected");
        buf = fz_new_buffer(ctx, info.nFileSizeLow);
        if (!ReadFile(h, buf->data, info.nFileSizeLow, &got, NULL) || got != info.nFileSizeLow)
            fz_throw(ctx, FZ_ERROR_LIBRARY, "cannot read font file");
        buf->len = got;
    }
    fz_always(ctx) CloseHandle(h);
    fz_catch(ctx)
    {
        fz_drop_buffer(ctx, buf);
        fz_rethrow(ctx);
    }
    return buf;
}

static int builder_add(builder* b, face* f)
{
    if (b->n == b->cap) {
        int cap = b->cap ? b->cap * 2 : 16;
        face* nv = realloc(b->v, (size_t)cap * sizeof *nv);
        if (!nv) return 0;
        b->v = nv;
        b->cap = cap;
    }
    b->v[b->n++] = *f;
    return 1;
}

static int face_count(const fz_buffer* buf)
{
    const unsigned char* d = buf->data;
    if (buf->len < 12 || memcmp(d, "ttcf", 4) != 0) return 1;
    unsigned n = ((unsigned)d[8] << 24) | ((unsigned)d[9] << 16) | ((unsigned)d[10] << 8) | d[11];
    return n < 1 ? 1 : n > EB_FONT_MAX_FACES ? EB_FONT_MAX_FACES : (int)n;
}

/* Reads family and style of one face; returns 0 for a face that is unusable. */
static int probe_face(fz_context* ctx, fz_buffer* buf, int index, face* out)
{
    fz_font* font = NULL;
    int ok = 0;
    fz_var(font);
    fz_try(ctx)
    {
        FT_Face ft = NULL;
        font = fz_new_font_from_buffer(ctx, NULL, buf, index, 0);
        ft = (FT_Face)fz_font_ft_face(ctx, font);
        if (ft && ft->family_name && ft->family_name[0]) {
            out->family = _strdup(ft->family_name);
            out->bold = fz_font_is_bold(ctx, font) != 0;
            out->italic = fz_font_is_italic(ctx, font) != 0;
            ok = out->family != NULL;
        }
    }
    fz_always(ctx) fz_drop_font(ctx, font);
    fz_catch(ctx) ok = 0;
    return ok;
}

static void add_file(fz_context* ctx, const wchar_t* path, builder* b)
{
    fz_buffer* buf = NULL;
    int faces = 0;
    fz_var(buf);
    fz_try(ctx)
    {
        buf = read_font_file(ctx, path);
        faces = face_count(buf);
        for (int i = 0; i < faces; ++i) {
            face f = {0};
            f.index = i;
            f.size = buf->len;
            if (!probe_face(ctx, buf, i, &f)) continue;
            f.path = _wcsdup(path);
            if (!f.path || !builder_add(b, &f)) {
                free(f.path);
                free(f.family);
            }
        }
    }
    fz_always(ctx) fz_drop_buffer(ctx, buf);
    fz_catch(ctx) {}
}

static wchar_t* join_path(const wchar_t* dir, const wchar_t* name)
{
    size_t n = wcslen(dir) + wcslen(name) + 2;
    wchar_t* p = malloc(n * sizeof *p);
    if (!p) return NULL;
    wcscpy_s(p, n, dir);
    if (!is_sep(dir[wcslen(dir) - 1])) wcscat_s(p, n, L"\\");
    wcscat_s(p, n, name);
    return p;
}

static int entry_ok(const WIN32_FIND_DATAW* fd)
{
    return !(fd->dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) && has_font_ext(fd->cFileName) &&
           fd->nFileSizeHigh == 0 && fd->nFileSizeLow > 0 && fd->nFileSizeLow <= EB_FONT_MAX_BYTES;
}

static void scan_pattern(fz_context* ctx, const wchar_t* dir, const wchar_t* pattern, builder* b)
{
    WIN32_FIND_DATAW fd;
    wchar_t* query = join_path(dir, pattern);
    HANDLE h = query ? FindFirstFileExW(query, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 0) : INVALID_HANDLE_VALUE;
    free(query);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        wchar_t* full = NULL;
        if (b->files >= EB_FONT_MAX_FILES) break;
        if (!entry_ok(&fd)) continue;
        full = join_path(dir, fd.cFileName);
        if (!full) break;
        ++b->files;
        add_file(ctx, full, b);
        free(full);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static void scan_dir(fz_context* ctx, const wchar_t* dir, builder* b)
{
    const int sys = is_system_fonts_dir(dir);
    const wchar_t* const* pats = sys ? k_system : k_any;
    const int n = sys ? (int)(sizeof k_system / sizeof k_system[0]) : (int)(sizeof k_any / sizeof k_any[0]);
    if (!dir_usable(dir)) return;
    for (int i = 0; i < n; ++i) scan_pattern(ctx, dir, pats[i], b);
}

static int ci_equal(const char* a, const char* b) { return _stricmp(a, b) == 0; }

static face* best_face(const char* family, int bold, int italic)
{
    face* best = NULL;
    int best_score = -1;
    for (int i = 0; i < g_nfaces; ++i) {
        int score = 0;
        if (!ci_equal(g_faces[i].family, family)) continue;
        score = 2 * (g_faces[i].bold == (bold != 0)) + 4 * (g_faces[i].italic == (italic != 0));
        if (score > best_score) {
            best_score = score;
            best = &g_faces[i];
        }
    }
    return best;
}

static face* generic_face(const char* const* prefs, size_t n, int bold, int italic)
{
    for (size_t i = 0; i < n; ++i) {
        face* f = best_face(prefs[i], bold, italic);
        if (f) return f;
    }
    return NULL;
}

#define COUNT_OF(a) (sizeof(a) / sizeof((a)[0]))

static face* pick_face(const char* name, int bold, int italic)
{
    if (!name || !*name) return NULL;
    if (ci_equal(name, "serif")) return generic_face(k_serif, COUNT_OF(k_serif), bold, italic);
    if (ci_equal(name, "sans-serif")) return generic_face(k_sans, COUNT_OF(k_sans), bold, italic);
    return best_face(name, bold, italic);
}

/* Caller holds g_font_lock; throws on read or parse failure. */
static fz_font* face_font(fz_context* ctx, face* f)
{
    fz_buffer* buf = NULL;
    fz_font* font = NULL;
    if (f->font) return fz_keep_font(ctx, f->font);
    fz_var(buf);
    fz_try(ctx)
    {
        buf = read_font_file(ctx, f->path);
        font = fz_new_font_from_buffer(ctx, NULL, buf, f->index, 0);
    }
    fz_always(ctx) fz_drop_buffer(ctx, buf);
    fz_catch(ctx) fz_rethrow(ctx);
    if (g_cached + f->size <= EB_FONT_CACHE_BYTES) {
        f->font = fz_keep_font(ctx, font);
        g_cached += f->size;
    }
    return font;
}

static fz_font* system_font(fz_context* ctx, const char* name, int bold, int italic, int exact)
{
    fz_font* font = NULL;
    (void)exact;
    fz_var(font);
    AcquireSRWLockExclusive(&g_font_lock);
    fz_try(ctx)
    {
        face* f = pick_face(name, bold, italic);
        if (f) font = face_font(ctx, f);
    }
    fz_always(ctx) ReleaseSRWLockExclusive(&g_font_lock);
    fz_catch(ctx) fz_rethrow(ctx);
    return font;
}

static int covers_vietnamese(fz_context* ctx, fz_font* font)
{
    return fz_encode_character(ctx, font, EB_VI_PROBE_A) != 0 && fz_encode_character(ctx, font, EB_VI_PROBE_O) != 0;
}

static fz_font* fallback_from(fz_context* ctx, const char* const* prefs, size_t n, int bold, int italic)
{
    for (size_t i = 0; i < n; ++i) {
        face* f = best_face(prefs[i], bold, italic);
        fz_font* font = f ? face_font(ctx, f) : NULL;
        if (font && covers_vietnamese(ctx, font)) return font;
        fz_drop_font(ctx, font);
    }
    return NULL;
}

static fz_font* fallback_font(fz_context* ctx, int script, int language, int serif, int bold, int italic)
{
    fz_font* font = NULL;
    (void)language;
    if (script != UCDN_SCRIPT_LATIN && script != UCDN_SCRIPT_COMMON && script != UCDN_SCRIPT_INHERITED) return NULL;
    fz_var(font);
    AcquireSRWLockExclusive(&g_font_lock);
    fz_try(ctx)
    {
        font = fallback_from(ctx, serif ? k_serif : k_sans, serif ? COUNT_OF(k_serif) : COUNT_OF(k_sans), bold, italic);
        if (!font) font = fallback_from(ctx, serif ? k_sans : k_serif, serif ? COUNT_OF(k_sans) : COUNT_OF(k_serif), bold, italic);
    }
    fz_always(ctx) ReleaseSRWLockExclusive(&g_font_lock);
    fz_catch(ctx) fz_rethrow(ctx);
    return font;
}

static void install_loaders(fz_context* ctx)
{
    if (g_installed) return;
    fz_install_load_system_font_funcs(ctx, system_font, NULL, fallback_font);
    g_installed = 1;
}

static int dirs_valid(const wchar_t* const* dirs, int32_t count)
{
    if (count < 0 || count > EB_FONT_MAX_DIRS || (count > 0 && !dirs)) return 0;
    for (int32_t i = 0; i < count; ++i) {
        if (!dirs[i] || wcsnlen(dirs[i], EB_FONT_MAX_PATH_CHARS + 1) > EB_FONT_MAX_PATH_CHARS) return 0;
    }
    return 1;
}

eb_status eb_set_font_dirs(const wchar_t* const* dirs, int32_t count)
{
    builder b = {0};
    face* old = NULL;
    int old_n = 0;
    fz_context* ctx = NULL;
    if (!dirs_valid(dirs, count)) return eb_fail_msg(EB_E_ARG, "bad font directories");
    ctx = eb_acquire_ctx();
    if (!ctx) return eb_take_oom() ? eb_fail_msg(EB_E_NOMEM, "out of memory") : eb_fail_msg(EB_E_ARG, "eb_init not called");
    for (int32_t i = 0; i < count; ++i) scan_dir(ctx, dirs[i], &b);
    AcquireSRWLockExclusive(&g_font_lock);
    old = g_faces;
    old_n = g_nfaces;
    g_faces = b.v;
    g_nfaces = b.n;
    g_cached = 0;
    install_loaders(ctx);
    ReleaseSRWLockExclusive(&g_font_lock);
    free_faces(ctx, old, old_n);
    eb_release_ctx(ctx);
    return EB_OK;
}

void eb_fonts_release(fz_context* ctx)
{
    AcquireSRWLockExclusive(&g_font_lock);
    free_faces(ctx, g_faces, g_nfaces);
    g_faces = NULL;
    g_nfaces = 0;
    g_cached = 0;
    g_installed = 0;
    ReleaseSRWLockExclusive(&g_font_lock);
}

static int cmp_names(const void* a, const void* b) { return _stricmp(*(char* const*)a, *(char* const*)b); }

static int dedupe(char** v, int n)
{
    int out = 0;
    for (int i = 0; i < n; ++i) {
        if (out > 0 && ci_equal(v[out - 1], v[i])) free(v[i]);
        else v[out++] = v[i];
    }
    return out;
}

eb_status eb_font_families(char*** names, int32_t* count)
{
    char** v = NULL;
    int n = 0, total = 0;
    if (!names || !count) return eb_fail_msg(EB_E_ARG, "null argument");
    *names = NULL;
    *count = 0;
    AcquireSRWLockShared(&g_font_lock);
    total = g_nfaces;
    v = calloc((size_t)total + 1, sizeof *v);
    for (int i = 0; v && i < total; ++i) {
        v[n] = _strdup(g_faces[i].family);
        if (!v[n]) break;
        ++n;
    }
    ReleaseSRWLockShared(&g_font_lock);
    if (!v || n < total) {
        eb_font_families_free(v, n);
        return eb_fail_msg(EB_E_NOMEM, "out of memory");
    }
    qsort(v, (size_t)n, sizeof *v, cmp_names);
    *count = dedupe(v, n);
    *names = v;
    return EB_OK;
}

void eb_font_families_free(char** names, int32_t count)
{
    for (int32_t i = 0; names && i < count; ++i) free(names[i]);
    free(names);
}
