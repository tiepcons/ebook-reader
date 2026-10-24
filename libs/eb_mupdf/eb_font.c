#include "eb_internal.h"
#include "eb_sfnt.h"

#include <mupdf/ucdn.h>

#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>

#define EB_FONT_MAX_DIRS 32
#define EB_FONT_MAX_FILES 512
#define EB_FONT_MAX_BYTES ((DWORD)32 << 20)
#define EB_FONT_CACHE_BYTES ((size_t)128 << 20)
#define EB_FONT_MAX_PATH_CHARS 32000
#define EB_FONT_DOC_LOADS 32
#define EB_VI_PROBE_A 0x1EA1
#define EB_VI_PROBE_O 0x01A1

typedef struct file_ent {
    wchar_t* path;
    size_t size;
    fz_buffer* buf; /* shared by every face of the file once loaded */
} file_ent;

typedef struct face {
    int file, index;
    char* family;
    int bold, italic;
    fz_font* font;
} face;

typedef struct state {
    file_ent* files;
    int nfiles, capfiles;
    face* faces;
    int nfaces, capfaces;
    int oom;
} state;

typedef struct ticket {
    int face, file, index;
    unsigned gen;
    wchar_t* path;
    fz_buffer* buf;
    fz_font* font;
    size_t size;
} ticket;

enum { CLAIM_NONE, CLAIM_HIT, CLAIM_WORK };

static SRWLOCK g_font_lock = SRWLOCK_INIT;
static state g_st;
static unsigned g_gen;
static size_t g_cached;
static int g_installed;

static const char* const k_serif[] = {"Literata", "Georgia", "Times New Roman", "Cambria", "Palatino Linotype"};
static const char* const k_sans[] = {"Be Vietnam Pro", "Segoe UI", "Arial", "Tahoma", "Calibri"};
static const wchar_t* const k_any[] = {L"*.ttf", L"*.otf", L"*.ttc"};
static const wchar_t* const k_system[] = {L"georgia*.ttf", L"cambria*.ttc", L"segoeui*.ttf", L"times*.ttf",
                                          L"arial*.ttf",   L"tahoma*.ttf",  L"pala*.ttf",    L"calibri*.ttf"};

#define COUNT_OF(a) (sizeof(a) / sizeof((a)[0]))

static void free_state(fz_context* ctx, state* s)
{
    for (int i = 0; i < s->nfiles; ++i) {
        fz_drop_buffer(ctx, s->files[i].buf);
        free(s->files[i].path);
    }
    for (int i = 0; i < s->nfaces; ++i) {
        fz_drop_font(ctx, s->faces[i].font);
        free(s->faces[i].family);
    }
    free(s->files);
    free(s->faces);
    memset(s, 0, sizeof *s);
}

static int is_sep(wchar_t c) { return c == L'\\' || c == L'/'; }

static int has_font_ext(const wchar_t* name)
{
    const wchar_t* dot = wcsrchr(name, L'.');
    return dot && (_wcsicmp(dot, L".ttf") == 0 || _wcsicmp(dot, L".otf") == 0 || _wcsicmp(dot, L".ttc") == 0);
}

static int grow(void** v, int* cap, size_t elem)
{
    int ncap = *cap ? *cap * 2 : 16;
    void* nv = realloc(*v, (size_t)ncap * elem);
    if (!nv) return 0;
    *v = nv;
    *cap = ncap;
    return 1;
}

/* Only "X:\..." and "\\?\X:\..." are accepted; UNC, device and other \\?\ forms are refused. */
static int path_shape_ok(const wchar_t* p)
{
    size_t n = wcsnlen(p, EB_FONT_MAX_PATH_CHARS + 1);
    if (n < 3 || n > EB_FONT_MAX_PATH_CHARS) return 0;
    if (is_sep(p[0]) && is_sep(p[1])) {
        if (p[2] != L'?' || p[3] != L'\\') return 0;
        p += 4;
    }
    return ((p[0] >= L'A' && p[0] <= L'Z') || (p[0] >= L'a' && p[0] <= L'z')) && p[1] == L':' && is_sep(p[2]);
}

static int is_plain_dir(HANDLE h)
{
    BY_HANDLE_FILE_INFORMATION info;
    return GetFileInformationByHandle(h, &info) && (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
           !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT);
}

