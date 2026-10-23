/* Structural pre-checks on a book loaded by libmobi, run before mobi_parse_rawml so that hostile size fields cannot
 * drive large allocations or non-terminating loops inside libmobi. Offsets follow the MOBI/FDST/INDX/HUFF/FONT
 * record layouts that libmobi itself parses. */
#include "eb_internal.h"

#include <mobi.h>
#include <string.h>

#define TEXT_CAP_MAX ((uint64_t)128 << 20)
#define TEXT_RECORD_MAX 8192u
#define FONT_TOTAL_MAX ((uint64_t)32 << 20)
#define INDX_ENTRIES_PER_RECORD 10000u
#define TAGX_CONTROL_BYTES_MAX 8u
#define HUFF_TABLE_ENTRIES 256u

static uint32_t be32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

static eb_status reject(const char* what) { return eb_fail_msg(EB_E_FORMAT, what); }

static uint64_t text_cap(size_t file_size)
{
    const uint64_t scaled = (uint64_t)file_size * 32u;
    return scaled < TEXT_CAP_MAX ? scaled : TEXT_CAP_MAX;
}

static size_t kf8_offset(const MOBIData* m) { return mobi_get_kf8offset(m); }

static const MOBIPdbRecord* record_at(const MOBIData* m, uint64_t n)
{
    return n < UINT32_MAX ? mobi_get_record_by_seqnumber(m, (size_t)n) : NULL;
}

static eb_status check_text(const MOBIData* m, size_t file_size)
{
    const uint64_t cap = text_cap(file_size);
    if (!m->rh || !m->ph) return reject("precheck: record 0 missing");
    if (m->rh->text_record_count + 1u > m->ph->rec_count) return reject("precheck: text record count exceeds records");
    if (m->rh->text_record_size > TEXT_RECORD_MAX) return reject("precheck: text record size too large");
    if (m->rh->text_length > cap) return reject("precheck: text length too large");
    if ((uint64_t)m->rh->text_record_count * m->rh->text_record_size > cap) return reject("precheck: text size too large");
    return EB_OK;
}

static eb_status check_fdst(const MOBIData* m, size_t file_size)
{
    const size_t n = mobi_get_fdst_record_number(m);
    const MOBIPdbRecord* r = NULL;
    uint64_t prev_end = 0;
    uint32_t count = 0;
    if (n == MOBI_NOTSET) return EB_OK;
    r = record_at(m, n);
    if (!r || r->size < 12 || memcmp(r->data, "FDST", 4) != 0) return EB_OK;
    count = be32(r->data + 8);
    if (count > (r->size - 12) / 8) return reject("precheck: fdst table exceeds record");
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t start = be32(r->data + 12 + (size_t)i * 8);
        const uint32_t end = be32(r->data + 16 + (size_t)i * 8);
        if (start != prev_end || end < start) return reject("precheck: fdst sections not contiguous");
        prev_end = end;
    }
    if (prev_end > text_cap(file_size)) return reject("precheck: fdst text size too large");
    return EB_OK;
}

/* libmobi re-reads TAGX and the totals from any record while the total is still zero, so every record is checked. */
static eb_status check_indx_record(const MOBIPdbRecord* r, size_t file_size)
{
    uint32_t header_len = 0, entries = 0, total = 0;
    if (r->size < 56 || memcmp(r->data, "INDX", 4) != 0) return EB_OK;
    header_len = be32(r->data + 4);
    entries = be32(r->data + 24);
    total = be32(r->data + 36);
    if ((uint64_t)total > (uint64_t)entries * INDX_ENTRIES_PER_RECORD || total > file_size / 2)
        return reject("precheck: index entry count too large");
    if (header_len > r->size || r->size - header_len < 12 || memcmp(r->data + header_len, "TAGX", 4) != 0) return EB_OK;
    if (be32(r->data + header_len + 8) > TAGX_CONTROL_BYTES_MAX) return reject("precheck: index tag table too large");
    if (total == 0) return reject("precheck: index without entries");
    return EB_OK;
}

static eb_status check_indx(const MOBIData* m, const uint32_t* index, size_t file_size)
{
    const MOBIPdbRecord* r = NULL;
    uint32_t left = 0;
    eb_status st = EB_OK;
    if (!index || *index == MOBI_NOTSET) return EB_OK;
    r = record_at(m, (uint64_t)*index + kf8_offset(m));
    if (!r) return EB_OK;
    st = check_indx_record(r, file_size);
    if (r->size >= 28) left = be32(r->data + 24);
    for (r = r->next; st == EB_OK && r && left > 0 && left <= INDX_ENTRIES_PER_RECORD; r = r->next, --left)
        st = check_indx_record(r, file_size);
    return st;
}

static eb_status check_indices(const MOBIData* m, size_t file_size)
{
    eb_status st = EB_OK;
    if (!m->mh) return EB_OK;
    if (mobi_is_dictionary(m)) return reject("precheck: dictionaries are not supported");
    st = check_indx(m, m->mh->skeleton_index, file_size);
    if (st == EB_OK) st = check_indx(m, m->mh->fragment_index, file_size);
    if (st == EB_OK) st = check_indx(m, m->mh->guide_index, file_size);
    if (st == EB_OK) st = check_indx(m, m->mh->ncx_index, file_size);
    return st;
}

static eb_status check_fonts(const MOBIData* m)
{
    uint64_t total = 0;
    for (const MOBIPdbRecord* r = m->rec; r; r = r->next) {
        uint32_t decoded = 0;
        if (r->size < 24 || memcmp(r->data, "FONT", 4) != 0) continue;
        decoded = be32(r->data + 4);
        total += decoded;
        if (total > FONT_TOTAL_MAX) return reject("precheck: font data too large");
        if ((be32(r->data + 8) & 1u) && decoded > (uint64_t)r->size * 16u) return reject("precheck: font expansion too large");
    }
    return EB_OK;
}

static eb_status check_huff(const MOBIData* m)
{
    const MOBIPdbRecord* r = NULL;
    uint32_t data1 = 0;
    if (!m->rh || m->rh->compression_type != MOBI_COMPRESSION_HUFFCDIC || !m->mh || !m->mh->huff_rec_index) return EB_OK;
    r = record_at(m, (uint64_t)*m->mh->huff_rec_index + kf8_offset(m));
    if (!r || r->size < 12 || memcmp(r->data, "HUFF", 4) != 0) return EB_OK;
    data1 = be32(r->data + 8);
    if (data1 > r->size || r->size - data1 < HUFF_TABLE_ENTRIES * 4u) return reject("precheck: huff table truncated");
    for (uint32_t i = 0; i < HUFF_TABLE_ENTRIES; ++i) {
        const uint32_t len = be32(r->data + data1 + (size_t)i * 4) & 0x1fu;
        if (len == 0 || len > 32) return reject("precheck: huff code length invalid");
    }
    return EB_OK;
}

eb_status eb_mobi_precheck(const MOBIData* m, size_t file_size)
{
    eb_status st = check_text(m, file_size);
    if (st == EB_OK) st = check_indices(m, file_size);
    if (st == EB_OK) st = check_fdst(m, file_size);
    if (st == EB_OK) st = check_fonts(m);
    if (st == EB_OK) st = check_huff(m);
    return st;
}
