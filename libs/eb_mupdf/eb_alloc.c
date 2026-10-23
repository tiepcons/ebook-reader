#include "eb_internal.h"

#include <stdlib.h>
#include <windows.h>

#define EB_BLOCK_HEADER 16

static volatile LONG64 g_used;
static LONG64 g_cap;
static __declspec(thread) int t_oom;

void eb_note_oom(void) { t_oom = 1; }

int eb_take_oom(void)
{
    int v = t_oom;
    t_oom = 0;
    return v;
}

void eb_throw_oom(fz_context* ctx)
{
    eb_note_oom();
    fz_throw(ctx, FZ_ERROR_SYSTEM, "out of memory");
}

static int reserve(size_t bytes)
{
    if (bytes > (size_t)g_cap) return 0;
    if (InterlockedAdd64(&g_used, (LONG64)bytes) > g_cap) {
        InterlockedAdd64(&g_used, -(LONG64)bytes);
        return 0;
    }
    return 1;
}

static void* a_malloc(void* user, size_t size)
{
    char* p = NULL;
    (void)user;
    if (size <= (size_t)g_cap && reserve(size + EB_BLOCK_HEADER)) {
        p = malloc(size + EB_BLOCK_HEADER);
        if (p) {
            *(size_t*)p = size;
            return p + EB_BLOCK_HEADER;
        }
        InterlockedAdd64(&g_used, -(LONG64)(size + EB_BLOCK_HEADER));
    }
    eb_note_oom();
    return NULL;
}

static void a_free(void* user, void* ptr)
{
    char* base = NULL;
    (void)user;
    if (!ptr) return;
    base = (char*)ptr - EB_BLOCK_HEADER;
    InterlockedAdd64(&g_used, -(LONG64)(*(size_t*)base + EB_BLOCK_HEADER));
    free(base);
}

static void* a_realloc(void* user, void* ptr, size_t size)
{
    char* base = NULL;
    char* fresh = NULL;
    size_t old = 0;
    if (!ptr) return a_malloc(user, size);
    base = (char*)ptr - EB_BLOCK_HEADER;
    old = *(size_t*)base;
    if (size > old && !reserve(size - old)) {
        eb_note_oom();
        return NULL;
    }
    fresh = realloc(base, size + EB_BLOCK_HEADER);
    if (!fresh) {
        if (size > old) InterlockedAdd64(&g_used, -(LONG64)(size - old));
        eb_note_oom();
        return NULL;
    }
    if (size < old) InterlockedAdd64(&g_used, -(LONG64)(old - size));
    *(size_t*)fresh = size;
    return fresh + EB_BLOCK_HEADER;
}

const fz_alloc_context* eb_alloc_context(uint64_t cap)
{
    static const fz_alloc_context ctx = {NULL, a_malloc, a_realloc, a_free};
    g_cap = cap > (uint64_t)INT64_MAX / 2 ? INT64_MAX / 2 : (LONG64)cap;
    InterlockedExchange64(&g_used, 0);
    return &ctx;
}
