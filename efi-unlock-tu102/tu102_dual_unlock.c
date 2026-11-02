/* SPDX-License-Identifier: GPL-2.0-only
 * Experimental dual TU102 UEFI port. The transaction lineage and required
 * attribution are recorded in KohakUnlock/NOTICE.md; firmware remains subject
 * to its own terms. Default builds are deliberately non-operational.
 *
 * Adapted from KohakUnlock/kernel/tu102_core_port.h (GPL-2.0-only; Turing-
 * unlock lineage, with copyleft-next attribution retained by the upstream
 * project) and CMP40HX-Unlock (MIT; attribution retained in NOTICE.md).
 */
#include <efi.h>
#include <efilib.h>
#include <efipciio.h>
#include <efiprot.h>
#include <efidevp.h>
#ifdef TU102_DUAL_ATTEMPT_V4
#include "tu102_trace.h"
#endif

#ifdef TU102_DUAL_ATTEMPT
/* The attempt entry must not rely on console output after driver detach. */
struct GPU_TARGET;
#define Print(...) do { } while (0)
EFI_STATUS tu102_attempt_before_first_gpu_write(struct GPU_TARGET *target,
                                               UINT32 offset, UINT32 value);
EFI_STATUS tu102_attempt_after_first_gpu_write(struct GPU_TARGET *target,
                                               EFI_STATUS status);
#ifdef TU102_DUAL_ATTEMPT_V4
EFI_STATUS tu102_attempt_transaction_stage(struct GPU_TARGET *target,
                                           const char *phase, BOOLEAN before,
                                           EFI_STATUS status);
EFI_STATUS tu102_attempt_hwcfg_reset_recheck(struct GPU_TARGET *target,
                                              UINT32 before, UINT32 after,
                                              EFI_STATUS status);
EFI_STATUS tu102_attempt_hwcfg_reset_skipped(struct GPU_TARGET *target,
                                              UINT32 before,
                                              EFI_STATUS status);
EFI_STATUS tu102_attempt_hwcfg_preflight(struct GPU_TARGET *target,
                                          UINT32 blocks, UINT32 minimum,
                                          UINT32 maximum, EFI_STATUS status);
#endif
#endif

#ifndef ENABLE_EXPERIMENT
#define ENABLE_EXPERIMENT 0
#endif

#if ENABLE_EXPERIMENT != 0 && ENABLE_EXPERIMENT != 1
#error "ENABLE_EXPERIMENT must be 0 or 1"
#endif

#if ENABLE_EXPERIMENT
#if defined(__has_include)
#if __has_include("tu102_blobs.h")
#include "tu102_blobs.h"
#else
extern const unsigned char blob_fwsec_card0[39968];
extern const unsigned char blob_fwsec_card1[39968];
extern const unsigned char blob_v67_payload[];
extern const unsigned char blob_booter_ucode_prod[];
extern const unsigned char blob_gsp_rm_boot[];
extern const unsigned char blob_bl_gsp_tu102[];
#endif
#else
#include "tu102_blobs.h"
#endif
#define TX_GSP 0x110000u
#define TX_SEC 0x840000u
#define TX_PROM_APERTURE 0x300000u
#define TX_POOL_SIZE 0x2000000u
#define TX_POOL_PAGES (TX_POOL_SIZE / 4096u)
#define TX_FWSEC_OFF 0x00000u
#define TX_BOOTER_OFF 0x10000u
#define TX_SIGNATURE_OFF 0x20000u
#define TX_BOOT_OFF 0x30000u
#define TX_META_OFF 0x40000u
#define TX_RADIX_OFF 0x50000u
#define TX_DATA_OFF 0x60000u
#define TX_FWSEC_SIZE_0 39968u
#define TX_FWSEC_SIZE_1 39968u
#define TX_BOOTER_SIZE 0xe700u
#define TX_SIGNATURE_SIZE 0xfa00u
#define TX_BOOT_SIZE 0x6000u
#define TX_ELF_SIZE 0x1bfadc8u
#define TX_FB_SIZE 0x580000000ULL
#define TX_FRTS 0x57fe00000ULL
#define PCI_COMMAND_MEMORY 0x0002u
#define PCI_COMMAND_MASTER 0x0004u
#define PCI_BAR0 0x10u
#define PCI_COMMAND 0x04u
#define BAR0_REQUIRED_LENGTH 0x840628ULL
#define BAR_RESOURCE_SCAN_LIMIT 4096u

#pragma pack(1)
typedef struct {
    UINT8 Descriptor;
    UINT16 Length;
    UINT8 ResourceType;
    UINT8 GeneralFlags;
    UINT8 TypeSpecificFlags;
    UINT64 Granularity;
    UINT64 Minimum;
    UINT64 Maximum;
    UINT64 Translation;
    UINT64 AddressLength;
} TU102_ACPI_ADDRESS_SPACE_DESCRIPTOR;

typedef struct {
    UINT8 Descriptor;
    UINT8 Checksum;
} TU102_ACPI_END_TAG_DESCRIPTOR;
#pragma pack()

typedef struct {
    const CHAR16 *name;
    UINTN segment, bus, device, function;
    UINT16 vendor_id, device_id, subsystem_vendor, subsystem_device;
    UINT32 prom_desc_off, prom_image_off, fwsec_size;
    UINT32 capacity_selector, workspace, wpr_lo, wpr_hi;
    UINT32 initial_ss0, initial_ss1;
    UINT32 mapper_offset, command_offset, init_command_offset;
    UINT32 imem_size, min_falcon_blocks, sec_imem_base, sec_imem_size;
    UINT32 dmem_offset, dmem_size;
    const UINT8 *fwsec_blob;
    UINT32 desc_words[15];
    BOOLEAN hardware_validated;
    UINT32 v4_bound_ss0, v4_bound_ss1;
    UINT32 v4_detached_ss0, v4_detached_ss1, v4_detached_ss1_alt;
} TU102_PROFILE;

/* CPU-view transaction diagnostics.  These fields describe the first local
 * comparison failure; they do not imply that a device consumed the buffer. */
typedef struct {
    BOOLEAN valid;
    const char *component;
    UINT64 index;
    UINT64 offset;
    UINT64 expected;
    UINT64 actual;
    EFI_STATUS status;
} TX_INPUT_DIAGNOSTIC;

/* Profiles describe the measured inputs, not approval for this machine.
 * Only an independently validated board-specific fixture may set this true. */
static const TU102_PROFILE g_Profiles[2] = {
    {
        .name = L"Card 0 (Leadtek FWSEC profile)",
        .segment = 0, .bus = 0x17, .device = 0, .function = 0,
        .vendor_id = 0x10de, .device_id = 0x1e04,
        .subsystem_vendor = 0x10de, .subsystem_device = 0x1e04,
        .prom_desc_off = 0x3ed98, .prom_image_off = 0x3edd4,
        .fwsec_size = TX_FWSEC_SIZE_0,
        .capacity_selector = 0x2c9, .workspace = 1,
        .wpr_lo = 0x1ffffe00, .wpr_hi = 0,
        .initial_ss0 = 0, .initial_ss1 = 0,
        .mapper_offset = 0x390, .command_offset = 0x3e0,
        .init_command_offset = 0x3bc, .imem_size = 0x9800,
        .min_falcon_blocks = 0x9a, .sec_imem_base = 0x400,
        .sec_imem_size = 0x9400, .dmem_offset = 0x9800,
        .dmem_size = 0x420, .fwsec_blob = blob_fwsec_card0,
        .desc_words = {0x3c0201,0x9c20,0x9c20,0,0xe0,0,0x9800,0,
                       0x400,0x9400,0x9800,0,0x420,0x9800,0x6628},
        .hardware_validated = FALSE,
        .v4_bound_ss0 = 0, .v4_bound_ss1 = 6,
        .v4_detached_ss0 = 0, .v4_detached_ss1 = 6,
        .v4_detached_ss1_alt = 7
    },
    {
        .name = L"Card 1 (Leadtek FWSEC profile)",
        .segment = 0, .bus = 0x65, .device = 0, .function = 0,
        .vendor_id = 0x10de, .device_id = 0x1e04,
        .subsystem_vendor = 0x10de, .subsystem_device = 0x1e04,
        .prom_desc_off = 0x3ed98, .prom_image_off = 0x3edd4,
        .fwsec_size = TX_FWSEC_SIZE_1,
        .capacity_selector = 0x2c9, .workspace = 1,
        .wpr_lo = 0x1ffffe00, .wpr_hi = 0,
        .initial_ss0 = 0, .initial_ss1 = 0,
        .mapper_offset = 0x390, .command_offset = 0x3e0,
        .init_command_offset = 0x3bc, .imem_size = 0x9800,
        .min_falcon_blocks = 0x9a, .sec_imem_base = 0x400,
        .sec_imem_size = 0x9400, .dmem_offset = 0x9800,
        .dmem_size = 0x420, .fwsec_blob = blob_fwsec_card1,
        .desc_words = {0x3c0201,0x9c20,0x9c20,0,0xe0,0,0x9800,0,
                       0x400,0x9400,0x9800,0,0x420,0x9800,0x6628},
        .hardware_validated = FALSE,
        .v4_bound_ss0 = 0x02040000, .v4_bound_ss1 = 4,
        .v4_detached_ss0 = 0x02040000, .v4_detached_ss1 = 4,
        .v4_detached_ss1_alt = 4
    }
};

typedef struct GPU_TARGET {
    EFI_HANDLE handle;
    EFI_PCI_IO_PROTOCOL *pci;
    const TU102_PROFILE *profile;
    UINTN segment, bus, device, function;
    UINT64 bar0_length;
    BOOLEAN preflight_ok;
    BOOLEAN mutated;
    BOOLEAN quiescent;
    VOID *pool;
    EFI_PHYSICAL_ADDRESS dma_address;
    VOID *dma_mapping;
    BOOLEAN buffer_allocated;
    BOOLEAN buffer_mapped;
    BOOLEAN allocation_uncertain;
    BOOLEAN mapping_uncertain;
    BOOLEAN first_write_recorded;
    BOOLEAN first_write_result_recorded;
    BOOLEAN transaction_data_prepared;
    TX_INPUT_DIAGNOSTIC v4_input_diag;
#ifdef TU102_DUAL_ATTEMPT_V4
    TX_INPUT_DIAGNOSTIC v4_static_audit_diag;
#endif
    UINTN dma_mapped_bytes;
    EFI_STATUS dma_map_status;
    EFI_STATUS post_read_status[13];
    UINT32 post_read_value[13];
#ifdef TU102_DUAL_ATTEMPT_V4
    struct tu102_trace v43_trace;
    struct tu102_trace_event v43_last_poll;
    BOOLEAN v43_trace_active;
    BOOLEAN v43_last_poll_valid;
    BOOLEAN v4_selector_baseline_valid;
    UINT32 v4_postdetach_ss0, v4_postdetach_ss1;
    BOOLEAN v4_hwcfg_valid;
    UINT32 v4_hwcfg_blocks;
#endif
#ifdef TU102_HOST_TEST
    BOOLEAN host_transaction_stub;
#endif
} GPU_TARGET;

#ifdef TU102_DUAL_ATTEMPT
static const UINT32 gPostReadOffsets[13] = {
    0x1438, 0x1fa824, 0x1fa828, TX_SEC + 0x040,
    0x409664, 0x40966c, 0x409650, 0x1fa824,
    0x1fa828, TX_SEC + 0x100, TX_GSP + 0x100,
    0x409664, 0x40966c
};
#endif

static VOID record_post_read(GPU_TARGET *target, UINTN index,
                             EFI_STATUS status, UINT32 value)
{
    if (target && index < 13u) {
        target->post_read_status[index] = status;
        target->post_read_value[index] = value;
    }
}

typedef struct {
    UINT32 offset;
    UINT32 expected;
    UINT32 actual;
    EFI_STATUS status;
} GPU_PREFLIGHT_REGISTER;

typedef struct {
    EFI_STATUS pci_identity_status;
    EFI_STATUS driver_status;
    EFI_STATUS bar_status;
    EFI_STATUS pci_command_status;
    EFI_STATUS register_status;
    EFI_STATUS rom_status;
    EFI_STATUS attributes_supported_status;
    EFI_STATUS attributes_get_status;
    EFI_STATUS attributes_supported_call_status;
    EFI_STATUS attributes_get_call_status;
    BOOLEAN attributes_supported_called;
    BOOLEAN attributes_get_called;
    UINT32 pci_id;
    UINT32 pci_subsystem;
    UINT32 pci_command;
    UINT64 bar0_length;
    UINT64 supported_attributes;
    UINT64 current_attributes;
    UINTN descriptor_words_checked;
    UINTN fwsec_bytes_checked;
    BOOLEAN identity_match;
    BOOLEAN command_match;
    BOOLEAN registers_match;
    EFI_STATUS gsp_hwcfg_status;
    UINT32 gsp_hwcfg_value;
    BOOLEAN gsp_hwcfg_match;
    GPU_PREFLIGHT_REGISTER registers[13];
} GPU_PREFLIGHT_SNAPSHOT;

typedef enum {
    Tu102PreflightLegacy = 0,
    Tu102PreflightV4Bound = 1,
    Tu102PreflightV4PostDetach = 2,
    Tu102PreflightV4Execution = 3
} TU102_PREFLIGHT_PHASE;

#ifdef TU102_DUAL_ATTEMPT_V4
static void v43_trace_operation(GPU_TARGET *target, enum tu102_trace_op op,
    UINT32 offset, UINT64 value, EFI_STATUS raw, EFI_STATUS effective,
    BOOLEAN called)
{
    struct tu102_trace_event e = {0};
    if (!target || !target->v43_trace_active) return;
    e.operation = op; e.offset = offset; e.value = value;
    e.status = raw; e.effective_status = effective;
    e.is_failure = EFI_ERROR(effective); e.protocol_called = called;
    e.observed_elapsed_state = TU102_TRACE_TIME_CLOCK_UNAVAILABLE;
    if (op == TU102_TRACE_OP_STALL) e.stall_requested_us = value;
    (void)tu102_trace_record(&target->v43_trace, &e);
}

static void v43_trace_poll(GPU_TARGET *target, UINT32 offset, UINT32 last,
    UINT32 mask, UINT32 expected, UINTN count, UINTN limit, EFI_STATUS result)
{
    struct tu102_trace_event e = {0};
    const struct tu102_trace_event *saved;
    if (!target || !target->v43_trace_active) return;
    e.operation = TU102_TRACE_OP_POLL; e.offset = offset;
    e.value = last; e.last_value = last; e.mask = mask; e.expected = expected;
    e.poll_count = count; e.poll_limit = limit; e.poll_valid = 1u;
    /* This is a summary of calls already traced, never another device read. */
    e.status = EFI_SUCCESS; e.effective_status = result;
    e.is_failure = EFI_ERROR(result);
    e.observed_elapsed_state = TU102_TRACE_TIME_CLOCK_UNAVAILABLE;
    (void)tu102_trace_record(&target->v43_trace, &e);
    saved = tu102_trace_at_oldest(&target->v43_trace,
                                tu102_trace_count(&target->v43_trace) - 1u);
    if (saved) { target->v43_last_poll = *saved; target->v43_last_poll_valid = TRUE; }
}

