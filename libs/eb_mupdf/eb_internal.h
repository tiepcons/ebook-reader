#ifndef EB_INTERNAL_H
#define EB_INTERNAL_H

#include "eb_mupdf.h"

#include <mupdf/fitz.h>

struct eb_doc {
    fz_context* ctx;
    fz_document* doc;
    int reflowable;
};

extern eb_limits eb_g_limits;

/* Maps the MuPDF error just caught on ctx to an eb_status and records its message for eb_last_error. */
eb_status eb_fail(fz_context* ctx);
eb_status eb_fail_msg(eb_status s, const char* msg);
eb_status eb_check_page(eb_doc* doc, int32_t page);

#endif
