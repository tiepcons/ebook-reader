#ifndef EB_INTERNAL_H
#define EB_INTERNAL_H

#include "eb_mupdf.h"

#include <mupdf/fitz.h>
#include <stdio.h>

struct eb_doc {
    fz_context* ctx;
    fz_document* doc;
    FILE* file;
    int reflowable;
    int laid_out;
    fz_cookie cookie;
};

extern eb_limits eb_g_limits;

/* Maps the MuPDF error just caught on ctx to an eb_status and records its message for eb_last_error. */
eb_status eb_fail(fz_context* ctx);
eb_status eb_fail_msg(eb_status s, const char* msg);

/* Page access helpers: reflowable documents must have been laid out first (EB_E_ARG otherwise). */
eb_status eb_require_layout(eb_doc* doc);
eb_status eb_count_pages(eb_doc* doc, int32_t* out);
eb_status eb_check_page(eb_doc* doc, int32_t page);

/* Clones the base context and counts it as an open document; release with eb_release_ctx. NULL if not initialised. */
fz_context* eb_acquire_ctx(void);
void eb_release_ctx(fz_context* ctx);

/* Capped allocator; failures are remembered per thread so FZ_ERROR_SYSTEM can be told apart from I/O errors. */
const fz_alloc_context* eb_alloc_context(uint64_t cap);
void eb_note_oom(void);
int eb_take_oom(void);
void eb_throw_oom(fz_context* ctx);

/* Per-call cancellation: eb_begin_call resets the cookie, eb_check_abort throws FZ_ERROR_ABORT if it was set. */
void eb_begin_call(eb_doc* doc);
void eb_check_abort(eb_doc* doc);

#endif