static void v43_trace_upload(GPU_TARGET *target, UINT32 offset,
    UINT32 requested, UINT32 completed, EFI_STATUS result)
{
    struct tu102_trace_event e = {0};
    if (!target || !target->v43_trace_active) return;
    e.operation = TU102_TRACE_OP_UPLOAD; e.offset = offset;
    e.value = requested; e.last_value = completed;
    e.status = EFI_SUCCESS; e.effective_status = result;
    e.is_failure = EFI_ERROR(result);
    e.observed_elapsed_state = TU102_TRACE_TIME_CLOCK_UNAVAILABLE;
    (void)tu102_trace_record(&target->v43_trace, &e);
}
#endif

static EFI_STATUS v43_stall(GPU_TARGET *target, UINTN microseconds)
{
    EFI_STATUS status;
    if (!BS || !BS->Stall) return EFI_UNSUPPORTED;
    status = uefi_call_wrapper(BS->Stall, 1, microseconds);
#ifdef TU102_DUAL_ATTEMPT_V4
    v43_trace_operation(target, TU102_TRACE_OP_STALL, 0, microseconds,
                        status, status, TRUE);
#else
    (void)target;
#endif
    return status;
}

static EFI_STATUS v43_poll_result(GPU_TARGET *target, UINT32 offset,
    UINT32 last, UINT32 mask, UINT32 expected, UINTN count, UINTN limit,
    EFI_STATUS result)
{
#ifdef TU102_DUAL_ATTEMPT_V4
    v43_trace_poll(target, offset, last, mask, expected, count, limit, result);
#else
    (void)target; (void)offset; (void)last; (void)mask; (void)expected;
    (void)count; (void)limit;
#endif
    return result;
}

static EFI_STATUS v43_upload_result(GPU_TARGET *target, UINT32 offset,
    UINT32 requested, UINT32 completed, EFI_STATUS result)
{
#ifdef TU102_DUAL_ATTEMPT_V4
    v43_trace_upload(target, offset, requested, completed, result);
#else
    (void)target; (void)offset; (void)requested; (void)completed;
#endif
    return result;
}

static EFI_STATUS pci_read32(EFI_PCI_IO_PROTOCOL *pci, UINT32 offset, UINT32 *value)
{
    if (!pci || !value || (offset & 3u)) return EFI_INVALID_PARAMETER;
    if (!pci->Pci.Read) return EFI_UNSUPPORTED;
    *value = 0;
    return uefi_call_wrapper(pci->Pci.Read, 5, pci, EfiPciIoWidthUint32,
                             offset, 1, value);
}

static EFI_STATUS gpu_read32(GPU_TARGET *target, UINT32 offset, UINT32 *value)
{
    EFI_STATUS status;
    if (!target || !target->pci || !value || (offset & 3u) ||
        !target->bar0_length || (UINT64)offset + sizeof(UINT32) > target->bar0_length)
        return EFI_INVALID_PARAMETER;
    if (!target->pci->Mem.Read) return EFI_UNSUPPORTED;
    *value = 0;
    status = uefi_call_wrapper(target->pci->Mem.Read, 6, target->pci,
                               EfiPciIoWidthUint32, 0, offset, 1, value);
    {
        EFI_STATUS effective = EFI_ERROR(status) ? status : EFI_SUCCESS;
        if (!EFI_ERROR(status) &&
            (*value == 0xffffffffu || (*value & 0xffff0000u) == 0xbadf0000u))
            effective = EFI_DEVICE_ERROR;
#ifdef TU102_DUAL_ATTEMPT_V4
        v43_trace_operation(target, TU102_TRACE_OP_READ, offset, *value,
                            status, effective, TRUE);
#endif
        return effective;
    }
}

#ifdef TU102_DUAL_ATTEMPT_V4
static BOOLEAN gpu_write_allowed(UINT32 offset)
{
    UINT32 base = offset & 0xffff0000u;
    UINT32 reg = offset & 0xffffu;
    if (base != TX_GSP && base != TX_SEC) return FALSE;
    switch (reg) {
    case 0x040: case 0x044: case 0x084: case 0x100: case 0x104:
    case 0x10c: case 0x130: case 0x180: case 0x184: case 0x188:
    case 0x1c0: case 0x1c4: case 0x3c0: case 0x610: case 0x624:
        return TRUE;
    default:
        return FALSE;
    }
}
#endif

static EFI_STATUS gpu_write32(GPU_TARGET *target, UINT32 offset, UINT32 value)
{
    EFI_STATUS status;
    if (!target || !target->pci || (offset & 3u) ||
        !target->bar0_length || (UINT64)offset + sizeof(UINT32) > target->bar0_length)
        return EFI_INVALID_PARAMETER;
    if (!target->pci->Mem.Write || !target->pci->Flush) return EFI_UNSUPPORTED;
#ifdef TU102_DUAL_ATTEMPT_V4
    if (!gpu_write_allowed(offset)) {
        v43_trace_operation(target, TU102_TRACE_OP_WRITE, offset, value,
                            EFI_NOT_READY, EFI_ACCESS_DENIED, FALSE);
        return EFI_ACCESS_DENIED;
    }
#endif
#ifdef TU102_DUAL_ATTEMPT
    if (!target->first_write_recorded) {
        status = tu102_attempt_before_first_gpu_write(target, offset, value);
        if (EFI_ERROR(status)) return status;
        target->first_write_recorded = TRUE;
    }
#endif
    target->mutated = TRUE; /* a failed MMIO write may have reached the device */
    status = uefi_call_wrapper(target->pci->Mem.Write, 6, target->pci,
                               EfiPciIoWidthUint32, 0, offset, 1, &value);
#ifdef TU102_DUAL_ATTEMPT_V4
    v43_trace_operation(target, TU102_TRACE_OP_WRITE, offset, value, status, status, TRUE);
#endif
    if (!EFI_ERROR(status)) {
        __sync_synchronize();
        status = uefi_call_wrapper(target->pci->Flush, 1, target->pci);
#ifdef TU102_DUAL_ATTEMPT_V4
        v43_trace_operation(target, TU102_TRACE_OP_FLUSH, offset, 0, status, status, TRUE);
#endif
    }
#ifdef TU102_DUAL_ATTEMPT
    if (target->first_write_recorded && target->mutated &&
        !target->first_write_result_recorded) {
        EFI_STATUS journal_status = tu102_attempt_after_first_gpu_write(target, status);
        if (!EFI_ERROR(status) && EFI_ERROR(journal_status)) status = journal_status;
        target->first_write_result_recorded = TRUE;
    }
#endif
    return status;
}

static EFI_STATUS read_prom_words(GPU_TARGET *target, UINT32 offset,
                                  const UINT32 *expected, UINTN words,
                                  GPU_PREFLIGHT_SNAPSHOT *snapshot)
{
    UINT32 scratch[64];
    UINTN i, count;
    EFI_STATUS status;
    if (!target || !target->pci || !target->pci->Mem.Read || !target->bar0_length ||
        (UINT64)offset + (UINT64)words * sizeof(UINT32) > target->bar0_length)
        return EFI_INVALID_PARAMETER;
    while (words) {
        count = words > 64 ? 64 : words;
        status = uefi_call_wrapper(target->pci->Mem.Read, 6, target->pci,
                                   EfiPciIoWidthUint32, 0, offset, count, scratch);
        if (EFI_ERROR(status)) return status;
        for (i = 0; i < count; ++i) {
            if (scratch[i] != expected[i]) return EFI_SECURITY_VIOLATION;
            if (snapshot) ++snapshot->descriptor_words_checked;
        }
        offset += (UINT32)(count * sizeof(UINT32));
        expected += count;
        words -= count;
    }
    return EFI_SUCCESS;
}

static EFI_STATUS validate_target_rom(GPU_TARGET *target,
                                     GPU_PREFLIGHT_SNAPSHOT *snapshot)
{
    if (!target || !target->pci || !target->pci->Mem.Read || !target->profile)
        return EFI_UNSUPPORTED;
    const TU102_PROFILE *p = target->profile;
    UINT32 actual[64];
    UINTN i, count, done = 0;
    EFI_STATUS status = read_prom_words(target, TX_PROM_APERTURE + p->prom_desc_off,
                                        p->desc_words, 15, snapshot);
    if (EFI_ERROR(status)) return status;
    while (done < p->fwsec_size) {
        count = (p->fwsec_size - done) / sizeof(UINT32);
        if (count > sizeof(actual) / sizeof(actual[0]))
            count = sizeof(actual) / sizeof(actual[0]);
        UINT32 read_offset = TX_PROM_APERTURE + p->prom_image_off + done;
        if ((UINT64)read_offset + (UINT64)count * sizeof(UINT32) > target->bar0_length)
            return EFI_INVALID_PARAMETER;
        status = uefi_call_wrapper(target->pci->Mem.Read, 6, target->pci,
                                   EfiPciIoWidthUint32, 0, read_offset,
                                   count, actual);
        if (EFI_ERROR(status)) return status;
        for (i = 0; i < count; ++i) {
            const UINT8 *b = p->fwsec_blob + done + i * 4;
            UINT32 expected = (UINT32)b[0] | ((UINT32)b[1] << 8) |
                              ((UINT32)b[2] << 16) | ((UINT32)b[3] << 24);
            if (actual[i] != expected) return EFI_SECURITY_VIOLATION;
            if (snapshot) snapshot->fwsec_bytes_checked += sizeof(UINT32);
        }
        done += count * sizeof(UINT32);
    }
    return EFI_SUCCESS;
}

static EFI_STATUS check_driver_binding(GPU_TARGET *target)
{
    EFI_OPEN_PROTOCOL_INFORMATION_ENTRY *entries = NULL;
    UINTN count = 0, i;
    VOID *interface = NULL;
    EFI_STATUS status;
    status = uefi_call_wrapper(BS->HandleProtocol, 3, target->handle,
                               &gEfiGraphicsOutputProtocolGuid, &interface);
    if (!EFI_ERROR(status)) {
        Print(L"Active GOP found on %s; refuse. Keep the primary display on a separate GPU/environment.\n",
              target->profile->name);
        return EFI_ACCESS_DENIED;
    }
    if (status != EFI_NOT_FOUND && status != EFI_UNSUPPORTED) return status;
    status = uefi_call_wrapper(BS->OpenProtocolInformation, 4, target->handle,
                               &(EFI_GUID)EFI_PCI_IO_PROTOCOL_GUID, &entries, &count);
    if (EFI_ERROR(status)) return status;
    for (i = 0; i < count; ++i) {
        if (entries[i].OpenCount &&
            (entries[i].Attributes & (EFI_OPEN_PROTOCOL_BY_DRIVER |
                                      EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER))) {
            Print(L"A UEFI driver is bound to %s; refuse.\n", target->profile->name);
            status = EFI_ACCESS_DENIED;
            break;
        }
    }
    if (entries) {
        EFI_STATUS free_status = uefi_call_wrapper(BS->FreePool, 1, entries);
        if (!EFI_ERROR(status) && EFI_ERROR(free_status)) status = free_status;
    }
    return status;
}

static EFI_STATUS validate_bar0_resources(GPU_TARGET *target)
{
    UINT64 supports = 0;
    VOID *resources = NULL;
    const UINT8 *cursor;
    UINTN scanned = 0;
    BOOLEAN found = FALSE, ended = FALSE;
    EFI_STATUS status;
    if (!target || !target->pci || !target->pci->GetBarAttributes)
        return EFI_UNSUPPORTED;
    status = uefi_call_wrapper(target->pci->GetBarAttributes, 4, target->pci,
                               0, &supports, &resources);
    if (EFI_ERROR(status)) {
        if (resources) (void)uefi_call_wrapper(BS->FreePool, 1, resources);
        return status;
    }
    /* GetBarAttributes.Supports contains policy bits for SetBarAttributes,
     * not a memory-resource indicator. The ACPI descriptor is authoritative. */
    (void)supports;
    if (!resources) {
        if (resources) (void)uefi_call_wrapper(BS->FreePool, 1, resources);
        return EFI_UNSUPPORTED;
    }
    cursor = (const UINT8 *)resources;
    while (scanned < BAR_RESOURCE_SCAN_LIMIT) {
        UINT8 descriptor = cursor[0];
        UINTN item_size;
        if ((descriptor & 0x80u) != 0) {
            UINT16 payload_length = (UINT16)cursor[1] | ((UINT16)cursor[2] << 8);
            item_size = 3u + payload_length;
            if (item_size > BAR_RESOURCE_SCAN_LIMIT - scanned) {
                status = EFI_COMPROMISED_DATA;
                goto done;
            }
            if (descriptor == 0x8au) {
                const TU102_ACPI_ADDRESS_SPACE_DESCRIPTOR *address;
                if (payload_length != sizeof(*address) - 3u) {
                    status = EFI_COMPROMISED_DATA;
                    goto done;
                }
                address = (const TU102_ACPI_ADDRESS_SPACE_DESCRIPTOR *)cursor;
                if (address->ResourceType == 0 &&
                    address->AddressLength >= BAR0_REQUIRED_LENGTH)
                    found = TRUE;
            }
        } else {
            UINTN payload_length = descriptor & 0x07u;
            item_size = 1u + payload_length;
            if (item_size > BAR_RESOURCE_SCAN_LIMIT - scanned) {
                status = EFI_COMPROMISED_DATA;
                goto done;
            }
            if ((descriptor & 0x78u) == 0x78u) {
                if (descriptor != 0x79u || payload_length != 1u) {
                    status = EFI_COMPROMISED_DATA;
                    goto done;
                }
                ended = TRUE;
                break;
            }
        }
        cursor += item_size;
        scanned += item_size;
    }
    status = (ended && found) ? EFI_SUCCESS : EFI_UNSUPPORTED;
    if (!EFI_ERROR(status)) {
        /* Re-read the matching descriptor to retain the verified resource size. */
        cursor = (const UINT8 *)resources;
        while (cursor[0] != 0x79u) {
            if (cursor[0] == 0x8au) {
                const TU102_ACPI_ADDRESS_SPACE_DESCRIPTOR *address =
                    (const TU102_ACPI_ADDRESS_SPACE_DESCRIPTOR *)cursor;
                if (address->ResourceType == 0 &&
                    address->AddressLength >= BAR0_REQUIRED_LENGTH) {
                    target->bar0_length = address->AddressLength;
                    break;
                }
            }
            cursor += (cursor[0] & 0x80u)
                ? 3u + ((UINTN)cursor[1] | ((UINTN)cursor[2] << 8))
                : 1u + (cursor[0] & 0x07u);
        }
    }
done:
    {
        EFI_STATUS free_status = uefi_call_wrapper(BS->FreePool, 1, resources);
        if (!EFI_ERROR(status) && EFI_ERROR(free_status)) status = free_status;
    }
    return status;
}