static HANDLE open_dir(const wchar_t* dir)
{
    HANDLE h = INVALID_HANDLE_VALUE;
    if (!path_shape_ok(dir)) return INVALID_HANDLE_VALUE;
    h = CreateFileW(dir, FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (h != INVALID_HANDLE_VALUE && !is_plain_dir(h)) {
        CloseHandle(h);
        return INVALID_HANDLE_VALUE;
    }
    return h;
}

/* malloc'd "\\?\X:\..." path of an open handle, or NULL. */
static wchar_t* final_path(HANDLE h)
{
    DWORD n = GetFinalPathNameByHandleW(h, NULL, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    wchar_t* p = NULL;
    if (n == 0 || n > EB_FONT_MAX_PATH_CHARS) return NULL;
    p = malloc((size_t)n * sizeof *p);
    if (p && GetFinalPathNameByHandleW(h, p, n, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS) == n - 1 && path_shape_ok(p)) return p;
    free(p);
    return NULL;
}

static int is_system_fonts_dir(const wchar_t* final)
{
    wchar_t sys[MAX_PATH + 8];
    UINT n = GetSystemWindowsDirectoryW(sys, MAX_PATH);
    HANDLE h = INVALID_HANDLE_VALUE;
    wchar_t* sysfinal = NULL;
    int same = 0;
    if (n == 0 || n >= MAX_PATH) return 0;
    wcscat_s(sys, sizeof sys / sizeof sys[0], L"\\Fonts");
    h = open_dir(sys);
    if (h == INVALID_HANDLE_VALUE) return 0;
    sysfinal = final_path(h);
    CloseHandle(h);
    same = sysfinal && _wcsicmp(sysfinal, final) == 0;
    free(sysfinal);
    return same;
}

typedef struct file_reader {
    HANDLE h;
} file_reader;

static int file_read(void* user, uint64_t off, void* dst, size_t len)
{
    LARGE_INTEGER pos;
    DWORD got = 0;
    pos.QuadPart = (LONGLONG)off;
    return len <= EB_FONT_MAX_BYTES && SetFilePointerEx(((file_reader*)user)->h, pos, NULL, FILE_BEGIN) &&
           ReadFile(((file_reader*)user)->h, dst, (DWORD)len, &got, NULL) && got == len;
}

static HANDLE open_font_file(const wchar_t* path, DWORD* size)
{
    BY_HANDLE_FILE_INFORMATION info;
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (h == INVALID_HANDLE_VALUE) return h;
    if (!GetFileInformationByHandle(h, &info) || (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
        info.nFileSizeHigh != 0 || info.nFileSizeLow == 0 || info.nFileSizeLow > EB_FONT_MAX_BYTES) {
        CloseHandle(h);
        return INVALID_HANDLE_VALUE;
    }
    *size = info.nFileSizeLow;
    return h;
}

static fz_buffer* read_font_file(fz_context* ctx, const wchar_t* path)
{
    DWORD size = 0, got = 0;
    HANDLE h = open_font_file(path, &size);
    fz_buffer* buf = NULL;
    fz_var(buf);
    if (h == INVALID_HANDLE_VALUE) fz_throw(ctx, FZ_ERROR_LIBRARY, "cannot open font file");
    fz_try(ctx)
    {
        buf = fz_new_buffer(ctx, size);
        if (!ReadFile(h, buf->data, size, &got, NULL) || got != size) fz_throw(ctx, FZ_ERROR_LIBRARY, "cannot read font file");
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

static void add_file(const wchar_t* path, state* s)
{
    DWORD size = 0;
    HANDLE h = open_font_file(path, &size);
    file_reader rd = {h};
    eb_sfnt_face found[EB_SFNT_MAX_FACES];
    int idx[EB_SFNT_MAX_FACES];
    int n = 0, count = 0;
    if (h == INVALID_HANDLE_VALUE) return;
    count = eb_sfnt_face_count(file_read, &rd, size);
    for (int i = 0; i < count; ++i) {
        if (eb_sfnt_probe(file_read, &rd, size, i, &found[n])) idx[n++] = i;
    }
    CloseHandle(h);
    if (n == 0) return;
    if (s->nfiles == s->capfiles && !grow((void**)&s->files, &s->capfiles, sizeof *s->files)) s->oom = 1;
    if (s->oom) return;
    s->files[s->nfiles] = (file_ent){_wcsdup(path), size, NULL};
    if (!s->files[s->nfiles].path) {
        s->oom = 1;
        return;
    }
    ++s->nfiles;
    for (int i = 0; i < n && !s->oom; ++i) {
        if (s->nfaces == s->capfaces && !grow((void**)&s->faces, &s->capfaces, sizeof *s->faces)) s->oom = 1;
        else if ((s->faces[s->nfaces].family = _strdup(found[i].family)) == NULL) s->oom = 1;
        else {
            s->faces[s->nfaces].file = s->nfiles - 1;
            s->faces[s->nfaces].index = idx[i];
            s->faces[s->nfaces].bold = found[i].bold;
            s->faces[s->nfaces].italic = found[i].italic;
            s->faces[s->nfaces++].font = NULL;
        }
    }
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

static void scan_pattern(const wchar_t* dir, const wchar_t* pattern, state* s, int* files)
{
    WIN32_FIND_DATAW fd;
    wchar_t* query = join_path(dir, pattern);
    HANDLE h = query ? FindFirstFileExW(query, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 0) : INVALID_HANDLE_VALUE;
    free(query);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        wchar_t* full = NULL;
        if (*files >= EB_FONT_MAX_FILES || s->oom) break;
        if (!entry_ok(&fd)) continue;
        full = join_path(dir, fd.cFileName);
        if (!full) {
            s->oom = 1;
            break;
        }
        ++*files;
        add_file(full, s);
        free(full);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static void scan_dir(const wchar_t* dir, state* s, int* files)
{
    HANDLE h = open_dir(dir);
    wchar_t* fp = h == INVALID_HANDLE_VALUE ? NULL : final_path(h);
    const int sys = fp && is_system_fonts_dir(fp);
    const wchar_t* const* pats = sys ? k_system : k_any;
    const int n = sys ? (int)COUNT_OF(k_system) : (int)COUNT_OF(k_any);
    for (int i = 0; fp && i < n; ++i) scan_pattern(fp, pats[i], s, files);
    free(fp);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
}

static int ci_equal(const char* a, const char* b) { return _stricmp(a, b) == 0; }

static int best_face(const char* family, int bold, int italic)
{
    int best = -1, best_score = -1;
    for (int i = 0; i < g_st.nfaces; ++i) {
        int score = 0;
        if (!ci_equal(g_st.faces[i].family, family)) continue;
        score = 2 * (g_st.faces[i].bold == (bold != 0)) + 4 * (g_st.faces[i].italic == (italic != 0));
        if (score > best_score) {
            best_score = score;
            best = i;
        }
    }
    return best;
}

static int generic_face(const char* const* prefs, size_t n, int bold, int italic)
{
    for (size_t i = 0; i < n; ++i) {
        int f = best_face(prefs[i], bold, italic);
        if (f >= 0) return f;
    }
    return -1;
}

static int pick_face(const char* name, int bold, int italic, int generic)
{
    if (!name || !*name) return -1;
    if (generic && ci_equal(name, "serif")) return generic_face(k_serif, COUNT_OF(k_serif), bold, italic);
    if (generic && ci_equal(name, "sans-serif")) return generic_face(k_sans, COUNT_OF(k_sans), bold, italic);
    return best_face(name, bold, italic);
}

/* Looks the face up under the lock and either returns the cached font (CLAIM_HIT) or the data needed to build it. */
static int claim(fz_context* ctx, const char* name, int bold, int italic, int generic, ticket* t)
{
    int r = CLAIM_NONE, fi = -1;
    AcquireSRWLockExclusive(&g_font_lock);
    fi = pick_face(name, bold, italic, generic);
    if (fi >= 0) {
        face* f = &g_st.faces[fi];
        file_ent* fl = &g_st.files[f->file];
        *t = (ticket){fi, f->file, f->index, g_gen, NULL, NULL, NULL, fl->size};
        if (f->font) {
            t->font = fz_keep_font(ctx, f->font);
            r = CLAIM_HIT;
        } else if (fl->buf) {
            t->buf = fz_keep_buffer(ctx, fl->buf);
            r = CLAIM_WORK;
        } else if (g_cached + fl->size <= EB_FONT_CACHE_BYTES && (t->path = _wcsdup(fl->path)) != NULL) {
            r = CLAIM_WORK;
        }
    }
    ReleaseSRWLockExclusive(&g_font_lock);
    return r;
}

/* Takes ownership of buf; returns the buffer to build from (the first published one wins), or NULL if the cache is full. */
static fz_buffer* publish_buffer(fz_context* ctx, const ticket* t, fz_buffer* buf)
{
    AcquireSRWLockExclusive(&g_font_lock);
    if (t->gen == g_gen) {
        file_ent* fl = &g_st.files[t->file];
        if (fl->buf) {
            fz_drop_buffer(ctx, buf);
            buf = fz_keep_buffer(ctx, fl->buf);
        } else if (g_cached + t->size <= EB_FONT_CACHE_BYTES) {
            fl->buf = fz_keep_buffer(ctx, buf);
            g_cached += t->size;
        } else {
            fz_drop_buffer(ctx, buf);
            buf = NULL;
        }
    }
    ReleaseSRWLockExclusive(&g_font_lock);
    return buf;
}

static fz_font* publish_font(fz_context* ctx, const ticket* t, fz_font* font)
{
    AcquireSRWLockExclusive(&g_font_lock);
    if (t->gen == g_gen) {
        face* f = &g_st.faces[t->face];
        if (f->font) {
            fz_drop_font(ctx, font);
            font = fz_keep_font(ctx, f->font);
        } else {
            f->font = fz_keep_font(ctx, font);
        }
    }
    ReleaseSRWLockExclusive(&g_font_lock);
    return font;
}

/* Reads and parses outside the lock. Failures other than out-of-memory only mean "no such font". */
static fz_font* build(fz_context* ctx, ticket* t)
{
    fz_buffer* buf = t->buf;
    fz_font* font = NULL;
    fz_var(buf);
    fz_var(font);
    fz_try(ctx)
    {
        if (!buf) {
            buf = read_font_file(ctx, t->path);
            buf = publish_buffer(ctx, t, buf);
        }
        if (buf) font = fz_new_font_from_buffer(ctx, NULL, buf, t->index, 0);
    }
    fz_always(ctx)
    {
        fz_drop_buffer(ctx, buf);
        free(t->path);
    }
    fz_catch(ctx)
    {
        fz_rethrow_if(ctx, FZ_ERROR_SYSTEM);
        fz_report_error(ctx);
        font = NULL;
    }
    return font ? publish_font(ctx, t, font) : NULL;
}

static fz_font* load_named(fz_context* ctx, const char* name, int bold, int italic, int generic)
{
    ticket t;
    int* loads = (int*)fz_user_context(ctx);
    int r = claim(ctx, name, bold, italic, generic, &t);
    if (r == CLAIM_NONE) return NULL;
    if (r == CLAIM_HIT) return t.font;
    if (loads && *loads >= EB_FONT_DOC_LOADS) {
        fz_drop_buffer(ctx, t.buf);
        free(t.path);
        return NULL;
    }
    if (loads) ++*loads;
    return build(ctx, &t);
}

static fz_font* system_font(fz_context* ctx, const char* name, int bold, int italic, int exact)
{
    (void)exact;
    return load_named(ctx, name, bold, italic, 1);
}

static int covers_vietnamese(fz_context* ctx, fz_font* font)
{
    return fz_encode_character(ctx, font, EB_VI_PROBE_A) != 0 && fz_encode_character(ctx, font, EB_VI_PROBE_O) != 0;
}

static fz_font* fallback_from(fz_context* ctx, const char* const* prefs, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        fz_font* font = load_named(ctx, prefs[i], 0, 0, 0);
        if (font && covers_vietnamese(ctx, font)) return font;
        fz_drop_font(ctx, font);
    }
    return NULL;
}

/* Latin-script glyph fallback from the app fonts. Regular weight only: MuPDF keeps the first fallback font per script
 * and serif flag for the whole life of the engine (until eb_shutdown), so style variants could not be honoured. */
static fz_font* fallback_font(fz_context* ctx, int script, int language, int serif, int bold, int italic)
{
    fz_font* font = NULL;
    (void)language;
    (void)bold;
    (void)italic;
    if (script != UCDN_SCRIPT_LATIN && script != UCDN_SCRIPT_COMMON && script != UCDN_SCRIPT_INHERITED) return NULL;
    font = fallback_from(ctx, serif ? k_serif : k_sans, serif ? COUNT_OF(k_serif) : COUNT_OF(k_sans));
    if (!font) font = fallback_from(ctx, serif ? k_sans : k_serif, serif ? COUNT_OF(k_sans) : COUNT_OF(k_serif));
    return font;
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
    state fresh = {0}, old;
    int files = 0;
    fz_context* ctx = NULL;
    if (!dirs_valid(dirs, count)) return eb_fail_msg(EB_E_ARG, "bad font directories");
    ctx = eb_acquire_ctx();
    if (!ctx) return eb_take_oom() ? eb_fail_msg(EB_E_NOMEM, "out of memory") : eb_fail_msg(EB_E_ARG, "eb_init not called");
    for (int32_t i = 0; i < count && !fresh.oom; ++i) scan_dir(dirs[i], &fresh, &files);
    if (fresh.oom) {
        free_state(ctx, &fresh);
        eb_release_ctx(ctx);
        return eb_fail_msg(EB_E_NOMEM, "out of memory");
    }
    AcquireSRWLockExclusive(&g_font_lock);
    old = g_st;
    g_st = fresh;
    ++g_gen;
    g_cached = 0;
    if (!g_installed) {
        fz_install_load_system_font_funcs(ctx, system_font, NULL, fallback_font);
        g_installed = 1;
    }
    ReleaseSRWLockExclusive(&g_font_lock);
    free_state(ctx, &old);
    eb_release_ctx(ctx);
    return EB_OK;
}

void eb_fonts_release(fz_context* ctx)
{
    state old;
    AcquireSRWLockExclusive(&g_font_lock);
    old = g_st;
    memset(&g_st, 0, sizeof g_st);
    ++g_gen;
    g_cached = 0;
    g_installed = 0;
    ReleaseSRWLockExclusive(&g_font_lock);
    free_state(ctx, &old);
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
    total = g_st.nfaces;
    v = calloc((size_t)total + 1, sizeof *v);
    for (int i = 0; v && i < total; ++i) {
        v[n] = _strdup(g_st.faces[i].family);
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
