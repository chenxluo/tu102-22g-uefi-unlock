/* SPDX-License-Identifier: GPL-2.0-only */
/* CPU-only PCI bridge range candidate classifier.
 *
 * The result is a range-based candidate set, not a proven PCIe topology chain.
 * It does not establish transaction forwarding, IOMMU state, or DMA reachability.
 */
#ifndef TU102_DMA_PATH_H
#define TU102_DMA_PATH_H

#include <stddef.h>
#include <stdint.h>

#define TU102_DMA_PATH_MAX_CANDIDATES 16u
#define TU102_DMA_PATH_CLASS_PCI_TO_PCI_BRIDGE 0x0604u
#define TU102_DMA_PATH_REACHABILITY_NOTE "unverified"

typedef struct {
    uint64_t location_raw_status;
    uint16_t segment;
    uint8_t bus;
    uint8_t device;
    uint8_t function;
    uint8_t values_valid;
} TU102_DMA_PATH_BDF;

typedef struct {
    uint64_t location;
    uint64_t identity;
    uint64_t class_code;
    uint64_t header_type;
    uint64_t bus_numbers;
    uint64_t command;
    uint64_t pci_status;
} TU102_DMA_PATH_RAW_STATUS;

typedef struct {
    TU102_DMA_PATH_BDF bdf;
    uint16_t vendor_id;
    uint16_t device_id;
    uint16_t class_code;
    uint8_t header_type;
    uint8_t primary_bus;
    uint8_t secondary_bus;
    uint8_t subordinate_bus;
    uint16_t command;
    uint16_t pci_status;
    TU102_DMA_PATH_RAW_STATUS raw;
} TU102_DMA_PATH_BRIDGE_RECORD;

typedef struct {
    TU102_DMA_PATH_BDF bdf;
    TU102_DMA_PATH_BDF observed_bdf;
    uint16_t command;
    uint16_t pci_status;
    uint64_t command_raw_status;
    uint64_t pci_status_raw_status;
    uint8_t location_matches_expected;
} TU102_DMA_PATH_ENDPOINT_RECORD;

typedef enum {
    TU102_DMA_PATH_INVALID_INPUT = 0,
    TU102_DMA_PATH_NO_CANDIDATE,
    TU102_DMA_PATH_CANDIDATE_SET_ONLY,
    TU102_DMA_PATH_INCOMPLETE,
    TU102_DMA_PATH_AMBIGUOUS,
    TU102_DMA_PATH_INVALID_RANGE,
    TU102_DMA_PATH_OVERFLOW
} TU102_DMA_PATH_CLASSIFICATION;

#define TU102_DMA_PATH_FLAG_DUPLICATE_BDF       0x0001u
#define TU102_DMA_PATH_FLAG_EQUAL_WIDTH         0x0002u
#define TU102_DMA_PATH_FLAG_NON_NESTED           0x0004u
#define TU102_DMA_PATH_FLAG_INVALID_RANGE        0x0008u
#define TU102_DMA_PATH_FLAG_UNKNOWN_FIELD        0x0010u
#define TU102_DMA_PATH_FLAG_CANDIDATE_OVERFLOW   0x0020u

typedef struct {
    TU102_DMA_PATH_ENDPOINT_RECORD endpoint;
    TU102_DMA_PATH_BRIDGE_RECORD candidates[TU102_DMA_PATH_MAX_CANDIDATES];
    uint64_t scan_raw_status;
    uint64_t free_raw_status;
    uint64_t first_read_raw_status;
    uint32_t candidates_total;
    uint32_t candidates_logged;
    uint32_t omitted_candidates;
    uint32_t read_errors;
    uint32_t same_segment_devices;
    uint32_t scan_handles_total;
    uint32_t scan_handles_scanned;
    uint32_t omitted_scan_handles;
    uint32_t location_value_anomalies;
    uint32_t flags;
    uint8_t complete;
    uint8_t classification;
} TU102_DMA_PATH_SNAPSHOT;