static UINT64 decode_capacity(UINT32 value)
{
    UINT64 bytes = (UINT64)((value >> 4) & 63u) << ((value & 15u) + 20u);
    if (value & (1u << 30)) bytes = (bytes / 16u) * 15u;
    return bytes;
}

static EFI_STATUS preflight_target_required_phase(GPU_TARGET *target,
                                           GPU_PREFLIGHT_SNAPSHOT *snapshot,
                                           UINT32 required_command_mask,
                                           TU102_PREFLIGHT_PHASE phase)
{
    static const UINT32 offsets[13] = {
        0, 0x100ce0, 0x625f04, 0x1fa824, 0x1fa828,
        0x409664, 0x40966c, 0x409650, 0x409660, 0x409668,
        0x118234, TX_GSP + 0x100, TX_SEC + 0x100
    };
    UINT32 expected[13];
    UINTN i;
    UINT32 id, sub, command;
#ifdef TU102_DUAL_ATTEMPT_V4
    UINT32 observed_ss0 = 0, observed_ss1 = 0;
    UINT32 gsp_hwcfg_blocks = 0;
    UINT64 supported_attributes = 0, current_attributes = 0;
#endif
    BOOLEAN registers_match = TRUE;
    EFI_STATUS status, register_status = EFI_SUCCESS, rom_status;
    const TU102_PROFILE *p = target->profile;
    if (snapshot) {
        UINT8 *b = (UINT8 *)snapshot;
        UINTN n;
        for (n = 0; n < sizeof(*snapshot); ++n) b[n] = 0;
        snapshot->pci_identity_status = EFI_NOT_READY;
        snapshot->driver_status = EFI_NOT_READY;
        snapshot->bar_status = EFI_NOT_READY;
        snapshot->pci_command_status = EFI_NOT_READY;
        snapshot->register_status = EFI_NOT_READY;
        snapshot->rom_status = EFI_NOT_READY;
        snapshot->attributes_supported_status = EFI_NOT_READY;
        snapshot->attributes_get_status = EFI_NOT_READY;
        snapshot->attributes_supported_call_status = EFI_NOT_READY;
        snapshot->attributes_get_call_status = EFI_NOT_READY;
        snapshot->gsp_hwcfg_status = EFI_NOT_READY;
        for (UINTN n = 0; n < 13u; ++n) {
            snapshot->registers[n].status = EFI_NOT_READY;
        }
    }
    target->preflight_ok = FALSE;
    status = pci_read32(target->pci, 0, &id);
    if (snapshot) snapshot->pci_identity_status = status;
    if (EFI_ERROR(status)) return status;
    if (snapshot) snapshot->pci_id = id;
    status = pci_read32(target->pci, 0x2c, &sub);
    if (snapshot) snapshot->pci_identity_status = status;
    if (snapshot && !EFI_ERROR(status)) snapshot->pci_subsystem = sub;
    if (EFI_ERROR(status)) return status;
    if ((id & 0xffffu) != p->vendor_id || (id >> 16) != p->device_id ||
        (sub & 0xffffu) != p->subsystem_vendor || (sub >> 16) != p->subsystem_device) {
        if (snapshot) snapshot->pci_identity_status = EFI_SECURITY_VIOLATION;
        return EFI_SECURITY_VIOLATION;
    }
    if (snapshot) snapshot->identity_match = TRUE;
#ifdef TU102_DUAL_ATTEMPT_V4
    if (phase != Tu102PreflightLegacy) {
        UINT64 required_supported = EFI_PCI_IO_ATTRIBUTE_MEMORY |
                                    EFI_PCI_IO_ATTRIBUTE_DUAL_ADDRESS_CYCLE;
        UINT64 required_current = EFI_PCI_IO_ATTRIBUTE_MEMORY;
        /* The bound observation can precede DAC enable; later phases cannot. */
        if (phase != Tu102PreflightV4Bound)
            required_current |= EFI_PCI_IO_ATTRIBUTE_DUAL_ADDRESS_CYCLE;
        if (!target->pci->Attributes) {
            if (snapshot) snapshot->attributes_supported_status = EFI_UNSUPPORTED;
            return EFI_UNSUPPORTED;
        }
        status = uefi_call_wrapper(target->pci->Attributes, 4, target->pci,
            EfiPciIoAttributeOperationSupported, 0, &supported_attributes);
        if (snapshot) {
            snapshot->attributes_supported_status = status;
            snapshot->attributes_supported_call_status = status;
            snapshot->attributes_supported_called = TRUE;
            snapshot->supported_attributes = supported_attributes;
        }
        if (EFI_ERROR(status)) return status;
        if ((supported_attributes & required_supported) != required_supported) {
            if (snapshot) snapshot->attributes_supported_status = EFI_UNSUPPORTED;
            return EFI_UNSUPPORTED;
        }
        status = uefi_call_wrapper(target->pci->Attributes, 4, target->pci,
            EfiPciIoAttributeOperationGet, 0, &current_attributes);
        if (snapshot) {
            snapshot->attributes_get_status = status;
            snapshot->attributes_get_call_status = status;
            snapshot->attributes_get_called = TRUE;
            snapshot->current_attributes = current_attributes;
        }
        if (EFI_ERROR(status)) return status;
        if ((current_attributes & required_current) != required_current ||
            (!!(current_attributes & EFI_PCI_IO_ATTRIBUTE_BUS_MASTER) !=
             !!(required_command_mask & PCI_COMMAND_MASTER))) {
            if (snapshot) snapshot->attributes_get_status = EFI_ACCESS_DENIED;
            return EFI_ACCESS_DENIED;
        }
    }
#endif
    if (phase == Tu102PreflightV4Bound) {
        /* The v4 caller has already matched the exact nvgop agent. This pass
         * is observational and intentionally leaves that driver connected. */
        status = EFI_SUCCESS;
    } else {
        status = check_driver_binding(target);
    }
    if (snapshot) snapshot->driver_status = status;
    if (EFI_ERROR(status)) return status;
    status = validate_bar0_resources(target);
    if (snapshot) {
        snapshot->bar_status = status;
        snapshot->bar0_length = target->bar0_length;
    }
    if (EFI_ERROR(status)) return status;
    status = pci_read32(target->pci, PCI_COMMAND, &command);
    if (snapshot) snapshot->pci_command_status = status;
    if (EFI_ERROR(status)) return status;
    if (snapshot) snapshot->pci_command = command;
    if (command == 0xffffffffu ||
        (command & required_command_mask) != required_command_mask) {
        if (snapshot) snapshot->pci_command_status = EFI_ACCESS_DENIED;
        return EFI_ACCESS_DENIED;
    }
#ifdef TU102_DUAL_ATTEMPT_V4
    if (phase != Tu102PreflightLegacy &&
        (!!(command & PCI_COMMAND_MASTER) !=
         !!(required_command_mask & PCI_COMMAND_MASTER))) {
        if (snapshot) snapshot->pci_command_status = EFI_ACCESS_DENIED;
        return EFI_ACCESS_DENIED;
    }
#endif
    if (snapshot) snapshot->command_match = TRUE;

    expected[0] = 0x162000a1;
    expected[1] = p->capacity_selector;
    expected[2] = p->workspace;
    expected[3] = p->wpr_lo;
    expected[4] = p->wpr_hi;
#ifdef TU102_DUAL_ATTEMPT_V4
    if (phase == Tu102PreflightV4Bound) {
        expected[5] = p->v4_bound_ss0;
        expected[6] = p->v4_bound_ss1;
    } else if (phase == Tu102PreflightV4PostDetach) {
        expected[5] = p->v4_detached_ss0;
        expected[6] = p->v4_detached_ss1;
    } else if (phase == Tu102PreflightV4Execution) {
        if (!target->v4_selector_baseline_valid) return EFI_NOT_READY;
        expected[5] = target->v4_postdetach_ss0;
        expected[6] = target->v4_postdetach_ss1;
    } else
#endif
    {
        expected[5] = p->initial_ss0;
        expected[6] = p->initial_ss1;
    }
    expected[7] = 0xffffff8f;
    expected[8] = 0x2f3;
    expected[9] = 0x00400080;
    expected[10] = 0x3ff;
    expected[11] = 0x10;
    expected[12] = 0x10;
    for (i = 0; i < 13; ++i) {
        UINT32 actual = 0;
        status = gpu_read32(target, offsets[i], &actual);
        if (snapshot) {
            snapshot->registers[i].offset = offsets[i];
            snapshot->registers[i].expected = expected[i];
            snapshot->registers[i].actual = actual;
            snapshot->registers[i].status = status;
        }
        if (EFI_ERROR(status)) {
            register_status = status;
            registers_match = FALSE;
            break;
        }
        if (i == 1 && decode_capacity(actual) != TX_FB_SIZE) registers_match = FALSE;
        if (i == 10) {
            if ((actual & 0x3ffu) != expected[i]) registers_match = FALSE;
        } else if (i == 5) {
#ifdef TU102_DUAL_ATTEMPT_V4
            observed_ss0 = actual;
            if (snapshot) snapshot->registers[i].expected = actual;
#else
            if (actual != expected[i]) registers_match = FALSE;
#endif
        } else if (i == 6) {
#ifdef TU102_DUAL_ATTEMPT_V4
            observed_ss1 = actual;
            if (snapshot) snapshot->registers[i].expected = actual;
#else
            if (actual != expected[i]) registers_match = FALSE;
#endif
        } else if (actual != expected[i]) registers_match = FALSE;
    }
#ifdef TU102_DUAL_ATTEMPT_V4
    if (phase != Tu102PreflightLegacy) {
        UINT32 hwcfg = 0;
        UINT32 blocks;
        status = gpu_read32(target, TX_GSP + 0x108, &hwcfg);
        if (snapshot) {
            snapshot->gsp_hwcfg_status = status;
            snapshot->gsp_hwcfg_value = hwcfg;
        }
        if (EFI_ERROR(status)) return status;
        blocks = hwcfg & 0x1ffu;
        if (blocks < p->min_falcon_blocks || blocks > 0x100u) {
            if (snapshot) snapshot->gsp_hwcfg_status = EFI_UNSUPPORTED;
            return EFI_UNSUPPORTED;
        }
        if (phase == Tu102PreflightV4Execution &&
            blocks != target->v4_hwcfg_blocks) {
            if (snapshot) snapshot->gsp_hwcfg_status = EFI_SECURITY_VIOLATION;
            return EFI_SECURITY_VIOLATION;
        }
        gsp_hwcfg_blocks = blocks;
        if (snapshot) snapshot->gsp_hwcfg_match = TRUE;
    }
#endif
    if (snapshot) {
        snapshot->register_status = register_status;
        snapshot->registers_match = registers_match;
    }
    if (EFI_ERROR(register_status)) return register_status;
    if (!registers_match && !snapshot) return EFI_SECURITY_VIOLATION;
    status = validate_target_rom(target, snapshot);
    rom_status = status;
    if (snapshot) snapshot->rom_status = rom_status;
    if (EFI_ERROR(rom_status)) return rom_status;
    if (!registers_match) return EFI_SECURITY_VIOLATION;
#ifdef TU102_DUAL_ATTEMPT_V4
    if (phase == Tu102PreflightV4PostDetach) {
        target->v4_hwcfg_blocks = gsp_hwcfg_blocks;
        target->v4_hwcfg_valid = TRUE;
        target->v4_postdetach_ss0 = observed_ss0;
        target->v4_postdetach_ss1 = observed_ss1;
        target->v4_selector_baseline_valid = TRUE;
    }
#endif
    target->preflight_ok = TRUE;
    return EFI_SUCCESS;
}

static EFI_STATUS preflight_target_required(GPU_TARGET *target,
                                           GPU_PREFLIGHT_SNAPSHOT *snapshot,
                                           UINT32 required_command_mask)
{
    return preflight_target_required_phase(target, snapshot,
        required_command_mask, Tu102PreflightLegacy);
}

/* Existing callers retain the strict full-command guard. Only the dedicated
 * dual-attempt entry may use the narrower pre-DMA MSE qualification. */
static EFI_STATUS preflight_target(GPU_TARGET *target,
                                   GPU_PREFLIGHT_SNAPSHOT *snapshot)
{
    return preflight_target_required(target, snapshot,
                                     PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
}

static EFI_STATUS __attribute__((unused)) enumerate_targets(GPU_TARGET targets[2])
{
    EFI_HANDLE *handles = NULL;
    UINTN count = 0, i, j;
    EFI_STATUS status;
    EFI_GUID pci_io_guid = EFI_PCI_IO_PROTOCOL_GUID;
    status = uefi_call_wrapper(BS->LocateHandleBuffer, 5, ByProtocol,
                               &pci_io_guid, NULL, &count, &handles);
    if (EFI_ERROR(status)) return status;
    for (i = 0; i < 2; ++i) {
        targets[i].profile = &g_Profiles[i];
        targets[i].preflight_ok = FALSE;
        targets[i].mutated = FALSE;
        targets[i].quiescent = FALSE;
    }
    for (i = 0; i < count; ++i) {
        EFI_PCI_IO_PROTOCOL *pci = NULL;
        UINTN segment, bus, device, function;
        status = uefi_call_wrapper(BS->HandleProtocol, 3, handles[i], &pci_io_guid,
                                   (VOID **)&pci);
        if (EFI_ERROR(status) || !pci) { status = EFI_DEVICE_ERROR; goto done; }
        status = uefi_call_wrapper(pci->GetLocation, 5, pci, &segment, &bus,
                                   &device, &function);
        if (EFI_ERROR(status)) goto done;
        for (j = 0; j < 2; ++j) {
            const TU102_PROFILE *p = targets[j].profile;
            if (segment == p->segment && bus == p->bus && device == p->device &&
                function == p->function) {
                if (targets[j].handle) { status = EFI_SECURITY_VIOLATION; goto done; }
                targets[j].handle = handles[i];
                targets[j].pci = pci;
                targets[j].segment = segment; targets[j].bus = bus;
                targets[j].device = device; targets[j].function = function;
            }
        }
    }
    status = EFI_SUCCESS;
    for (j = 0; j < 2; ++j) {
        if (!targets[j].handle) { status = EFI_NOT_FOUND; goto done; }
        status = preflight_target(&targets[j], NULL);
        if (EFI_ERROR(status)) goto done;
    }
done:
    if (handles) {
        EFI_STATUS free_status = uefi_call_wrapper(BS->FreePool, 1, handles);
        if (!EFI_ERROR(status) && EFI_ERROR(free_status)) status = free_status;
    }
    return status;
}

static VOID bytes_zero(VOID *dst, UINTN size)
{
    UINT8 *p = dst;
    while (size--) *p++ = 0;
}

static VOID bytes_copy(VOID *dst, const VOID *src, UINTN size)
{
    UINT8 *d = dst;
    const UINT8 *s = src;
    while (size--) *d++ = *s++;
}

static UINT32 load_le32(const UINT8 *p)
{
    return (UINT32)p[0] | ((UINT32)p[1] << 8) |
           ((UINT32)p[2] << 16) | ((UINT32)p[3] << 24);
}

static VOID store_le32(UINT8 *p, UINT32 value)
{
    UINTN i;
    for (i = 0; i < 4; ++i) p[i] = (UINT8)(value >> (i * 8));
}

static VOID store_le64(UINT8 *p, UINT64 value)
{
    store_le32(p, (UINT32)value);
    store_le32(p + 4, (UINT32)(value >> 32));
}

static EFI_STATUS falcon_reset(GPU_TARGET *target, UINT32 base)
{
    UINT32 value = 0;
    UINTN i;
    EFI_STATUS status;
    if (!BS || !BS->Stall) return EFI_UNSUPPORTED;
    status = gpu_write32(target, base + 0x3c0, 1);
    if (EFI_ERROR(status)) return status;
    for (i = 0; i < 16; ++i) {
        status = gpu_read32(target, base + 0x3c0, &value);
        if (EFI_ERROR(status) || !(value & 1u))
            return v43_poll_result(target, base + 0x3c0, value, 1, 1,
                i + 1u, 16u, EFI_ERROR(status) ? status : EFI_DEVICE_ERROR);
    }
    (void)v43_poll_result(target, base + 0x3c0, value, 1, 1, 16, 16, EFI_SUCCESS);
    status = v43_stall(target, 50);
    if (EFI_ERROR(status)) return status;
    status = gpu_write32(target, base + 0x3c0, 0);
    if (EFI_ERROR(status)) return status;
    status = gpu_read32(target, base + 0x3c0, &value);
    if (EFI_ERROR(status)) return status;
    if (value & 1u)
        return v43_poll_result(target, base + 0x3c0, value, 1, 0, 1, 1, EFI_DEVICE_ERROR);
    for (i = 0; i < 3000; ++i) {
        status = gpu_read32(target, base + 0x10c, &value);
        if (EFI_ERROR(status))
            return v43_poll_result(target, base + 0x10c, value, 6, 0, i + 1u, 3000, status);
        if (!(value & 6u)) {
            (void)v43_poll_result(target, base + 0x10c, value, 6, 0, i + 1u, 3000, EFI_SUCCESS);
            return gpu_write32(target, base + 0x084, 0x162000a1);
        }
        status = v43_stall(target, 1000);
        if (EFI_ERROR(status))
            return v43_poll_result(target, base + 0x10c, value, 6, 0, i + 1u, 3000, status);
    }
    return v43_poll_result(target, base + 0x10c, value, 6, 0, 3000, 3000, EFI_TIMEOUT);
}

static EFI_STATUS falcon_context(GPU_TARGET *target, UINT32 base)
{
    UINT32 value;
    EFI_STATUS status = gpu_read32(target, base + 0x624, &value);
    if (EFI_ERROR(status)) return status;
    status = gpu_write32(target, base + 0x624, value | 0x80u);
    if (EFI_ERROR(status)) return status;
    return gpu_write32(target, base + 0x10c, 0);
}

static EFI_STATUS falcon_dmem(GPU_TARGET *target, UINT32 base,
                              const UINT8 *src, UINT32 bytes)
{
    UINT32 i;
    EFI_STATUS status;
    if (!src || (bytes & 3u)) return EFI_INVALID_PARAMETER;
    status = gpu_write32(target, base + 0x1c0, 1u << 24);
    if (EFI_ERROR(status)) return v43_upload_result(target, base + 0x1c4, bytes, 0, status);
    for (i = 0; i < bytes; i += 4) {
        status = gpu_write32(target, base + 0x1c4, load_le32(src + i));
        if (EFI_ERROR(status)) return v43_upload_result(target, base + 0x1c4, bytes, i, status);
    }
    return v43_upload_result(target, base + 0x1c4, bytes, bytes, EFI_SUCCESS);
}

static EFI_STATUS falcon_imem(GPU_TARGET *target, UINT32 base, UINT32 dest,
                              const UINT8 *src, UINT32 bytes, UINT32 tag,
                              BOOLEAN secure)
{
    UINT32 i, control;
    EFI_STATUS status;
    if (!src || (dest & 255u) || (bytes & 3u) || (tag & 255u))
        return EFI_INVALID_PARAMETER;
    control = dest | (1u << 24) | (secure ? (1u << 28) : 0);
    status = gpu_write32(target, base + 0x180, control);
    if (EFI_ERROR(status)) return v43_upload_result(target, base + 0x184, bytes, 0, status);
    for (i = 0; i < bytes; i += 4) {
        if (!(i & 255u)) {
            status = gpu_write32(target, base + 0x188, (tag + i) >> 8);
            if (EFI_ERROR(status)) return v43_upload_result(target, base + 0x184, bytes, i, status);
        }
        status = gpu_write32(target, base + 0x184, load_le32(src + i));
        if (EFI_ERROR(status)) return v43_upload_result(target, base + 0x184, bytes, i, status);
    }
    return v43_upload_result(target, base + 0x184, bytes, bytes, EFI_SUCCESS);
}

static EFI_STATUS falcon_start(GPU_TARGET *target, UINT32 base)
{
    UINT32 value;
    EFI_STATUS status = gpu_read32(target, base + 0x100, &value);
    if (EFI_ERROR(status)) return status;
    return gpu_write32(target, base + ((value & 0x40u) ? 0x130 : 0x100), 2);
}

static EFI_STATUS falcon_halt(GPU_TARGET *target, UINT32 base)
{
    UINT32 value = 0;
    UINTN i;
    EFI_STATUS status;
    for (i = 0; i < 5000; ++i) {
        status = gpu_read32(target, base + 0x100, &value);
        if (EFI_ERROR(status) || (value & 0x10u))
            return v43_poll_result(target, base + 0x100, value, 0x10, 0x10,
                i + 1u, 5000, EFI_ERROR(status) ? status : EFI_SUCCESS);
        status = v43_stall(target, 1000);
        if (EFI_ERROR(status))
            return v43_poll_result(target, base + 0x100, value, 0x10, 0x10, i + 1u, 5000, status);
    }
    return v43_poll_result(target, base + 0x100, value, 0x10, 0x10, 5000, 5000, EFI_TIMEOUT);
}

static EFI_STATUS allocate_dma_pool(GPU_TARGET *target)
{
    UINT64 supported = 0;
    UINT64 current = 0;
    EFI_STATUS status;
    if (!target || !target->pci) return EFI_INVALID_PARAMETER;
    if (!target->pci->Attributes || !target->pci->AllocateBuffer)
        return EFI_UNSUPPORTED;
    if (target->pool || target->dma_mapping || target->buffer_allocated ||
        target->buffer_mapped || target->allocation_uncertain ||
        target->mapping_uncertain || target->mutated)
        return EFI_ACCESS_DENIED;
    target->pool = NULL;
    target->dma_address = 0;
    target->dma_mapping = NULL;
    target->dma_mapped_bytes = 0;
    target->dma_map_status = EFI_NOT_READY;
    target->buffer_allocated = FALSE;
    target->buffer_mapped = FALSE;
    target->allocation_uncertain = FALSE;
    target->mapping_uncertain = FALSE;
    status = uefi_call_wrapper(target->pci->Attributes, 4, target->pci,
                               EfiPciIoAttributeOperationSupported, 0, &supported);
    if (EFI_ERROR(status)) return status;
    if (!(supported & EFI_PCI_IO_ATTRIBUTE_DUAL_ADDRESS_CYCLE)) return EFI_UNSUPPORTED;
    status = uefi_call_wrapper(target->pci->Attributes, 4, target->pci,
                               EfiPciIoAttributeOperationGet, 0, &current);
    if (EFI_ERROR(status)) return status;
    /* Do not change PCI controller/bridge attributes here. The common-buffer
     * mapping can use >32-bit device addresses only when DAC is already active. */
    if (!(current & EFI_PCI_IO_ATTRIBUTE_DUAL_ADDRESS_CYCLE)) return EFI_UNSUPPORTED;
    status = uefi_call_wrapper(target->pci->AllocateBuffer, 6, target->pci,
                               AllocateAnyPages, EfiBootServicesData, TX_POOL_PAGES,
                               &target->pool, 0);
    if (EFI_ERROR(status)) {
        /* Only a documented out-of-resources result with no output pointer
         * may be considered for a later allocation-order candidate. */
        if (target->pool) target->allocation_uncertain = TRUE;
        return status;
    }
    if (!target->pool) {
        target->allocation_uncertain = TRUE;
        return EFI_DEVICE_ERROR;
    }
    target->buffer_allocated = TRUE;
    return EFI_SUCCESS;
}

static EFI_STATUS map_dma_pool(GPU_TARGET *target, BOOLEAN batch_mode)
{
    UINTN bytes = TX_POOL_SIZE;
    EFI_STATUS status;
    if (!target || !target->pci || !target->buffer_allocated || !target->pool)
        return EFI_INVALID_PARAMETER;
    if (!target->pci->Map) return EFI_UNSUPPORTED;
    target->dma_address = 0;
    target->dma_mapping = NULL;
    target->dma_mapped_bytes = 0;
    target->dma_map_status = EFI_NOT_READY;
    status = uefi_call_wrapper(target->pci->Map, 6, target->pci,
                               EfiPciIoOperationBusMasterCommonBuffer, target->pool,
                               &bytes, &target->dma_address, &target->dma_mapping);
    target->dma_mapped_bytes = bytes;
    target->dma_map_status = status;
    if (EFI_ERROR(status)) {
        /* Map error outputs are undefined by the UEFI contract. Batch mode
         * therefore stops on every Map error, even if Mapping stayed NULL.
         * Preserve the legacy wrapper's historical NULL-token cleanup path. */
        if (batch_mode || target->dma_mapping) target->mapping_uncertain = TRUE;
        return status;
    }
    target->buffer_mapped = TRUE;
    if (bytes != TX_POOL_SIZE || (target->dma_address & 4095u) ||
#ifdef TU102_DUAL_ATTEMPT_V4
        /* V4's descriptor/metadata/radix fields carry full 64-bit device
         * addresses; a successful low-address Map needs no high-half bit.
         * Reject zero as an experimental null-address policy. Keep the
         * legacy profile's independent 4GiB lower bound below. */
        target->dma_address == 0 ||
#else
        target->dma_address < 0x100000000ULL ||
#endif
        target->dma_address > 0x10000000000ULL - TX_POOL_SIZE)
        return EFI_UNSUPPORTED;
    return EFI_SUCCESS;
}

#ifndef TU102_DUAL_ATTEMPT
static EFI_STATUS prepare_dma_pool(GPU_TARGET *target)
{
    EFI_STATUS status = allocate_dma_pool(target);
    if (EFI_ERROR(status)) return status;
    return map_dma_pool(target, FALSE);
}
#endif

static EFI_STATUS cleanup_dma_pool(GPU_TARGET *target)
{
    EFI_STATUS status;
    if (target->allocation_uncertain || target->mapping_uncertain)
        return EFI_ACCESS_DENIED;
    if (target->mutated && !target->quiescent) return EFI_ACCESS_DENIED;
    if (target->buffer_mapped) {
        if (!target->pci || !target->pci->Unmap) return EFI_UNSUPPORTED;
        status = uefi_call_wrapper(target->pci->Unmap, 2, target->pci,
                                   target->dma_mapping);
        if (EFI_ERROR(status)) return status;
        target->buffer_mapped = FALSE;
        target->dma_mapping = NULL;
    }
    if (target->buffer_allocated) {
        if (!target->pci || !target->pci->FreeBuffer) return EFI_UNSUPPORTED;
        status = uefi_call_wrapper(target->pci->FreeBuffer, 3, target->pci,
                                   TX_POOL_PAGES, target->pool);
        if (EFI_ERROR(status)) return status;
        target->buffer_allocated = FALSE;
        target->pool = NULL;
    }
    return EFI_SUCCESS;
}

static EFI_PHYSICAL_ADDRESS dma_address(GPU_TARGET *target, UINT32 offset)
{
    return target->dma_address + offset;
}

static UINT64 load_le64(const UINT8 *p)
{
    return (UINT64)load_le32(p) | ((UINT64)load_le32(p + 4) << 32);
}

static VOID tx_diagnostic_clear(TX_INPUT_DIAGNOSTIC *diagnostic)
{
    if (!diagnostic) return;
    diagnostic->valid = FALSE;
    diagnostic->component = NULL;
    diagnostic->index = 0;
    diagnostic->offset = 0;
    diagnostic->expected = 0;
    diagnostic->actual = 0;
    diagnostic->status = EFI_SUCCESS;
}

static EFI_STATUS tx_diagnostic_failure(TX_INPUT_DIAGNOSTIC *diagnostic,
                                        const char *component,
                                        UINT64 index, UINT64 offset,
                                        UINT64 expected, UINT64 actual,
                                        EFI_STATUS status)
{
    if (diagnostic && !diagnostic->valid) {
        diagnostic->valid = TRUE;
        diagnostic->component = component;
        diagnostic->index = index;
        diagnostic->offset = offset;
        diagnostic->expected = expected;
        diagnostic->actual = actual;
        diagnostic->status = status;
    }
    return status;
}

static EFI_STATUS tx_input_expect(GPU_TARGET *target, const char *component,
                                  UINT64 index, UINT64 offset,
                                  UINT64 expected, UINT64 actual,
                                  EFI_STATUS failure_status)
{
    if (expected == actual) return EFI_SUCCESS;
    return tx_diagnostic_failure(target ? &target->v4_input_diag : NULL,
        component, index, offset, expected, actual, failure_status);
}

#ifdef TU102_DUAL_ATTEMPT_V4
typedef struct {
    const char *name;
    UINT64 offset;
    UINT64 size;
} TX_STATIC_REGION;

static EFI_STATUS tx_static_expect(TX_INPUT_DIAGNOSTIC *diagnostic,
                                   const char *component, UINT64 index,
                                   UINT64 offset, UINT64 expected,
                                   UINT64 actual)
{
    if (expected == actual) return EFI_SUCCESS;
    return tx_diagnostic_failure(diagnostic, component, index, offset,
        expected, actual, EFI_SECURITY_VIOLATION);
}

static EFI_STATUS tx_static_validate_regions(
    const TX_STATIC_REGION *regions, UINTN region_count, UINT64 pool_size,
    TX_INPUT_DIAGNOSTIC *diagnostic)
{
    UINTN i, j;
    for (i = 0; i < region_count; ++i) {
        const TX_STATIC_REGION *r = &regions[i];
        if (r->offset > pool_size || r->size > pool_size - r->offset)
            return tx_diagnostic_failure(diagnostic, r->name, i, r->offset,
                pool_size - (r->offset <= pool_size ? r->offset : pool_size),
                r->size, EFI_SECURITY_VIOLATION);
    }
    for (i = 0; i < region_count; ++i) {
        UINT64 i_end = regions[i].offset + regions[i].size;
        for (j = i + 1u; j < region_count; ++j) {
            UINT64 j_end = regions[j].offset + regions[j].size;
            if (regions[i].offset < j_end && regions[j].offset < i_end)
                return tx_diagnostic_failure(diagnostic,
                    "layout.segment_overlap", j, regions[j].offset,
                    i_end, regions[j].offset, EFI_SECURITY_VIOLATION);
        }
    }
    return EFI_SUCCESS;
}

static EFI_STATUS tx_static_check_radix_capacity(UINT64 elf_size,
                                                  UINT64 radix_capacity,
                                                  UINT64 data_capacity,
                                                  TX_INPUT_DIAGNOSTIC *diag)
{
    UINT64 pages, l2, radix_bytes, data_bytes;
    if (elf_size > UINT64_MAX - 4095u)
        return tx_diagnostic_failure(diag, "radix.capacity", 0,
            TX_RADIX_OFF, radix_capacity, UINT64_MAX,
            EFI_SECURITY_VIOLATION);
    pages = (elf_size + 4095u) / 4096u;
    if (pages > UINT64_MAX - 511u)
        return tx_diagnostic_failure(diag, "radix.capacity", pages,
            TX_RADIX_OFF, radix_capacity, UINT64_MAX,
            EFI_SECURITY_VIOLATION);
    l2 = (pages + 511u) / 512u;
    if (l2 > (UINT64_MAX / 4096u) - 2u)
        return tx_diagnostic_failure(diag, "radix.capacity", l2,
            TX_RADIX_OFF, radix_capacity, UINT64_MAX,
            EFI_SECURITY_VIOLATION);
    radix_bytes = (2u + l2) * 4096u;
    if (radix_bytes > radix_capacity)
        return tx_diagnostic_failure(diag, "radix.capacity", l2,
            TX_RADIX_OFF, radix_capacity, radix_bytes,
            EFI_SECURITY_VIOLATION);
    if (pages > UINT64_MAX / 4096u)
        return tx_diagnostic_failure(diag, "workspace.capacity", pages,
            TX_DATA_OFF, data_capacity, UINT64_MAX,
            EFI_SECURITY_VIOLATION);
    data_bytes = pages * 4096u;
    if (data_bytes > data_capacity)
        return tx_diagnostic_failure(diag, "workspace.capacity", pages,
            TX_DATA_OFF, data_capacity, data_bytes,
            EFI_SECURITY_VIOLATION);
    return EFI_SUCCESS;
}

static EFI_STATUS tx_static_check_metadata_geometry(UINT64 framebuffer_size,
    UINT64 frts, UINT64 elf_size, UINT64 boot_size,
    TX_INPUT_DIAGNOSTIC *diag)
{
    const UINT64 mb = 0x100000ULL;
    UINT64 expected_frts, boot_base, elf_base, reserved_size;
    UINT64 reserved_base, end;
    if (framebuffer_size < 2u * mb)
        return tx_diagnostic_failure(diag, "metadata.framebuffer_geometry",
            22, 0, 2u * mb, framebuffer_size, EFI_SECURITY_VIOLATION);
    expected_frts = framebuffer_size - 2u * mb;
    if (frts != expected_frts)
        return tx_diagnostic_failure(diag, "metadata.frts_geometry", 19,
            0, expected_frts, frts, EFI_SECURITY_VIOLATION);
    if (frts < boot_size)
        return tx_diagnostic_failure(diag, "metadata.boot_geometry", 5,
            0, boot_size, frts, EFI_SECURITY_VIOLATION);
    boot_base = (frts - boot_size) & ~0xfffULL;
    if (boot_base < elf_size)
        return tx_diagnostic_failure(diag, "metadata.elf_geometry", 3,
            0, elf_size, boot_base, EFI_SECURITY_VIOLATION);
    elf_base = (boot_base - elf_size) & ~0xffffULL;
    reserved_size = 107u * mb;
    if (elf_base < reserved_size)
        return tx_diagnostic_failure(diag,
            "metadata.reservation_geometry", 16, 0,
            reserved_size, elf_base, EFI_SECURITY_VIOLATION);
    reserved_base = (elf_base - reserved_size) & ~(mb - 1u);
    if (reserved_base > UINT64_MAX - reserved_size)
        return tx_diagnostic_failure(diag,
            "metadata.reservation_geometry", 16, 0,
            elf_base, UINT64_MAX, EFI_SECURITY_VIOLATION);
    end = reserved_base + reserved_size;
    if (end > elf_base || framebuffer_size - reserved_base > 256u * mb)
        return tx_diagnostic_failure(diag,
            "metadata.reservation_geometry", 16, 0,
            elf_base, end, EFI_SECURITY_VIOLATION);
    return EFI_SUCCESS;
}

static const TU102_PROFILE *tx_v4_pinned_profile(const TU102_PROFILE *profile)
{
    if (!profile) return NULL;
    if (profile == &g_Profiles[0] ||
        (profile->bus == g_Profiles[0].bus &&
         profile->subsystem_device == g_Profiles[0].subsystem_device))
        return &g_Profiles[0];
    if (profile == &g_Profiles[1] ||
        (profile->bus == g_Profiles[1].bus &&
         profile->subsystem_device == g_Profiles[1].subsystem_device))
        return &g_Profiles[1];
    return NULL;
}

/* Pure CPU audit: reads only the selected profile and immutable embedded
 * asset descriptors.  It does not allocate memory or call EFI/PCI/GPU APIs. */
static EFI_STATUS v4_static_audit_profile(const TU102_PROFILE *profile,
                                          TX_INPUT_DIAGNOSTIC *diagnostic)
{
    const TU102_PROFILE *pinned;
    const UINT8 *fwsec_asset;
    UINT64 fwsec_asset_size;
    TX_STATIC_REGION regions[7];
    EFI_STATUS status;
    UINTN i;
    tx_diagnostic_clear(diagnostic);
    if (!profile)
        return tx_diagnostic_failure(diagnostic, "profile.present", 0,
            0, 1, 0, EFI_SECURITY_VIOLATION);
    pinned = tx_v4_pinned_profile(profile);
    if (!pinned)
        return tx_diagnostic_failure(diagnostic, "profile.identity", 0,
            0, g_Profiles[0].subsystem_device, profile->subsystem_device,
            EFI_SECURITY_VIOLATION);
    if (!profile->fwsec_blob)
        return tx_diagnostic_failure(diagnostic, "asset.fwsec.present", 0,
            0, 1, 0, EFI_SECURITY_VIOLATION);
    fwsec_asset = pinned->fwsec_blob;
    fwsec_asset_size = pinned == &g_Profiles[0] ?
        sizeof(blob_fwsec_card0) : sizeof(blob_fwsec_card1);
    status = tx_static_expect(diagnostic, "asset.fwsec.pointer", 0, 0,
        (UINT64)(UINTN)fwsec_asset, (UINT64)(UINTN)profile->fwsec_blob);
    if (EFI_ERROR(status)) return status;
    status = tx_static_expect(diagnostic, "asset.fwsec.length", 0,
        TX_FWSEC_OFF, fwsec_asset_size, profile->fwsec_size);
    if (EFI_ERROR(status)) return status;
    status = tx_static_expect(diagnostic, "asset.booter.length", 0,
        TX_BOOTER_OFF, TX_BOOTER_SIZE, sizeof(blob_booter_ucode_prod));
    if (EFI_ERROR(status)) return status;
    status = tx_static_expect(diagnostic, "asset.signature.length", 0,
        TX_SIGNATURE_OFF, TX_SIGNATURE_SIZE, sizeof(blob_v67_payload));
    if (EFI_ERROR(status)) return status;
    status = tx_static_expect(diagnostic, "asset.boot.length", 0,
        TX_BOOT_OFF, TX_BOOT_SIZE, sizeof(blob_gsp_rm_boot));
    if (EFI_ERROR(status)) return status;

    /* Validate variable profile ranges before comparing immutable fields so
     * an out-of-range command offset is reported as the actionable cause. */
    if (profile->fwsec_size > TX_BOOTER_OFF - TX_FWSEC_OFF)
        return tx_diagnostic_failure(diagnostic, "layout.fwsec_range", 0,
            TX_FWSEC_OFF, TX_BOOTER_OFF - TX_FWSEC_OFF,
            profile->fwsec_size, EFI_SECURITY_VIOLATION);
    if (profile->dmem_offset > profile->fwsec_size ||
        profile->dmem_size > profile->fwsec_size - profile->dmem_offset)
        return tx_diagnostic_failure(diagnostic, "dmem.range", 0,
            profile->dmem_offset, profile->fwsec_size,
            (UINT64)profile->dmem_offset + profile->dmem_size,
            EFI_SECURITY_VIOLATION);
    if (profile->command_offset > profile->dmem_size ||
        12u * sizeof(UINT32) > profile->dmem_size - profile->command_offset)
        return tx_diagnostic_failure(diagnostic, "dmem.command_range", 0,
            (UINT64)profile->dmem_offset + profile->command_offset,
            profile->dmem_size, (UINT64)profile->command_offset +
                12u * sizeof(UINT32), EFI_SECURITY_VIOLATION);
    if (profile->init_command_offset > profile->dmem_size ||
        sizeof(UINT32) > profile->dmem_size - profile->init_command_offset)
        return tx_diagnostic_failure(diagnostic, "dmem.init_command_range", 0,
            (UINT64)profile->dmem_offset + profile->init_command_offset,
            profile->dmem_size, (UINT64)profile->init_command_offset +
                sizeof(UINT32), EFI_SECURITY_VIOLATION);
    if (profile->command_offset < profile->init_command_offset + sizeof(UINT32) &&
        profile->init_command_offset < profile->command_offset +
            12u * sizeof(UINT32))
        return tx_diagnostic_failure(diagnostic, "dmem.command_overlap", 0,
            (UINT64)profile->dmem_offset + profile->command_offset,
            profile->init_command_offset + sizeof(UINT32),
            profile->command_offset, EFI_SECURITY_VIOLATION);

#define TX_PROFILE_FIELD(field) do { \
    status = tx_static_expect(diagnostic, "profile." #field, 0, \
        profile->field, pinned->field, profile->field); \
    if (EFI_ERROR(status)) return status; \
} while (0)
    TX_PROFILE_FIELD(segment);
    TX_PROFILE_FIELD(bus);
    TX_PROFILE_FIELD(device);
    TX_PROFILE_FIELD(function);
    TX_PROFILE_FIELD(vendor_id);
    TX_PROFILE_FIELD(device_id);
    TX_PROFILE_FIELD(subsystem_vendor);
    TX_PROFILE_FIELD(prom_desc_off);
    TX_PROFILE_FIELD(prom_image_off);
    TX_PROFILE_FIELD(fwsec_size);
    TX_PROFILE_FIELD(capacity_selector);
    TX_PROFILE_FIELD(workspace);
    TX_PROFILE_FIELD(wpr_lo);
    TX_PROFILE_FIELD(wpr_hi);
    TX_PROFILE_FIELD(initial_ss0);
    TX_PROFILE_FIELD(initial_ss1);
    TX_PROFILE_FIELD(mapper_offset);
    TX_PROFILE_FIELD(command_offset);
    TX_PROFILE_FIELD(init_command_offset);
    TX_PROFILE_FIELD(imem_size);
    TX_PROFILE_FIELD(min_falcon_blocks);
    TX_PROFILE_FIELD(sec_imem_base);
    TX_PROFILE_FIELD(sec_imem_size);
    TX_PROFILE_FIELD(dmem_offset);
    TX_PROFILE_FIELD(dmem_size);
    TX_PROFILE_FIELD(hardware_validated);
    TX_PROFILE_FIELD(v4_bound_ss0);
    TX_PROFILE_FIELD(v4_bound_ss1);
    TX_PROFILE_FIELD(v4_detached_ss0);
    TX_PROFILE_FIELD(v4_detached_ss1);
    TX_PROFILE_FIELD(v4_detached_ss1_alt);
#undef TX_PROFILE_FIELD
    for (i = 0; i < 15u; ++i) {
        status = tx_static_expect(diagnostic, "profile.desc_words", i,
            i * sizeof(UINT32), pinned->desc_words[i],
            profile->desc_words[i]);
        if (EFI_ERROR(status)) return status;
    }

    regions[0] = (TX_STATIC_REGION){"layout.fwsec", TX_FWSEC_OFF,
                                    profile->fwsec_size};
    regions[1] = (TX_STATIC_REGION){"layout.booter", TX_BOOTER_OFF,
                                    TX_BOOTER_SIZE};
    regions[2] = (TX_STATIC_REGION){"layout.signature", TX_SIGNATURE_OFF,
                                    TX_SIGNATURE_SIZE};
    regions[3] = (TX_STATIC_REGION){"layout.boot", TX_BOOT_OFF,
                                    TX_BOOT_SIZE};
    regions[4] = (TX_STATIC_REGION){"layout.metadata", TX_META_OFF,
                                    32u * sizeof(UINT64) + 1u};
    regions[5] = (TX_STATIC_REGION){"layout.radix", TX_RADIX_OFF,
                                    TX_DATA_OFF - TX_RADIX_OFF};
    regions[6] = (TX_STATIC_REGION){"layout.workspace", TX_DATA_OFF,
                                    TX_ELF_SIZE};
    status = tx_static_validate_regions(regions, 7u, TX_POOL_SIZE, diagnostic);
    if (EFI_ERROR(status)) return status;
    if (TX_BOOTER_OFF > TX_POOL_SIZE ||
        TX_BOOTER_SIZE > TX_POOL_SIZE - TX_BOOTER_OFF ||
        0x8700u > TX_BOOTER_SIZE ||
        16u > TX_BOOTER_SIZE - 0x8700u)
        return tx_diagnostic_failure(diagnostic, "layout.signature_slot", 0,
            TX_BOOTER_OFF + 0x8700u, TX_BOOTER_SIZE,
            0x8700u + 16u, EFI_SECURITY_VIOLATION);
    status = tx_static_check_metadata_geometry(TX_FB_SIZE, TX_FRTS,
        TX_ELF_SIZE, TX_BOOT_SIZE, diagnostic);
    if (EFI_ERROR(status)) return status;
    status = tx_static_check_radix_capacity(TX_ELF_SIZE,
        TX_DATA_OFF - TX_RADIX_OFF, TX_POOL_SIZE - TX_DATA_OFF, diagnostic);
    if (EFI_ERROR(status)) return status;
    return EFI_SUCCESS;
}