static int tu102_dma_path_status_ok(uint64_t raw_status)
{
    /* UEFI EFI_SUCCESS is zero. A failed read never makes its value usable. */
    return raw_status == 0u;
}

static int tu102_dma_path_record_complete(const TU102_DMA_PATH_BRIDGE_RECORD *r)
{
    return r->bdf.values_valid && tu102_dma_path_status_ok(r->raw.location) &&
           tu102_dma_path_status_ok(r->raw.identity) &&
           tu102_dma_path_status_ok(r->raw.class_code) &&
           tu102_dma_path_status_ok(r->raw.header_type) &&
           tu102_dma_path_status_ok(r->raw.bus_numbers) &&
           tu102_dma_path_status_ok(r->raw.command) &&
           tu102_dma_path_status_ok(r->raw.pci_status);
}

static int tu102_dma_path_candidate(const TU102_DMA_PATH_ENDPOINT_RECORD *endpoint,
                                    const TU102_DMA_PATH_BRIDGE_RECORD *r,
                                    int *range_known, uint16_t *width)
{
    *range_known = 0;
    *width = 0u;
    if (!tu102_dma_path_status_ok(r->raw.location) || !r->bdf.values_valid) return 1;
    if (r->bdf.segment != endpoint->bdf.segment) return 0;
    if (!tu102_dma_path_status_ok(r->raw.class_code)) return 1;
    if (r->class_code != TU102_DMA_PATH_CLASS_PCI_TO_PCI_BRIDGE) return 0;
    if (!tu102_dma_path_status_ok(r->raw.bus_numbers)) return 1;
    if (r->primary_bus > r->secondary_bus ||
        r->secondary_bus > r->subordinate_bus) return 1;
    *range_known = 1;
    *width = (uint16_t)(r->subordinate_bus - r->secondary_bus);
    return endpoint->bdf.bus >= r->secondary_bus &&
           endpoint->bdf.bus <= r->subordinate_bus;
}

static int tu102_dma_path_before(const TU102_DMA_PATH_BRIDGE_RECORD *a,
                                const TU102_DMA_PATH_BRIDGE_RECORD *b)
{
    int a_known = a->bdf.values_valid &&
        tu102_dma_path_status_ok(a->raw.class_code) &&
        a->class_code == TU102_DMA_PATH_CLASS_PCI_TO_PCI_BRIDGE &&
        tu102_dma_path_status_ok(a->raw.bus_numbers) &&
        a->primary_bus <= a->secondary_bus &&
        a->secondary_bus <= a->subordinate_bus;
    int b_known = b->bdf.values_valid &&
        tu102_dma_path_status_ok(b->raw.class_code) &&
        b->class_code == TU102_DMA_PATH_CLASS_PCI_TO_PCI_BRIDGE &&
        tu102_dma_path_status_ok(b->raw.bus_numbers) &&
        b->primary_bus <= b->secondary_bus &&
        b->secondary_bus <= b->subordinate_bus;
    uint16_t a_width = a_known ? (uint16_t)(a->subordinate_bus - a->secondary_bus) : 0u;
    uint16_t b_width = b_known ? (uint16_t)(b->subordinate_bus - b->secondary_bus) : 0u;
    if (a_known != b_known) return a_known;
    if (a_known && a_width != b_width) return a_width < b_width;
    if (tu102_dma_path_status_ok(a->raw.location) !=
        tu102_dma_path_status_ok(b->raw.location))
        return tu102_dma_path_status_ok(a->raw.location);
    if (a->bdf.bus != b->bdf.bus) return a->bdf.bus < b->bdf.bus;
    if (a->bdf.device != b->bdf.device) return a->bdf.device < b->bdf.device;
    return a->bdf.function < b->bdf.function;
}