static EFI_STATUS __attribute__((unused))
v4_static_audit_target(GPU_TARGET *target)
{
    EFI_STATUS status;
    if (!target) return EFI_INVALID_PARAMETER;
    tx_diagnostic_clear(&target->v4_static_audit_diag);
    status = v4_static_audit_profile(target->profile,
                                     &target->v4_static_audit_diag);
    return status;
}
#endif /* TU102_DUAL_ATTEMPT_V4 */

static EFI_STATUS verify_transaction_inputs(GPU_TARGET *target,
                                            const UINT32 command[12],
                                            const UINT8 sig[16],
                                            const UINT64 metadata[32],
                                            UINT64 pages, UINT64 l2)
{
    const TU102_PROFILE *p;
    const UINT8 *pool;
    UINT64 radix_bytes, data_bytes;
    UINTN i;
    EFI_STATUS status;
    if (!target) return EFI_INVALID_PARAMETER;
    tx_diagnostic_clear(&target->v4_input_diag);
    p = target->profile;
    pool = target->pool;
    if (!p || !pool || !p->fwsec_blob || !command || !sig || !metadata)
        return tx_diagnostic_failure(&target->v4_input_diag,
            "inputs.present", 0, 0, 1, 0, EFI_SECURITY_VIOLATION);
    if (l2 > TX_POOL_SIZE / 4096u ||
        2u > TX_POOL_SIZE / 4096u - l2)
        return tx_diagnostic_failure(&target->v4_input_diag,
            "radix.capacity", l2, TX_RADIX_OFF,
            TX_DATA_OFF - TX_RADIX_OFF, UINT64_MAX, EFI_SECURITY_VIOLATION);
    radix_bytes = (2u + l2) * 4096u;
    if (radix_bytes > TX_DATA_OFF - TX_RADIX_OFF)
        return tx_diagnostic_failure(&target->v4_input_diag,
            "radix.capacity", l2, TX_RADIX_OFF,
            TX_DATA_OFF - TX_RADIX_OFF, radix_bytes,
            EFI_SECURITY_VIOLATION);
    if (pages > TX_POOL_SIZE / 4096u)
        return tx_diagnostic_failure(&target->v4_input_diag,
            "workspace.capacity", pages, TX_DATA_OFF,
            TX_POOL_SIZE - TX_DATA_OFF, UINT64_MAX,
            EFI_SECURITY_VIOLATION);
    data_bytes = pages * 4096u;
    if (data_bytes > TX_POOL_SIZE - TX_DATA_OFF)
        return tx_diagnostic_failure(&target->v4_input_diag,
            "workspace.capacity", pages, TX_DATA_OFF,
            TX_POOL_SIZE - TX_DATA_OFF, data_bytes,
            EFI_SECURITY_VIOLATION);
    for (i = 0; i < p->fwsec_size; ++i) {
        UINT32 rel = (UINT32)i;
        BOOLEAN command_byte = rel >= p->dmem_offset + p->command_offset &&
            rel < p->dmem_offset + p->command_offset + sizeof(UINT32) * 12u;
        BOOLEAN init_byte = rel >= p->dmem_offset + p->init_command_offset &&
            rel < p->dmem_offset + p->init_command_offset + sizeof(UINT32);
        if (!command_byte && !init_byte) {
            status = tx_input_expect(target, "fwsec.asset", i,
                TX_FWSEC_OFF + i, p->fwsec_blob[i],
                pool[TX_FWSEC_OFF + i], EFI_COMPROMISED_DATA);
            if (EFI_ERROR(status)) return status;
        }
    }
    for (i = 0; i < 12u; ++i) {
        UINT64 offset = TX_FWSEC_OFF + p->dmem_offset +
            p->command_offset + i * sizeof(UINT32);
        status = tx_input_expect(target, "fwsec.command", i, offset,
            command[i], load_le32(pool + offset), EFI_COMPROMISED_DATA);
        if (EFI_ERROR(status)) return status;
    }
    {
        UINT64 offset = TX_FWSEC_OFF + p->dmem_offset +
            p->init_command_offset;
        status = tx_input_expect(target, "fwsec.init_command", 0, offset,
            0x15u, load_le32(pool + offset), EFI_COMPROMISED_DATA);
        if (EFI_ERROR(status)) return status;
    }
    for (i = 0; i < TX_BOOTER_SIZE; ++i) {
        UINT8 expected = (i >= 0x8700u && i < 0x8710u) ?
            sig[i - 0x8700u] : blob_booter_ucode_prod[i];
        status = tx_input_expect(target, "booter.container", i,
            TX_BOOTER_OFF + i, expected, pool[TX_BOOTER_OFF + i],
            EFI_COMPROMISED_DATA);
        if (EFI_ERROR(status)) return status;
    }
    for (i = 0; i < TX_SIGNATURE_SIZE; ++i) {
        status = tx_input_expect(target, "signature.asset", i,
            TX_SIGNATURE_OFF + i, blob_v67_payload[i],
            pool[TX_SIGNATURE_OFF + i], EFI_COMPROMISED_DATA);
        if (EFI_ERROR(status)) return status;
    }
    for (i = 0; i < TX_BOOT_SIZE; ++i) {
        status = tx_input_expect(target, "boot.asset", i,
            TX_BOOT_OFF + i, blob_gsp_rm_boot[i],
            pool[TX_BOOT_OFF + i], EFI_COMPROMISED_DATA);
        if (EFI_ERROR(status)) return status;
    }
    for (i = 0; i < 32u; ++i) {
        UINT64 expected = metadata[i];
        if (i == 30u) expected |= 0x100u;
        UINT64 offset = TX_META_OFF + i * sizeof(UINT64);
        status = tx_input_expect(target, "metadata.word", i, offset,
            expected, load_le64(pool + offset), EFI_COMPROMISED_DATA);
        if (EFI_ERROR(status)) return status;
    }
    status = tx_input_expect(target, "metadata.marker", 241,
        TX_META_OFF + 241u, 1u, pool[TX_META_OFF + 241u],
        EFI_COMPROMISED_DATA);
    if (EFI_ERROR(status)) return status;
    status = tx_input_expect(target, "metadata.radix_pointer", 2,
        TX_META_OFF + 2u * sizeof(UINT64),
        dma_address(target, TX_RADIX_OFF), metadata[2], EFI_COMPROMISED_DATA);
    if (EFI_ERROR(status)) return status;
    status = tx_input_expect(target, "metadata.elf_size", 3,
        TX_META_OFF + 3u * sizeof(UINT64), TX_ELF_SIZE, metadata[3],
        EFI_COMPROMISED_DATA);
    if (EFI_ERROR(status)) return status;
    status = tx_input_expect(target, "metadata.boot_pointer", 4,
        TX_META_OFF + 4u * sizeof(UINT64),
        dma_address(target, TX_BOOT_OFF), metadata[4], EFI_COMPROMISED_DATA);
    if (EFI_ERROR(status)) return status;
    status = tx_input_expect(target, "metadata.boot_size", 5,
        TX_META_OFF + 5u * sizeof(UINT64), TX_BOOT_SIZE, metadata[5],
        EFI_COMPROMISED_DATA);
    if (EFI_ERROR(status)) return status;
    status = tx_input_expect(target, "metadata.signature_pointer", 9,
        TX_META_OFF + 9u * sizeof(UINT64),
        dma_address(target, TX_SIGNATURE_OFF), metadata[9],
        EFI_COMPROMISED_DATA);
    if (EFI_ERROR(status)) return status;
    status = tx_input_expect(target, "metadata.signature_size", 10,
        TX_META_OFF + 10u * sizeof(UINT64), TX_SIGNATURE_SIZE, metadata[10],
        EFI_COMPROMISED_DATA);
    if (EFI_ERROR(status)) return status;
    status = tx_input_expect(target, "metadata.frts", 19,
        TX_META_OFF + 19u * sizeof(UINT64), TX_FRTS, metadata[19],
        EFI_COMPROMISED_DATA);
    if (EFI_ERROR(status)) return status;
    status = tx_input_expect(target, "metadata.framebuffer_size", 22,
        TX_META_OFF + 22u * sizeof(UINT64), TX_FB_SIZE, metadata[22],
        EFI_COMPROMISED_DATA);
    if (EFI_ERROR(status)) return status;
    status = tx_input_expect(target, "radix.root", 0, TX_RADIX_OFF,
        dma_address(target, TX_RADIX_OFF + 0x1000u),
        load_le64(pool + TX_RADIX_OFF), EFI_COMPROMISED_DATA);
    if (EFI_ERROR(status)) return status;
    for (i = 0; i < l2; ++i) {
        UINT64 expected = dma_address(target, TX_RADIX_OFF + 0x2000u + 4096u * i);
        UINT64 offset = TX_RADIX_OFF + 0x1000u + 8u * i;
        status = tx_input_expect(target, "radix.level2", i, offset,
            expected, load_le64(pool + offset), EFI_COMPROMISED_DATA);
        if (EFI_ERROR(status)) return status;
    }
    for (i = 0; i < pages; ++i) {
        UINT64 expected = dma_address(target, TX_DATA_OFF + 4096u * i);
        UINT64 offset = TX_RADIX_OFF + 0x2000u + 8u * i;
        status = tx_input_expect(target, "radix.data_page", i, offset,
            expected, load_le64(pool + offset), EFI_COMPROMISED_DATA);
        if (EFI_ERROR(status)) return status;
    }
    /* The ELF range is intentionally a zeroed device workspace in this
     * transaction, not a missing copy to be silently filled by this verifier. */
    for (i = 0; i < TX_ELF_SIZE; ++i) {
        status = tx_input_expect(target, "workspace.zero", i,
            TX_DATA_OFF + i, 0, pool[TX_DATA_OFF + i],
            EFI_COMPROMISED_DATA);
        if (EFI_ERROR(status)) return status;
    }
    return EFI_SUCCESS;
}

static EFI_STATUS prepare_transaction_data(GPU_TARGET *target)
{
    const TU102_PROFILE *p = target->profile;
    UINT8 *pool = target->pool;
    UINT8 *pd;
    UINT64 metadata[32] = {0}, mb = 0x100000ULL;
    UINT64 pages, l2, i;
    UINT32 command[12] = {1,24,0,0,0,2,1,20,
                           (UINT32)(TX_FRTS >> 12),0x100,2,0};
    static const UINT8 sig[16] = {0x32,0x60,0x43,0x67,0x98,0x89,0x7b,0x06,
                                  0xe8,0x5f,0x1a,0xa8,0x71,0x43,0x5d,0xcb};
    if (!pool || !p || !p->fwsec_blob ||
        p->fwsec_size > TX_BOOTER_OFF ||
        p->dmem_offset > p->fwsec_size ||
        p->dmem_size > p->fwsec_size - p->dmem_offset ||
        p->command_offset > p->dmem_size ||
        sizeof(command) > p->dmem_size - p->command_offset ||
        p->init_command_offset > p->dmem_size ||
        sizeof(UINT32) > p->dmem_size - p->init_command_offset ||
        TX_BOOTER_OFF > TX_POOL_SIZE ||
        TX_BOOTER_SIZE > TX_POOL_SIZE - TX_BOOTER_OFF ||
        TX_BOOTER_OFF + TX_BOOTER_SIZE > TX_SIGNATURE_OFF ||
        TX_BOOTER_OFF + 0x8700u > TX_POOL_SIZE ||
        sizeof(sig) > TX_POOL_SIZE - (TX_BOOTER_OFF + 0x8700u) ||
        TX_SIGNATURE_OFF > TX_POOL_SIZE ||
        TX_SIGNATURE_SIZE > TX_POOL_SIZE - TX_SIGNATURE_OFF ||
        TX_SIGNATURE_OFF + TX_SIGNATURE_SIZE > TX_BOOT_OFF ||
        TX_BOOT_OFF > TX_POOL_SIZE ||
        TX_BOOT_SIZE > TX_POOL_SIZE - TX_BOOT_OFF ||
        TX_BOOT_OFF + TX_BOOT_SIZE > TX_META_OFF ||
        TX_META_OFF > TX_POOL_SIZE ||
        sizeof(metadata) + 1u > TX_POOL_SIZE - TX_META_OFF ||
        TX_META_OFF + sizeof(metadata) + 1u > TX_RADIX_OFF ||
        TX_RADIX_OFF > TX_DATA_OFF || TX_DATA_OFF > TX_POOL_SIZE ||
        TX_ELF_SIZE > TX_POOL_SIZE - TX_DATA_OFF ||
        sizeof(blob_booter_ucode_prod) < TX_BOOTER_SIZE ||
        sizeof(blob_v67_payload) < TX_SIGNATURE_SIZE ||
        sizeof(blob_gsp_rm_boot) < TX_BOOT_SIZE)
        return EFI_SECURITY_VIOLATION;
    bytes_zero(pool, TX_POOL_SIZE);
    bytes_copy(pool + TX_FWSEC_OFF, p->fwsec_blob, p->fwsec_size);
    bytes_copy(pool + TX_BOOTER_OFF, blob_booter_ucode_prod, TX_BOOTER_SIZE);
    bytes_copy(pool + TX_SIGNATURE_OFF, blob_v67_payload, TX_SIGNATURE_SIZE);
    bytes_copy(pool + TX_BOOT_OFF, blob_gsp_rm_boot, TX_BOOT_SIZE);
    bytes_copy(pool + TX_BOOTER_OFF + 0x8700, sig, sizeof(sig));
    pd = pool + TX_FWSEC_OFF + p->dmem_offset;
    for (i = 0; i < 12; ++i) store_le32(pd + p->command_offset + i * 4, command[i]);
    store_le32(pd + p->init_command_offset, 0x15);

    metadata[0] = 0xdc3aae21371a60b3ULL; metadata[1] = 1;
    metadata[2] = dma_address(target, TX_RADIX_OFF); metadata[3] = TX_ELF_SIZE;
    metadata[4] = dma_address(target, TX_BOOT_OFF); metadata[5] = TX_BOOT_SIZE;
    metadata[6] = 0x1800; metadata[7] = 0x800; metadata[8] = 0;
    metadata[9] = dma_address(target, TX_SIGNATURE_OFF); metadata[10] = TX_SIGNATURE_SIZE;
    metadata[22] = TX_FB_SIZE; metadata[23] = TX_FB_SIZE - mb; metadata[24] = mb;
    metadata[21] = metadata[23]; metadata[20] = mb; metadata[19] = metadata[21] - mb;
    metadata[18] = (metadata[19] - metadata[5]) & ~0xfffULL;
    metadata[17] = (metadata[18] - metadata[3]) & ~0xffffULL;
    metadata[16] = 107 * mb; metadata[15] = (metadata[17] - metadata[16]) & ~(mb - 1);
    metadata[14] = metadata[15] - mb; metadata[13] = mb;
    metadata[12] = metadata[14] - mb; metadata[11] = metadata[12];
    if (metadata[19] != TX_FRTS || TX_FB_SIZE - metadata[11] > 256 * mb ||
        metadata[15] + metadata[16] > metadata[17]) return EFI_SECURITY_VIOLATION;
    for (i = 0; i < 32; ++i) store_le64(pool + TX_META_OFF + i * 8, metadata[i]);
    pool[TX_META_OFF + 241] = 1;
    pages = (TX_ELF_SIZE + 4095ULL) >> 12;
    l2 = (pages + 511) >> 9;
    if (TX_RADIX_OFF + (2 + l2) * 4096 > TX_DATA_OFF ||
        TX_DATA_OFF + pages * 4096 > TX_POOL_SIZE) return EFI_SECURITY_VIOLATION;
    store_le64(pool + TX_RADIX_OFF, dma_address(target, TX_RADIX_OFF + 0x1000));
    for (i = 0; i < l2; ++i)
        store_le64(pool + TX_RADIX_OFF + 0x1000 + 8 * i,
                   dma_address(target, TX_RADIX_OFF + 0x2000 + 4096 * i));
    for (i = 0; i < pages; ++i)
        store_le64(pool + TX_RADIX_OFF + 0x2000 + 8 * i,
                   dma_address(target, TX_DATA_OFF + 4096 * i));
    {
        EFI_STATUS verify_status = verify_transaction_inputs(target, command, sig,
                                                              metadata, pages, l2);
        if (EFI_ERROR(verify_status)) return verify_status;
    }
    __sync_synchronize();
    {
        if (!target->pci->Flush) return EFI_UNSUPPORTED;
        EFI_STATUS status = uefi_call_wrapper(target->pci->Flush, 1, target->pci);
        if (!EFI_ERROR(status)) target->transaction_data_prepared = TRUE;
        return status;
    }
}