static const char *tu102_dma_path_classification_name(unsigned value)
{
    switch (value) {
    case TU102_DMA_PATH_NO_CANDIDATE: return "no_candidate";
    case TU102_DMA_PATH_CANDIDATE_SET_ONLY: return "candidate_set_only";
    case TU102_DMA_PATH_INCOMPLETE: return "incomplete";
    case TU102_DMA_PATH_AMBIGUOUS: return "ambiguous";
    case TU102_DMA_PATH_INVALID_RANGE: return "invalid_range";
    case TU102_DMA_PATH_OVERFLOW: return "overflow";
    case TU102_DMA_PATH_INVALID_INPUT:
    default: return "invalid_input";
    }
}

/* `records` contains same-segment bridge candidates plus any records with an
 * unreadable location/class/range that could therefore be a candidate.
 * `omitted_candidates` is set by a bounded collector when it could not retain
 * every possible candidate. No parent pointer or physical chain is produced.
 */
static int tu102_dma_path_classify(
    const TU102_DMA_PATH_ENDPOINT_RECORD *endpoint,
    const TU102_DMA_PATH_BRIDGE_RECORD *records, size_t record_count,
    uint32_t omitted_candidates, int scan_complete,
    TU102_DMA_PATH_SNAPSHOT *out)
{
    size_t i, j;
    uint32_t total = 0u;
    uint32_t flags = 0u;
    uint8_t complete;

    if (!out) return -1;
    for (i = 0; i < sizeof(*out); ++i) ((unsigned char *)out)[i] = 0u;
    out->classification = TU102_DMA_PATH_INVALID_INPUT;
    if (!endpoint || (record_count && !records)) return -1;
    out->endpoint = *endpoint;
    complete = (uint8_t)(scan_complete != 0 &&
        endpoint->bdf.values_valid &&
        tu102_dma_path_status_ok(endpoint->bdf.location_raw_status) &&
        tu102_dma_path_status_ok(endpoint->command_raw_status) &&
        tu102_dma_path_status_ok(endpoint->pci_status_raw_status));
    if (!complete) flags |= TU102_DMA_PATH_FLAG_UNKNOWN_FIELD;

    for (i = 0; i < record_count; ++i) {
        int range_known;
        uint16_t width;
        int is_candidate = tu102_dma_path_candidate(endpoint, &records[i],
                                                     &range_known, &width);
        if (!is_candidate) continue;
        (void)width;
        if (total != 0xffffffffu) ++total;
        if (!range_known || !tu102_dma_path_record_complete(&records[i])) {
            complete = 0u;
            flags |= TU102_DMA_PATH_FLAG_UNKNOWN_FIELD;
        }
        if (tu102_dma_path_status_ok(records[i].raw.bus_numbers) &&
            records[i].class_code == TU102_DMA_PATH_CLASS_PCI_TO_PCI_BRIDGE &&
            (records[i].primary_bus > records[i].secondary_bus ||
             records[i].secondary_bus > records[i].subordinate_bus))
            flags |= TU102_DMA_PATH_FLAG_INVALID_RANGE;
        if (total <= TU102_DMA_PATH_MAX_CANDIDATES)
            out->candidates[total - 1u] = records[i];
    }
    out->omitted_candidates = omitted_candidates;
    if (omitted_candidates) {
        flags |= TU102_DMA_PATH_FLAG_CANDIDATE_OVERFLOW;
        complete = 0u;
    }
    if (record_count > TU102_DMA_PATH_MAX_CANDIDATES && total > TU102_DMA_PATH_MAX_CANDIDATES) {
        flags |= TU102_DMA_PATH_FLAG_CANDIDATE_OVERFLOW;
        complete = 0u;
    }
    total = omitted_candidates > 0xffffffffu - total ? 0xffffffffu :
            total + omitted_candidates;

    out->candidates_total = total;
    out->candidates_logged = total < TU102_DMA_PATH_MAX_CANDIDATES ?
        total : TU102_DMA_PATH_MAX_CANDIDATES;
    /* Keep deterministic, narrowest-range-first ordering. Unknown ranges sort
     * after known ranges and never participate in width/nesting comparisons. */
    for (i = 1; i < out->candidates_logged; ++i) {
        TU102_DMA_PATH_BRIDGE_RECORD value = out->candidates[i];
        j = i;
        while (j && tu102_dma_path_before(&value, &out->candidates[j - 1u])) {
            out->candidates[j] = out->candidates[j - 1u];
            --j;
        }
        out->candidates[j] = value;
    }
    for (i = 0; i < out->candidates_logged; ++i) {
        const TU102_DMA_PATH_BRIDGE_RECORD *a = &out->candidates[i];
        int a_known = a->bdf.values_valid &&
            tu102_dma_path_status_ok(a->raw.class_code) &&
            tu102_dma_path_status_ok(a->raw.bus_numbers) &&
            a->class_code == TU102_DMA_PATH_CLASS_PCI_TO_PCI_BRIDGE &&
            a->primary_bus <= a->secondary_bus &&
            a->secondary_bus <= a->subordinate_bus;
        if (!tu102_dma_path_status_ok(a->raw.location)) continue;
        for (j = i + 1u; j < out->candidates_logged; ++j) {
            const TU102_DMA_PATH_BRIDGE_RECORD *b = &out->candidates[j];
            int b_known = b->bdf.values_valid &&
                tu102_dma_path_status_ok(b->raw.class_code) &&
                tu102_dma_path_status_ok(b->raw.bus_numbers) &&
                b->class_code == TU102_DMA_PATH_CLASS_PCI_TO_PCI_BRIDGE &&
                b->primary_bus <= b->secondary_bus &&
                b->secondary_bus <= b->subordinate_bus;
            if (!tu102_dma_path_status_ok(b->raw.location)) continue;
            if (a->bdf.values_valid && b->bdf.values_valid &&
                a->bdf.segment == b->bdf.segment && a->bdf.bus == b->bdf.bus &&
                a->bdf.device == b->bdf.device && a->bdf.function == b->bdf.function)
                flags |= TU102_DMA_PATH_FLAG_DUPLICATE_BDF;
            if (a_known && b_known) {
                uint16_t a_width = (uint16_t)(a->subordinate_bus - a->secondary_bus);
                uint16_t b_width = (uint16_t)(b->subordinate_bus - b->secondary_bus);
                if (a_width == b_width) flags |= TU102_DMA_PATH_FLAG_EQUAL_WIDTH;
                else if (a_width < b_width) {
                    if (a->secondary_bus < b->secondary_bus ||
                        a->subordinate_bus > b->subordinate_bus)
                        flags |= TU102_DMA_PATH_FLAG_NON_NESTED;
                } else if (b->secondary_bus < a->secondary_bus ||
                           b->subordinate_bus > a->subordinate_bus) {
                    flags |= TU102_DMA_PATH_FLAG_NON_NESTED;
                }
            }
        }
    }
    if (out->candidates_logged < total) flags |= TU102_DMA_PATH_FLAG_CANDIDATE_OVERFLOW;
    out->flags = flags;
    out->complete = complete;
    if (flags & TU102_DMA_PATH_FLAG_CANDIDATE_OVERFLOW)
        out->classification = TU102_DMA_PATH_OVERFLOW;
    else if (flags & (TU102_DMA_PATH_FLAG_DUPLICATE_BDF |
                      TU102_DMA_PATH_FLAG_EQUAL_WIDTH |
                      TU102_DMA_PATH_FLAG_NON_NESTED))
        out->classification = TU102_DMA_PATH_AMBIGUOUS;
    else if (flags & TU102_DMA_PATH_FLAG_INVALID_RANGE)
        out->classification = TU102_DMA_PATH_INVALID_RANGE;
    else if (!complete)
        out->classification = TU102_DMA_PATH_INCOMPLETE;
    else if (!total)
        out->classification = TU102_DMA_PATH_NO_CANDIDATE;
    else
        out->classification = TU102_DMA_PATH_CANDIDATE_SET_ONLY;
    return 0;
}

#endif /* TU102_DMA_PATH_H */