#ifdef TU102_DUAL_ATTEMPT_V4
static BOOLEAN post_read_value_matches(UINTN index, UINT32 value)
{
    switch (index) {
    case 0: return (value >> 16) == 0;
    case 1: return (value & ~15u) == (UINT32)(TX_FRTS >> 8);
    case 2: return (value & ~15u) == (UINT32)((TX_FRTS + 0xe0000) >> 8);
    case 3: return value == 0;
    case 4: case 11: return value == 0x88888888u;
    case 5: case 12: return value == 8u;
    case 6: return value == 0xffffff8fu;
    case 7: return value == 0x1ffffe00u;
    case 8: return value == 0;
    case 9: case 10: return (value & 2u) == 0;
    default: return FALSE;
    }
}
#endif

#ifdef TU102_DUAL_ATTEMPT_V4
#define V4_PHASE_BEGIN(name) do { \
    status = tu102_attempt_transaction_stage(target, (name), TRUE, EFI_SUCCESS); \
    if (EFI_ERROR(status)) return status; \
} while (0)
#define V4_PHASE_CHECK(name) do { \
    if (EFI_ERROR(status)) { \
        EFI_STATUS v4_actual_status = status; \
        EFI_STATUS v4_journal_status = tu102_attempt_transaction_stage( \
            target, (name), FALSE, v4_actual_status); \
        return EFI_ERROR(v4_journal_status) ? v4_journal_status : v4_actual_status; \
    } \
} while (0)
#define V4_PHASE_END(name) do { \
    status = tu102_attempt_transaction_stage(target, (name), FALSE, EFI_SUCCESS); \
    if (EFI_ERROR(status)) return status; \
} while (0)
#else
#define V4_PHASE_BEGIN(name) do { (void)(name); } while (0)
#define V4_PHASE_CHECK(name) do { (void)(name); if (EFI_ERROR(status)) return status; } while (0)
#define V4_PHASE_END(name) do { (void)(name); } while (0)
#endif

static EFI_STATUS execute_profile(GPU_TARGET *target)
{
    const TU102_PROFILE *p = target->profile;
    EFI_STATUS status;
    UINT32 value = 0, blocks, dest, low = 0, high = 0;
#ifdef TU102_DUAL_ATTEMPT_V4
    UINT32 v4_blocks_before = 0, v4_blocks_after = 0;
#endif
    UINT8 desc[0x54] = {0};
    UINT64 fw, meta;
    if (!target || !target->pci || !BS || !BS->Stall) return EFI_UNSUPPORTED;
    {
        UINTN i;
        for (i = 0; i < 13u; ++i) {
            target->post_read_status[i] = EFI_NOT_READY;
            target->post_read_value[i] = 0;
        }
    }
    if (!target->transaction_data_prepared) {
        status = prepare_transaction_data(target);
        if (EFI_ERROR(status)) return status;
    }
    /* Allocation/preparation can take time; revalidate all hardware guards
     * immediately before the first write for each card. */
#ifdef TU102_DUAL_ATTEMPT_V4
    {
        GPU_PREFLIGHT_SNAPSHOT prewrite_snapshot;
        EFI_STATUS journal_status;
        status = preflight_target_required_phase(target, &prewrite_snapshot,
            PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER, Tu102PreflightV4Execution);
        v4_blocks_before = prewrite_snapshot.gsp_hwcfg_value & 0x1ffu;
        journal_status = tu102_attempt_hwcfg_preflight(target, v4_blocks_before,
            p->min_falcon_blocks, 0x100u,
            EFI_ERROR(status) ? prewrite_snapshot.gsp_hwcfg_status : EFI_SUCCESS);
        if (EFI_ERROR(journal_status)) return journal_status;
    }
    if (EFI_ERROR(status)) return status;
    if (!target->v4_hwcfg_valid) return EFI_NOT_READY;
    if (v4_blocks_before != target->v4_hwcfg_blocks) return EFI_SECURITY_VIOLATION;
#else
    status = preflight_target(target, NULL);
    if (EFI_ERROR(status)) return status;
#endif

    V4_PHASE_BEGIN("FWSEC_RESET");
    status = falcon_reset(target, TX_GSP);
#ifdef TU102_DUAL_ATTEMPT_V4
    if (EFI_ERROR(status)) {
        EFI_STATUS reset_status = status;
        EFI_STATUS phase_log = tu102_attempt_transaction_stage(target,
            "FWSEC_RESET", FALSE, reset_status);
        EFI_STATUS capacity_log = tu102_attempt_hwcfg_reset_skipped(target,
            v4_blocks_before, reset_status);
        if (EFI_ERROR(phase_log)) return phase_log;
        if (EFI_ERROR(capacity_log)) return capacity_log;
        return reset_status;
    }
#endif
    V4_PHASE_CHECK("FWSEC_RESET");
    V4_PHASE_END("FWSEC_RESET");

#ifdef TU102_DUAL_ATTEMPT_V4
    status = gpu_read32(target, TX_GSP + 0x108, &value);
    if (!EFI_ERROR(status)) {
        v4_blocks_after = value & 0x1ffu;
        if (v4_blocks_after != v4_blocks_before ||
            v4_blocks_after < p->min_falcon_blocks || v4_blocks_after > 0x100u)
            status = EFI_SECURITY_VIOLATION;
    }
    {
        EFI_STATUS journal_status = tu102_attempt_hwcfg_reset_recheck(target,
            v4_blocks_before, v4_blocks_after, status);
        if (EFI_ERROR(journal_status)) return journal_status;
    }
    if (EFI_ERROR(status)) return status;
#endif

    V4_PHASE_BEGIN("FWSEC_LOAD");
    status = falcon_context(target, TX_GSP);
    V4_PHASE_CHECK("FWSEC_LOAD");
    status = gpu_read32(target, TX_GSP + 0x610, &value);
    V4_PHASE_CHECK("FWSEC_LOAD");
    status = gpu_write32(target, TX_GSP + 0x610, (value & ~0x1fu) | 0x15u);
    V4_PHASE_CHECK("FWSEC_LOAD");
    fw = dma_address(target, TX_FWSEC_OFF);
    store_le32(desc + 0x20, 4); store_le64(desc + 0x24, fw);
    store_le32(desc + 0x2c, 0);
    store_le32(desc + 0x30, p->sec_imem_base);
    store_le32(desc + 0x34, p->sec_imem_base);
    store_le32(desc + 0x38, p->sec_imem_size);
    store_le64(desc + 0x40, fw + p->dmem_offset);
    store_le32(desc + 0x48, p->dmem_size);
    status = falcon_dmem(target, TX_GSP, desc, sizeof(desc));
    V4_PHASE_CHECK("FWSEC_LOAD");
    status = gpu_read32(target, TX_GSP + 0x108, &value);
    V4_PHASE_CHECK("FWSEC_LOAD");
    blocks = value & 0x1ffu;
#ifdef TU102_DUAL_ATTEMPT_V4
    if (blocks != v4_blocks_before || blocks < p->min_falcon_blocks || blocks > 0x100u)
        status = EFI_SECURITY_VIOLATION;
#else
    if (blocks < p->min_falcon_blocks || blocks > 0x100u) status = EFI_DEVICE_ERROR;
#endif
    V4_PHASE_CHECK("FWSEC_LOAD");
    dest = (blocks - 2) << 8;
    status = falcon_imem(target, TX_GSP, dest, blob_bl_gsp_tu102, 0x200, 0xfd00, FALSE);
    V4_PHASE_CHECK("FWSEC_LOAD");
    status = gpu_write32(target, TX_GSP + 0x180, dest);
    V4_PHASE_CHECK("FWSEC_LOAD");
    status = gpu_read32(target, TX_GSP + 0x184, &value);
    V4_PHASE_CHECK("FWSEC_LOAD");
    if (value != load_le32(blob_bl_gsp_tu102)) status = EFI_DEVICE_ERROR;
    V4_PHASE_CHECK("FWSEC_LOAD");
    status = gpu_write32(target, TX_GSP + 0x1c0, 0x20);
    V4_PHASE_CHECK("FWSEC_LOAD");
    status = gpu_read32(target, TX_GSP + 0x1c4, &value);
    V4_PHASE_CHECK("FWSEC_LOAD");
    if (value != 4) status = EFI_DEVICE_ERROR;
    V4_PHASE_CHECK("FWSEC_LOAD");
    status = gpu_write32(target, TX_GSP + 0x104, 0xfd00);
    V4_PHASE_CHECK("FWSEC_LOAD");
    V4_PHASE_END("FWSEC_LOAD");

    V4_PHASE_BEGIN("FWSEC_START");
    status = falcon_start(target, TX_GSP);
    V4_PHASE_CHECK("FWSEC_START");
    status = v43_stall(target, 100000);
    V4_PHASE_CHECK("FWSEC_START");
    status = falcon_halt(target, TX_GSP);
    V4_PHASE_CHECK("FWSEC_START");
    V4_PHASE_END("FWSEC_START");

    V4_PHASE_BEGIN("FWSEC_RESULT");
    status = gpu_read32(target, 0x1438, &value);
    record_post_read(target, 0, status, value);
    V4_PHASE_CHECK("FWSEC_RESULT");
    status = gpu_read32(target, 0x1fa824, &low);
    record_post_read(target, 1, status, low);
    V4_PHASE_CHECK("FWSEC_RESULT");
    status = gpu_read32(target, 0x1fa828, &high);
    record_post_read(target, 2, status, high);
    V4_PHASE_CHECK("FWSEC_RESULT");
    if ((value >> 16) || (low & ~15u) != (UINT32)(TX_FRTS >> 8) ||
        (high & ~15u) != (UINT32)((TX_FRTS + 0xe0000) >> 8))
        status = EFI_DEVICE_ERROR;
    V4_PHASE_CHECK("FWSEC_RESULT");
    V4_PHASE_END("FWSEC_RESULT");

    /* The pinned boot container contains a dummy GSP RM. Never start it. */
    V4_PHASE_BEGIN("SEC2_RESET");
    status = falcon_reset(target, TX_GSP);
    V4_PHASE_CHECK("SEC2_RESET");
    status = falcon_reset(target, TX_SEC);
    V4_PHASE_CHECK("SEC2_RESET");
    V4_PHASE_END("SEC2_RESET");

    V4_PHASE_BEGIN("SEC2_LOAD");
    status = falcon_context(target, TX_SEC);
    V4_PHASE_CHECK("SEC2_LOAD");
    status = falcon_imem(target, TX_SEC, 0, target->pool + TX_BOOTER_OFF,
                         0x100, 0, FALSE);
    V4_PHASE_CHECK("SEC2_LOAD");
    status = falcon_imem(target, TX_SEC, 0x100, target->pool + TX_BOOTER_OFF + 0x100,
                         0x8400, 0x100, TRUE);
    V4_PHASE_CHECK("SEC2_LOAD");
    status = falcon_dmem(target, TX_SEC, target->pool + TX_BOOTER_OFF + 0x8500, 0x6200);
    V4_PHASE_CHECK("SEC2_LOAD");
    status = gpu_write32(target, TX_SEC + 0x180, 0);
    V4_PHASE_CHECK("SEC2_LOAD");
    status = gpu_read32(target, TX_SEC + 0x184, &value);
    V4_PHASE_CHECK("SEC2_LOAD");
    if (value != load_le32(target->pool + TX_BOOTER_OFF)) status = EFI_DEVICE_ERROR;
    V4_PHASE_CHECK("SEC2_LOAD");
    status = gpu_write32(target, TX_SEC + 0x1c0, 0x200);
    V4_PHASE_CHECK("SEC2_LOAD");
    status = gpu_read32(target, TX_SEC + 0x1c4, &value);
    V4_PHASE_CHECK("SEC2_LOAD");
    if (value != load_le32(target->pool + TX_BOOTER_OFF + 0x8700)) status = EFI_DEVICE_ERROR;
    V4_PHASE_CHECK("SEC2_LOAD");
    meta = dma_address(target, TX_META_OFF);
    status = gpu_write32(target, TX_SEC + 0x104, 0);
    V4_PHASE_CHECK("SEC2_LOAD");
    status = gpu_write32(target, TX_SEC + 0x040, (UINT32)meta);
    V4_PHASE_CHECK("SEC2_LOAD");
    status = gpu_write32(target, TX_SEC + 0x044, (UINT32)(meta >> 32));
    V4_PHASE_CHECK("SEC2_LOAD");
    V4_PHASE_END("SEC2_LOAD");

    V4_PHASE_BEGIN("SEC2_START");
    status = falcon_start(target, TX_SEC);
    V4_PHASE_CHECK("SEC2_START");
    status = v43_stall(target, 3000000);
    V4_PHASE_CHECK("SEC2_START");
    status = falcon_halt(target, TX_SEC);
    V4_PHASE_CHECK("SEC2_START");
    V4_PHASE_END("SEC2_START");

    V4_PHASE_BEGIN("SEC2_RESULT");
    status = gpu_read32(target, TX_SEC + 0x040, &value);
    record_post_read(target, 3, status, value);
    V4_PHASE_CHECK("SEC2_RESULT");
    status = gpu_read32(target, 0x409664, &low);
    record_post_read(target, 4, status, low);
    V4_PHASE_CHECK("SEC2_RESULT");
    status = gpu_read32(target, 0x40966c, &high);
    record_post_read(target, 5, status, high);
    V4_PHASE_CHECK("SEC2_RESULT");
    if (value || low != 0x88888888 || high != 8) status = EFI_DEVICE_ERROR;
    V4_PHASE_CHECK("SEC2_RESULT");
    status = gpu_read32(target, 0x409650, &value);
    record_post_read(target, 6, status, value);
    V4_PHASE_CHECK("SEC2_RESULT");
    if (value != 0xffffff8f) status = EFI_DEVICE_ERROR;
    V4_PHASE_CHECK("SEC2_RESULT");
    status = gpu_read32(target, 0x1fa824, &low);
    record_post_read(target, 7, status, low);
    V4_PHASE_CHECK("SEC2_RESULT");
    status = gpu_read32(target, 0x1fa828, &high);
    record_post_read(target, 8, status, high);
    V4_PHASE_CHECK("SEC2_RESULT");
    if (low != 0x1ffffe00 || high) status = EFI_DEVICE_ERROR;
    V4_PHASE_CHECK("SEC2_RESULT");
    V4_PHASE_END("SEC2_RESULT");

    V4_PHASE_BEGIN("FINAL_RESET");
    status = falcon_reset(target, TX_SEC);
    V4_PHASE_CHECK("FINAL_RESET");
    status = falcon_reset(target, TX_GSP);
    V4_PHASE_CHECK("FINAL_RESET");
    status = gpu_write32(target, TX_SEC + 0x040, 0);
    V4_PHASE_CHECK("FINAL_RESET");
    status = gpu_write32(target, TX_SEC + 0x044, 0);
    V4_PHASE_CHECK("FINAL_RESET");
    status = gpu_read32(target, TX_SEC + 0x100, &value);
    record_post_read(target, 9, status, value);
    V4_PHASE_CHECK("FINAL_RESET");
    if (value & 2u) status = EFI_DEVICE_ERROR;
    V4_PHASE_CHECK("FINAL_RESET");
    status = gpu_read32(target, TX_GSP + 0x100, &value);
    record_post_read(target, 10, status, value);
    V4_PHASE_CHECK("FINAL_RESET");
    if (value & 2u) status = EFI_DEVICE_ERROR;
    V4_PHASE_CHECK("FINAL_RESET");
    status = gpu_read32(target, 0x409664, &low);
    record_post_read(target, 11, status, low);
    V4_PHASE_CHECK("FINAL_RESET");
    status = gpu_read32(target, 0x40966c, &high);
    record_post_read(target, 12, status, high);
    V4_PHASE_CHECK("FINAL_RESET");
    if (low != 0x88888888 || high != 8) status = EFI_DEVICE_ERROR;
    V4_PHASE_CHECK("FINAL_RESET");
    __sync_synchronize();
    if (!target->pci->Flush) return EFI_UNSUPPORTED;
    status = uefi_call_wrapper(target->pci->Flush, 1, target->pci);
    V4_PHASE_CHECK("FINAL_RESET");
    target->quiescent = TRUE;
    V4_PHASE_END("FINAL_RESET");
    return EFI_SUCCESS;
}

#undef V4_PHASE_BEGIN
#undef V4_PHASE_CHECK
#undef V4_PHASE_END

#pragma pack(1)
typedef struct {
    EFI_GUID PartitionTypeGuid;
    EFI_GUID UniquePartitionGuid;
    EFI_LBA StartingLba;
    EFI_LBA EndingLba;
    UINT64 Attributes;
    CHAR16 PartitionName[36];
} TU102_PARTITION_INFO_GPT;

typedef struct {
    UINT32 Revision;
    UINT32 Type;
    UINT8 System;
    UINT8 Reserved[7];
    union {
        MBR_PARTITION_RECORD Mbr;
        TU102_PARTITION_INFO_GPT Gpt;
    } Info;
} TU102_PARTITION_INFO;
#pragma pack()

#define TU102_PARTITION_TYPE_GPT 2u
static EFI_GUID gPartitionInfoGuid = {0x8cf2f62c,0xbc9b,0x4821,
    {0x80,0x8d,0xec,0x9e,0xc4,0x21,0xa1,0xa0}};
static EFI_GUID gEspTypeGuid = {0xc12a7328,0xf81f,0x11d2,
    {0xba,0x4b,0x00,0xa0,0xc9,0x3e,0xc9,0x3b}};

static BOOLEAN guid_equal(const EFI_GUID *a, const EFI_GUID *b)
{
    const UINT8 *x = (const UINT8 *)a, *y = (const UINT8 *)b;
    UINTN i;
    for (i = 0; i < sizeof(EFI_GUID); ++i) if (x[i] != y[i]) return FALSE;
    return TRUE;
}

static BOOLEAN path_contains_bootmgfw(const EFI_DEVICE_PATH_PROTOCOL *path)
{
    static const CHAR16 expected[] = L"\\EFI\\Microsoft\\Boot\\bootmgfw.efi";
    const EFI_DEVICE_PATH_PROTOCOL *node = path;
    UINTN nodes = 0;
    while (node && nodes++ < 256 && !IsDevicePathEnd(node)) {
        UINTN node_length = DevicePathNodeLength(node);
        if (node_length < sizeof(EFI_DEVICE_PATH_PROTOCOL)) return FALSE;
        if (DevicePathType(node) == MEDIA_DEVICE_PATH &&
            DevicePathSubType(node) == MEDIA_FILEPATH_DP) {
            const FILEPATH_DEVICE_PATH *file = (const FILEPATH_DEVICE_PATH *)node;
            UINTN path_chars, i;
            if (node_length < SIZE_OF_FILEPATH_DEVICE_PATH + sizeof(CHAR16)) return FALSE;
            path_chars = (node_length - SIZE_OF_FILEPATH_DEVICE_PATH) / sizeof(CHAR16);
            for (i = 0; i < path_chars && expected[i] && file->PathName[i]; ++i) {
                CHAR16 a = file->PathName[i], b = expected[i];
                if (a >= L'a' && a <= L'z') a -= 32;
                if (b >= L'a' && b <= L'z') b -= 32;
                if (a != b) break;
            }
            if (i < path_chars && !expected[i] && !file->PathName[i]) return TRUE;
        }
        node = NextDevicePathNode(node);
    }
    return FALSE;
}

static EFI_STATUS locate_windows_image(EFI_HANDLE image, EFI_HANDLE *child_image)
{
    EFI_HANDLE *handles = NULL, selected = NULL;
    EFI_LOADED_IMAGE_PROTOCOL *current = NULL, *loaded = NULL;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs;
    EFI_FILE_HANDLE root = NULL, file = NULL;
    TU102_PARTITION_INFO *partition;
    EFI_DEVICE_PATH_PROTOCOL *target_path = NULL;
    UINTN count = 0, i, matches = 0;
    EFI_STATUS status, close_status;
    EFI_GUID fs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    static const CHAR16 win_path[] = L"\\EFI\\Microsoft\\Boot\\bootmgfw.efi";
    VOID *interface = NULL;
    status = uefi_call_wrapper(BS->HandleProtocol, 3, image,
                               &gEfiLoadedImageProtocolGuid, (VOID **)&current);
    if (EFI_ERROR(status) || !current) return EFI_SECURITY_VIOLATION;
    if (path_contains_bootmgfw(current->FilePath)) return EFI_SECURITY_VIOLATION;
    status = uefi_call_wrapper(BS->LocateHandleBuffer, 5, ByProtocol, &fs_guid,
                               NULL, &count, &handles);
    if (EFI_ERROR(status)) return status;
    for (i = 0; i < count; ++i) {
        status = uefi_call_wrapper(BS->HandleProtocol, 3, handles[i],
                                   &gPartitionInfoGuid, &interface);
        if (status == EFI_NOT_FOUND || status == EFI_UNSUPPORTED) continue;
        if (EFI_ERROR(status) || !interface) goto done;
        partition = (TU102_PARTITION_INFO *)interface;
        if (partition->Type != TU102_PARTITION_TYPE_GPT || !partition->System ||
            !guid_equal(&partition->Info.Gpt.PartitionTypeGuid, &gEspTypeGuid))
            continue;
        fs = NULL; root = NULL; file = NULL;
        status = uefi_call_wrapper(BS->HandleProtocol, 3, handles[i], &fs_guid,
                                   (VOID **)&fs);
        if (EFI_ERROR(status) || !fs) goto done;
        status = uefi_call_wrapper(fs->OpenVolume, 2, fs, &root);
        if (EFI_ERROR(status) || !root) goto done;
        status = uefi_call_wrapper(root->Open, 5, root, &file, (CHAR16 *)win_path,
                                   EFI_FILE_MODE_READ, 0);
        if (!EFI_ERROR(status) && file) {
            ++matches;
            selected = handles[i];
            close_status = uefi_call_wrapper(file->Close, 1, file);
            file = NULL;
            if (EFI_ERROR(close_status)) { status = close_status; goto done; }
        } else if (status != EFI_NOT_FOUND && status != EFI_NO_MEDIA) {
            goto done;
        }
        close_status = uefi_call_wrapper(root->Close, 1, root);
        root = NULL;
        if (EFI_ERROR(close_status)) { status = close_status; goto done; }
    }
    if (matches != 1 || !selected) { status = EFI_SECURITY_VIOLATION; goto done; }
    target_path = FileDevicePath(selected, (CHAR16 *)win_path);
    if (!target_path) { status = EFI_OUT_OF_RESOURCES; goto done; }
    if (current->DeviceHandle == selected &&
        path_contains_bootmgfw(current->FilePath)) {
        status = EFI_SECURITY_VIOLATION;
        goto done;
    }
    status = uefi_call_wrapper(BS->LoadImage, 6, FALSE, image, target_path,
                               NULL, 0, child_image);
    if (EFI_ERROR(status)) goto done;
    status = uefi_call_wrapper(BS->HandleProtocol, 3, *child_image,
                               &gEfiLoadedImageProtocolGuid, (VOID **)&loaded);
    if (EFI_ERROR(status) || !loaded || loaded->DeviceHandle != selected ||
        !path_contains_bootmgfw(loaded->FilePath)) {
        status = uefi_call_wrapper(BS->UnloadImage, 1, *child_image);
        if (!EFI_ERROR(status)) {
            *child_image = NULL;
            status = EFI_SECURITY_VIOLATION;
        }
        goto done;
    }
    status = EFI_SUCCESS;
done:
    if (file) {
        EFI_STATUS close_result = uefi_call_wrapper(file->Close, 1, file);
        if (!EFI_ERROR(status) && EFI_ERROR(close_result)) status = close_result;
    }
    if (root) {
        EFI_STATUS close_result = uefi_call_wrapper(root->Close, 1, root);
        if (!EFI_ERROR(status) && EFI_ERROR(close_result)) status = close_result;
    }
    if (target_path) {
        EFI_STATUS free_status = uefi_call_wrapper(BS->FreePool, 1, target_path);
        if (!EFI_ERROR(status) && EFI_ERROR(free_status)) status = free_status;
    }
    if (handles) {
        EFI_STATUS free_status = uefi_call_wrapper(BS->FreePool, 1, handles);
        if (!EFI_ERROR(status) && EFI_ERROR(free_status)) status = free_status;
    }
    return status;
}

#ifdef TU102_HOST_TEST
typedef EFI_STATUS (*TU102_HOST_TX_HOOK)(GPU_TARGET *target);
static TU102_HOST_TX_HOOK gHostTransactionHook;
#endif

static EFI_STATUS __attribute__((unused)) run_profile_transaction(GPU_TARGET *target)
{
#ifdef TU102_HOST_TEST
    if (gHostTransactionHook) return gHostTransactionHook(target);
#endif
    return execute_profile(target);
}

static VOID fail_halt(VOID)
{
#ifdef TU102_HOST_TEST
    extern VOID tu102_host_fail_halt(VOID);
    tu102_host_fail_halt();
#else
    for (;;) __asm__ __volatile__("cli; hlt" ::: "memory");
#endif
}
#endif /* ENABLE_EXPERIMENT */

/* GNU-EFI's x86_64 crt0 converts the firmware's EFI ABI arguments before
 * calling libefi's _entry, which calls efi_main with the compiler's SysV ABI.
 * Keep this entry itself unannotated; EFI service/protocol callbacks retain
 * EFIAPI and are invoked through uefi_call_wrapper. */
#ifndef TU102_DUAL_ATTEMPT
EFI_STATUS efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
#ifdef TU102_HOST_TEST
    ST = SystemTable;
    BS = SystemTable ? SystemTable->BootServices : NULL;
    RT = SystemTable ? SystemTable->RuntimeServices : NULL;
#else
    InitializeLib(ImageHandle, SystemTable);
#endif
#if !ENABLE_EXPERIMENT
    (void)ImageHandle;
    Print(L"TU102 preboot transaction disabled (ENABLE_EXPERIMENT=0).\n");
    return EFI_UNSUPPORTED;
#else
    GPU_TARGET targets[2] = {{0}};
    EFI_STATUS status, cleanup_status;
    EFI_HANDLE windows_image = NULL;
    UINTN i;
    BOOLEAN any_mutated = FALSE;
    (void)ImageHandle;
    if (!BS) return EFI_UNSUPPORTED;
    status = enumerate_targets(targets);
    if (EFI_ERROR(status)) {
        Print(L"Preflight refused before mutation: %r\n", status);
        return status;
    }
#ifndef TU102_HOST_TEST
    if (!g_Profiles[0].hardware_validated || !g_Profiles[1].hardware_validated) {
        Print(L"Profiles are not hardware validated; experiment refused.\n");
        return EFI_UNSUPPORTED;
    }
#endif
    status = locate_windows_image(ImageHandle, &windows_image);
    if (EFI_ERROR(status)) {
        Print(L"Unique verified Windows Boot Manager ESP target not found: %r\n", status);
        return status;
    }
    /* Allocate and map both per-device common buffers before the first GPU
     * write. A partial allocation is unwound while every engine is untouched. */
    for (i = 0; i < 2; ++i) {
        status = prepare_dma_pool(&targets[i]);
        if (EFI_ERROR(status)) goto safe_prewrite_failure;
    }
    for (i = 0; i < 2; ++i) {
        status = run_profile_transaction(&targets[i]);
        any_mutated = any_mutated || targets[i].mutated;
        if (EFI_ERROR(status) || !targets[i].quiescent || !targets[i].mutated) {
            Print(L"Transaction failed on %s; mutated=%u quiescent=%u status=%r\n",
                  targets[i].profile->name, targets[i].mutated,
                  targets[i].quiescent, status);
            if (any_mutated) fail_halt();
            goto safe_prewrite_failure;
        }
    }
    /* DMA buffers are released only after tx_run-style engine reset, halt,
     * mailbox, selector and WPR verification set quiescent on both cards. */
    for (i = 0; i < 2; ++i) {
        cleanup_status = cleanup_dma_pool(&targets[i]);
        if (EFI_ERROR(cleanup_status)) {
            Print(L"DMA cleanup failed after mutation: %r\n", cleanup_status);
            fail_halt();
        }
    }
    {
        UINTN exit_data_size = 0;
        CHAR16 *exit_data = NULL;
        status = uefi_call_wrapper(BS->StartImage, 3, windows_image,
                                  &exit_data_size, &exit_data);
        if (exit_data) {
            cleanup_status = uefi_call_wrapper(BS->FreePool, 1, exit_data);
            if (EFI_ERROR(cleanup_status))
                Print(L"Failed to release Boot Manager exit data: %r\n", cleanup_status);
        }
    }
    Print(L"Windows Boot Manager returned unexpectedly: %r\n", status);
    if (any_mutated) fail_halt();
    return EFI_DEVICE_ERROR;

safe_prewrite_failure:
    if (any_mutated) fail_halt();
    for (i = 0; i < 2; ++i) {
        cleanup_status = cleanup_dma_pool(&targets[i]);
        if (EFI_ERROR(cleanup_status)) fail_halt();
    }
    if (windows_image) {
        cleanup_status = uefi_call_wrapper(BS->UnloadImage, 1, windows_image);
        if (EFI_ERROR(cleanup_status)) return cleanup_status;
    }
    return status;
#endif
}
#endif
#ifdef TU102_DUAL_ATTEMPT
#undef Print
#endif
