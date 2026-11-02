/* SPDX-License-Identifier: GPL-2.0-only */
/* Bounded journal and exact NVIDIA nvgop detach support for a one-shot attempt. */
#include <efi.h>
#include <efilib.h>
#include <efigpt.h>
#include <efipart.h>
#include <efidevp.h>
#include <string.h>
#include "attempt_support.h"

#define DUAL_MAX_HANDLES 128u
#define DUAL_MAX_OPEN_INFO 64u
#define DUAL_MAX_DEVICE_PATH_NODES 256u
#define DUAL_STAGE_CAP 48u
#define DUAL_JOURNAL_HEADER "TU102_DUAL_ATTEMPT_LOG_V1\n"
#define DUAL_PLAN_HEADER "TU102_RESOURCE_PLAN_LOG_V1\n"

#define DUAL_PARTITION_INFO_MBR 1u
#define DUAL_PARTITION_INFO_GPT 2u
#define DUAL_PCI_COMMAND_OFFSET 0x04u
#define DUAL_PCI_COMMAND_MEMORY 0x0002u
#define DUAL_PCI_COMMAND_BUS_MASTER 0x0004u
#define DUAL_PCI_MEMORY_ATTRIBUTES \
    (EFI_PCI_IO_ATTRIBUTE_MEMORY | EFI_PCI_IO_ATTRIBUTE_DUAL_ADDRESS_CYCLE)

#define EFI_PARTITION_INFO_PROTOCOL_GUID \
    {0x8cf2f62c,0xbc9b,0x4821,{0x80,0x8d,0xec,0x9e,0xc4,0x21,0xa1,0xa0}}

typedef struct {
    UINT32 Revision;
    UINT32 Type;
    UINT8 System;
    UINT8 Reserved[7];
    union { MBR_PARTITION_RECORD Mbr; EFI_PARTITION_ENTRY Gpt; } Info;
} DUAL_PARTITION_INFO;

static EFI_GUID g_sfs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
static EFI_GUID g_block_io_guid = EFI_BLOCK_IO_PROTOCOL_GUID;
static EFI_GUID g_device_path_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;
static EFI_GUID g_partition_info_guid = EFI_PARTITION_INFO_PROTOCOL_GUID;
static EFI_GUID g_pci_io_guid = EFI_PCI_IO_PROTOCOL_GUID;
static EFI_GUID g_loaded_image_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
static EFI_GUID g_driver_binding_guid = EFI_DRIVER_BINDING_PROTOCOL_GUID;
static EFI_GUID g_device_path_to_text_guid = EFI_DEVICE_PATH_TO_TEXT_PROTOCOL_GUID;
static EFI_GUID g_component_name2_guid = EFI_COMPONENT_NAME2_PROTOCOL_GUID;
static EFI_GUID g_file_info_guid = EFI_FILE_INFO_ID;
static EFI_GUID g_esp_guid =
    {0xc12a7328,0xf81f,0x11d2,{0xba,0x4b,0x00,0xa0,0xc9,0x3e,0xc9,0x3b}};

static void dual_plan_set_path(UINTN number, const char *id, UINTN id_len,
                               CHAR16 *path);
static BOOLEAN dual_plan_id_valid(const char *id, UINTN *length);

typedef struct {
    UINTN Segment, Bus, Device, Function;
    UINT16 Vendor, Product, SubVendor, SubProduct;
} DUAL_PROFILE;

static const DUAL_PROFILE g_profiles[2] = {
    {0,0x17,0,0,0x10de,0x1e04,0x10de,0x1e04},
    {0,0x65,0,0,0x10de,0x1e04,0x10de,0x1e04}
};

static BOOLEAN dual_guid_equal(const EFI_GUID *a, const EFI_GUID *b)
{
    UINTN i;
    if (!a || !b || a->Data1 != b->Data1 || a->Data2 != b->Data2 ||
        a->Data3 != b->Data3) return FALSE;
    for (i = 0; i < sizeof(a->Data4); ++i)
        if (a->Data4[i] != b->Data4[i]) return FALSE;
    return TRUE;
}

static EFI_STATUS dual_preserve_status(EFI_STATUS primary, EFI_STATUS next)
{
    return EFI_ERROR(primary) ? primary : next;
}

static void dual_set_first_error(DUAL_JOURNAL *j, EFI_STATUS status)
{
    if (j && EFI_ERROR(status) && !EFI_ERROR(j->FirstError))
        j->FirstError = status;
}

static void dual_set_path(UINTN number, CHAR16 *path)
{
    static const CHAR16 prefix[] = L"\\TU102-ATTEMPT-";
    static const CHAR16 suffix[] = L".LOG";
    UINTN i = 0;
    while (prefix[i]) { path[i] = prefix[i]; ++i; }
    path[i++] = (CHAR16)('0' + ((number / 1000u) % 10u));
    path[i++] = (CHAR16)('0' + ((number / 100u) % 10u));
    path[i++] = (CHAR16)('0' + ((number / 10u) % 10u));
    path[i++] = (CHAR16)('0' + (number % 10u));
    {
        UINTN n = 0;
        while (suffix[n]) path[i++] = suffix[n++];
    }
    path[i] = 0;
}

static BOOLEAN dual_is_end(const EFI_DEVICE_PATH_PROTOCOL *node)
{
    return node && DevicePathType(node) == END_DEVICE_PATH_TYPE &&
           DevicePathSubType(node) == END_ENTIRE_DEVICE_PATH_SUBTYPE;
}

static EFI_STATUS dual_inspect_safe_volume(EFI_SYSTEM_TABLE *st, EFI_HANDLE h,
                                            BOOLEAN *is_safe)
{
    EFI_DEVICE_PATH_PROTOCOL *path = NULL;
    EFI_BLOCK_IO_PROTOCOL *block = NULL;
    DUAL_PARTITION_INFO *partition = NULL;
    EFI_STATUS s;
    UINTN n;
    BOOLEAN saw_end = FALSE, has_usb = FALSE, has_hd = FALSE;
    BOOLEAN removable = FALSE, logical = FALSE, partition_known = FALSE;
    BOOLEAN partition_protocol = FALSE, is_esp = FALSE;

    *is_safe = FALSE;
    s = uefi_call_wrapper(st->BootServices->HandleProtocol, 3, h,
                          &g_device_path_guid, (VOID **)&path);
    if (EFI_ERROR(s) || !path) return EFI_ERROR(s) ? s : EFI_UNSUPPORTED;
    for (n = 0; n < DUAL_MAX_DEVICE_PATH_NODES; ++n) {
        UINTN len;
        UINT8 type, sub;
        if (dual_is_end(path)) { saw_end = TRUE; break; }
        len = DevicePathNodeLength(path);
        if (len < sizeof(EFI_DEVICE_PATH_PROTOCOL) || len > 4096u)
            return EFI_COMPROMISED_DATA;
        type = DevicePathType(path); sub = DevicePathSubType(path);
        if (type == MESSAGING_DEVICE_PATH &&
            (sub == MSG_USB_DP || sub == MSG_USB_CLASS_DP || sub == MSG_USB_WWID_DP))
            has_usb = TRUE;
        if (type == MEDIA_DEVICE_PATH && sub == MEDIA_HARDDRIVE_DP) has_hd = TRUE;
        path = NextDevicePathNode(path);
    }
    if (!saw_end) return EFI_COMPROMISED_DATA;

    s = uefi_call_wrapper(st->BootServices->HandleProtocol, 3, h,
                          &g_block_io_guid, (VOID **)&block);
    if (!EFI_ERROR(s) && block && block->Media) {
        removable = block->Media->RemovableMedia;
        logical = block->Media->LogicalPartition;
    } else if (s != EFI_NOT_FOUND && s != EFI_UNSUPPORTED) {
        return s;
    }
    s = uefi_call_wrapper(st->BootServices->HandleProtocol, 3, h,
                          &g_partition_info_guid, (VOID **)&partition);
    if (EFI_ERROR(s)) {
        if (s != EFI_NOT_FOUND && s != EFI_UNSUPPORTED) return s;
    } else if (!partition) {
        return EFI_COMPROMISED_DATA;
    } else {
        partition_protocol = TRUE;
        if (partition->Type == DUAL_PARTITION_INFO_GPT) {
            partition_known = TRUE;
            is_esp = dual_guid_equal(&partition->Info.Gpt.PartitionTypeGUID,
                                     &g_esp_guid);
        } else if (partition->Type == DUAL_PARTITION_INFO_MBR) {
            partition_known = TRUE;
            is_esp = partition->Info.Mbr.OSIndicator == EFI_PARTITION;
        } else {
            return EFI_UNSUPPORTED;
        }
    }
    if (!partition_known && (partition_protocol ||
        !(has_usb && removable && !has_hd && !logical)))
        return EFI_UNSUPPORTED;
    if (partition_known && partition->System != 0) return EFI_ACCESS_DENIED;
    if (is_esp || !(has_usb || removable)) return EFI_ACCESS_DENIED;
    *is_safe = TRUE;
    return EFI_SUCCESS;
}

static EFI_STATUS dual_has_marker(EFI_SYSTEM_TABLE *st, EFI_HANDLE volume,
                                  BOOLEAN *found)
{
    static CHAR16 marker_name[] = L"\\TU102-DIAG-OUTPUT.TAG";
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *sfs = NULL;
    EFI_FILE_PROTOCOL *root = NULL, *marker = NULL;
    EFI_STATUS s, close_s;
    *found = FALSE;
    s = uefi_call_wrapper(st->BootServices->HandleProtocol, 3, volume,
                          &g_sfs_guid, (VOID **)&sfs);
    if (EFI_ERROR(s) || !sfs) return EFI_ERROR(s) ? s : EFI_UNSUPPORTED;
    s = uefi_call_wrapper(sfs->OpenVolume, 2, sfs, &root);
    if (EFI_ERROR(s) || !root) return EFI_ERROR(s) ? s : EFI_DEVICE_ERROR;
    s = uefi_call_wrapper(root->Open, 5, root, &marker, marker_name,
                          EFI_FILE_MODE_READ, 0);
    if (!EFI_ERROR(s) && marker) {
        *found = TRUE;
        close_s = uefi_call_wrapper(marker->Close, 1, marker);
        if (EFI_ERROR(close_s)) s = close_s;
    } else if (!EFI_ERROR(s)) {
        s = EFI_DEVICE_ERROR;
    } else if (s == EFI_NOT_FOUND) {
        s = EFI_SUCCESS;
    }
    close_s = uefi_call_wrapper(root->Close, 1, root);
    if (!EFI_ERROR(s) && EFI_ERROR(close_s)) s = close_s;
    return s;
}

static EFI_STATUS dual_candidate_volumes(EFI_SYSTEM_TABLE *st,
                                         EFI_HANDLE **out_handles,
                                         UINTN *out_count)
{
    EFI_HANDLE *handles = NULL;
    UINTN count = 0, i;
    EFI_STATUS s;
    EFI_LOADED_IMAGE_PROTOCOL *loaded = NULL;
    EFI_HANDLE own = NULL;
    if (!st->BootServices->LocateHandleBuffer || !st->BootServices->FreePool)
        return EFI_UNSUPPORTED;
    s = uefi_call_wrapper(st->BootServices->LocateHandleBuffer, 5, ByProtocol,
                          &g_sfs_guid, NULL, &count, &handles);
    if (EFI_ERROR(s)) return s;
    if (count > DUAL_MAX_HANDLES || (count && !handles)) {
        if (handles) (void)uefi_call_wrapper(st->BootServices->FreePool, 1, handles);
        return EFI_OUT_OF_RESOURCES;
    }
    /* Caller adds ImageHandle's volume after this SFS enumeration if needed. */
    (void)loaded; (void)own; (void)i;
    *out_handles = handles; *out_count = count;
    return EFI_SUCCESS;
}

static UINTN dual_strlen(const char *s, UINTN limit)
{
    UINTN n = 0;
    if (!s) return 0;
    while (n < limit && s[n]) ++n;
    return n;
}

/* Stage text was length-checked by the caller. Keep this comparison local so
 * standalone EFI builds do not depend on a host C library's strcmp. */
static BOOLEAN dual_stage_equal(const char *stage, UINTN length,
                                 const char *expected, UINTN expected_length)
{
    UINTN i;
    if (length != expected_length) return FALSE;
    for (i = 0; i < length; ++i)
        if (stage[i] != expected[i]) return FALSE;
    return TRUE;
}

static BOOLEAN dual_valid_stage(const char *stage, UINTN length)
{
    UINTN i;
    if (!stage || !length || length >= DUAL_STAGE_CAP) return FALSE;
    for (i = 0; i < length; ++i)
        if (!((stage[i] >= 'A' && stage[i] <= 'Z') ||
              (stage[i] >= '0' && stage[i] <= '9') || stage[i] == '_'))
            return FALSE;
    return TRUE;
}

static EFI_STATUS dual_write_exact(DUAL_JOURNAL *j, const CHAR8 *data, UINTN n)
{
    UINTN written = n;
    EFI_STATUS s;
    if (!j || !j->Open || !j->File || !j->File->Write) return EFI_NOT_READY;
    if (n > DUAL_JOURNAL_MAX_BYTES - j->BytesWritten) return EFI_OUT_OF_RESOURCES;
    s = uefi_call_wrapper(j->File->Write, 3, j->File, &written, (VOID *)data);
    if (EFI_ERROR(s)) return s;
    j->BytesWritten += written;
    if (written != n) { j->Truncated = TRUE; return EFI_BAD_BUFFER_SIZE; }
    return EFI_SUCCESS;
}

static EFI_STATUS dual_file_info_size(EFI_FILE_PROTOCOL *file, UINT64 *file_size)
{
    union { UINT64 Align; UINT8 Bytes[SIZE_OF_EFI_FILE_INFO + 64u]; } storage;
    EFI_FILE_INFO *info = (EFI_FILE_INFO *)storage.Bytes;
    UINTN size = sizeof(storage.Bytes);
    EFI_STATUS s;
    if (!file->GetInfo) return EFI_UNSUPPORTED;
    s = uefi_call_wrapper(file->GetInfo, 4, file, &g_file_info_guid, &size, info);
    if (EFI_ERROR(s)) return s;
    if (size < SIZE_OF_EFI_FILE_INFO || info->Size < SIZE_OF_EFI_FILE_INFO ||
        info->Size > size) return EFI_COMPROMISED_DATA;
    *file_size = info->FileSize;
    return EFI_SUCCESS;
}

static EFI_STATUS dual_any_plan_file_exists(EFI_FILE_PROTOCOL *root,
                                            UINTN number, BOOLEAN *found)
{
    static const char *ids[] = {"A", "B", "C", "D", "AUTO"};
    EFI_FILE_PROTOCOL *file = NULL;
    CHAR16 path[64];
    UINTN i, length;
    EFI_STATUS s, close_s;
    *found = FALSE;
    for (i = 0; i < sizeof(ids) / sizeof(ids[0]); ++i) {
        if (!dual_plan_id_valid(ids[i], &length)) return EFI_COMPROMISED_DATA;
        dual_plan_set_path(number, ids[i], length, path);
        s = uefi_call_wrapper(root->Open, 5, root, &file, path,
                              EFI_FILE_MODE_READ, 0);
        if (!EFI_ERROR(s) && file) {
            *found = TRUE;
            close_s = uefi_call_wrapper(file->Close, 1, file);
            return close_s;
        }
        if (s != EFI_NOT_FOUND) return EFI_ERROR(s) ? s : EFI_DEVICE_ERROR;
    }
    return EFI_SUCCESS;
}

static EFI_STATUS dual_open_new_file(DUAL_JOURNAL *j)
{
    UINTN number;
    EFI_FILE_PROTOCOL *existing = NULL, *file = NULL;
    EFI_STATUS s, close_s;
    BOOLEAN orphan_plan;
    UINT64 file_size;
    for (number = 0; number <= DUAL_JOURNAL_MAX_SESSION_NUMBER; ++number) {
        dual_set_path(number, j->Path);
        s = uefi_call_wrapper(j->Root->Open, 5, j->Root, &existing, j->Path,
                              EFI_FILE_MODE_READ, 0);
        if (!EFI_ERROR(s) && existing) {
            close_s = uefi_call_wrapper(existing->Close, 1, existing);
            existing = NULL;
            if (EFI_ERROR(close_s)) return close_s;
            continue;
        }
        if (s != EFI_NOT_FOUND) return EFI_ERROR(s) ? s : EFI_DEVICE_ERROR;
        s = dual_any_plan_file_exists(j->Root, number, &orphan_plan);
        if (EFI_ERROR(s)) return s;
        if (orphan_plan) continue;
        s = uefi_call_wrapper(j->Root->Open, 5, j->Root, &file, j->Path,
                              EFI_FILE_MODE_CREATE | EFI_FILE_MODE_READ |
                              EFI_FILE_MODE_WRITE, EFI_FILE_ARCHIVE);
        if (EFI_ERROR(s) || !file) return EFI_ERROR(s) ? s : EFI_DEVICE_ERROR;
        s = dual_file_info_size(file, &file_size);
        if (EFI_ERROR(s)) {
            (void)uefi_call_wrapper(file->Close, 1, file);
            return s;
        }
        if (file_size != 0) {
            close_s = uefi_call_wrapper(file->Close, 1, file);
            if (EFI_ERROR(close_s)) return close_s;
            file = NULL;
            continue;
        }
        j->File = file;
        j->SessionNumber = number;
        j->Open = TRUE;
        return EFI_SUCCESS;
    }
    return EFI_OUT_OF_RESOURCES;
}

static EFI_STATUS dual_locate_unique_journal_volume(EFI_HANDLE image,
                                                   EFI_SYSTEM_TABLE *st,
                                                   EFI_HANDLE *volume_out,
                                                   EFI_FILE_PROTOCOL **root_out)
{
    EFI_HANDLE *handles = NULL, own = NULL, selected = NULL, h;
    EFI_LOADED_IMAGE_PROTOCOL *loaded = NULL;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *sfs = NULL;
    EFI_FILE_PROTOCOL *root = NULL;
    UINTN count = 0, total, i, j, matches = 0;
    BOOLEAN safe, marker, duplicate;
    EFI_STATUS s, cleanup_s;
    s = dual_candidate_volumes(st, &handles, &count);
    if (EFI_ERROR(s)) return s;
    s = uefi_call_wrapper(st->BootServices->HandleProtocol, 3, image,
                          &g_loaded_image_guid, (VOID **)&loaded);
    if (!EFI_ERROR(s) && loaded) own = loaded->DeviceHandle;
    total = count + (own ? 1u : 0u);
    if (total > DUAL_MAX_HANDLES + 1u) { s = EFI_OUT_OF_RESOURCES; goto done; }
    for (i = 0; i < total; ++i) {
        h = i < count ? handles[i] : own;
        duplicate = FALSE;
        for (j = 0; j < i && j < count; ++j)
            if (handles[j] == h) { duplicate = TRUE; break; }
        if (duplicate || !h) continue;
        s = uefi_call_wrapper(st->BootServices->HandleProtocol, 3, h,
                              &g_sfs_guid, (VOID **)&sfs);
        if (EFI_ERROR(s) || !sfs) continue;
        s = dual_inspect_safe_volume(st, h, &safe);
        if (s == EFI_ACCESS_DENIED || s == EFI_UNSUPPORTED) continue;
        if (EFI_ERROR(s)) goto done;
        if (!safe) continue;
        s = dual_has_marker(st, h, &marker);
        if (EFI_ERROR(s)) goto done;
        if (!marker) continue;
        selected = h;
        ++matches;
    }
    if (matches != 1u) { s = matches ? EFI_ACCESS_DENIED : EFI_NOT_FOUND; goto done; }
    s = uefi_call_wrapper(st->BootServices->HandleProtocol, 3, selected,
                          &g_sfs_guid, (VOID **)&sfs);
    if (EFI_ERROR(s) || !sfs) { s = EFI_UNSUPPORTED; goto done; }
    s = uefi_call_wrapper(sfs->OpenVolume, 2, sfs, &root);
    if (EFI_ERROR(s) || !root) { s = EFI_ERROR(s) ? s : EFI_DEVICE_ERROR; goto done; }
    *volume_out = selected;
    *root_out = root;
    root = NULL;
    s = EFI_SUCCESS;
done:
    if (root) {
        cleanup_s = uefi_call_wrapper(root->Close, 1, root);
        if (!EFI_ERROR(s) && EFI_ERROR(cleanup_s)) s = cleanup_s;
    }
    if (handles) {
        cleanup_s = uefi_call_wrapper(st->BootServices->FreePool, 1, handles);
        if (!EFI_ERROR(s) && EFI_ERROR(cleanup_s)) s = cleanup_s;
    }
    return s;
}

EFI_STATUS DualJournalOpen(EFI_HANDLE image, EFI_SYSTEM_TABLE *st,
                           DUAL_JOURNAL *j)
{
    EFI_STATUS s, close_s;
    UINTN header_size = sizeof(DUAL_JOURNAL_HEADER) - 1u;
    if (!image || !st || !st->BootServices || !j) return EFI_INVALID_PARAMETER;
    memset(j, 0, sizeof(*j));
    j->FirstError = EFI_SUCCESS;
    s = dual_locate_unique_journal_volume(image, st, &j->Volume, &j->Root);
    if (EFI_ERROR(s)) goto fail;
    s = dual_open_new_file(j);
    if (EFI_ERROR(s)) goto fail;
    s = dual_write_exact(j, (const CHAR8 *)DUAL_JOURNAL_HEADER, header_size);
    if (EFI_ERROR(s)) goto fail;
    s = uefi_call_wrapper(j->File->Flush, 1, j->File);
    if (EFI_ERROR(s)) goto fail;
    return EFI_SUCCESS;
fail:
    dual_set_first_error(j, s);
    if (j->File) {
        close_s = uefi_call_wrapper(j->File->Close, 1, j->File);
        j->File = NULL;
        if (!EFI_ERROR(s) && EFI_ERROR(close_s)) s = close_s;
    }
    if (j->Root) {
        close_s = uefi_call_wrapper(j->Root->Close, 1, j->Root);
        j->Root = NULL;
        if (!EFI_ERROR(s) && EFI_ERROR(close_s)) s = close_s;
    }
    j->Open = FALSE;
    dual_set_first_error(j, s);
    return s;
}

EFI_STATUS DualJournalRecord(DUAL_JOURNAL *j, const char *stage,
                             EFI_STATUS status, const char *detail)
{
    CHAR8 line[DUAL_JOURNAL_MAX_LINE];
    static const CHAR8 hex[] = "0123456789abcdef";
    UINTN n = 0, stage_len, detail_len, i, k;
    UINT64 value;
    EFI_STATUS s, flush_s;
    if (!j || !j->Open || !j->File) return EFI_NOT_READY;
    stage_len = dual_strlen(stage, DUAL_STAGE_CAP);
    detail_len = dual_strlen(detail, DUAL_JOURNAL_MAX_DETAIL + 1u);
    if (!dual_valid_stage(stage, stage_len) || stage_len >= DUAL_STAGE_CAP ||
        detail_len > DUAL_JOURNAL_MAX_DETAIL) {
        s = EFI_INVALID_PARAMETER;
        dual_set_first_error(j, s);
        return s;
    }
    if (!EFI_ERROR(status) && j->BytesWritten + 1u + stage_len + 1u + 18u + 1u +
        detail_len + 1u > DUAL_JOURNAL_MAX_BYTES - DUAL_JOURNAL_RESERVED_FAILURE_BYTES) {
        j->Truncated = TRUE;
        s = EFI_OUT_OF_RESOURCES;
        dual_set_first_error(j, s);
        return s;
    }
    for (i = 0; i < stage_len; ++i) line[n++] = (CHAR8)stage[i];
    line[n++] = '\t';
    line[n++] = '0'; line[n++] = 'x';
    value = (UINT64)status;
    for (i = 0; i < 16u; ++i) {
        UINTN shift = (15u - i) * 4u;
        line[n++] = hex[(value >> shift) & 0x0fu];
    }
    line[n++] = '\t';
    for (k = 0; k < detail_len; ++k) {
        UINT8 ch = (UINT8)detail[k];
        line[n++] = (ch >= 0x20u && ch <= 0x7eu && ch != '\t') ?
                    (CHAR8)ch : (CHAR8)' ';
    }
    line[n++] = '\n';
    s = dual_write_exact(j, line, n);
    if (EFI_ERROR(s)) { dual_set_first_error(j, s); return s; }
    flush_s = uefi_call_wrapper(j->File->Flush, 1, j->File);
    if (EFI_ERROR(flush_s)) dual_set_first_error(j, flush_s);
    dual_set_first_error(j, status);
    return EFI_ERROR(flush_s) ? flush_s : EFI_SUCCESS;
}

EFI_STATUS DualJournalFlush(DUAL_JOURNAL *j)
{
    EFI_STATUS s;
    if (!j || !j->Open || !j->File || !j->File->Flush) return EFI_NOT_READY;
    s = uefi_call_wrapper(j->File->Flush, 1, j->File);
    dual_set_first_error(j, s);
    return s;
}

EFI_STATUS DualJournalClose(DUAL_JOURNAL *j)
{
    EFI_STATUS s = EFI_SUCCESS, next;
    if (!j) return EFI_INVALID_PARAMETER;
    if (j->File) {
        if (j->File->Flush) {
            next = uefi_call_wrapper(j->File->Flush, 1, j->File);
            s = dual_preserve_status(s, next);
        }
        next = uefi_call_wrapper(j->File->Close, 1, j->File);
        s = dual_preserve_status(s, next);
        j->File = NULL;
    }
    if (j->Root) {
        next = uefi_call_wrapper(j->Root->Close, 1, j->Root);
        s = dual_preserve_status(s, next);
        j->Root = NULL;
    }
    j->Open = FALSE;
    dual_set_first_error(j, s);
    return EFI_ERROR(j->FirstError) ? j->FirstError : s;
}

static BOOLEAN dual_plan_id_valid(const char *id, UINTN *length)
{
    UINTN n = dual_strlen(id, 6u);
    if (!length) return FALSE;
    if (id && ((n == 1u && (id[0] == 'A' || id[0] == 'B' ||
                            id[0] == 'C' || id[0] == 'D')) ||
               (n == 4u && id[0] == 'A' && id[1] == 'U' &&
                           id[2] == 'T' && id[3] == 'O'))) {
        *length = n;
        return TRUE;
    }
    *length = 0;
    return FALSE;
}

static void dual_plan_set_path(UINTN number, const char *id, UINTN id_len,
                               CHAR16 *path)
{
    static const CHAR16 prefix[] = L"\\TU102-ATTEMPT-";
    static const CHAR16 plan[] = L"-PLAN-";
    static const CHAR16 suffix[] = L".LOG";
    UINTN i = 0, n;
    while (prefix[i]) { path[i] = prefix[i]; ++i; }
    path[i++] = (CHAR16)('0' + ((number / 1000u) % 10u));
    path[i++] = (CHAR16)('0' + ((number / 100u) % 10u));
    path[i++] = (CHAR16)('0' + ((number / 10u) % 10u));
    path[i++] = (CHAR16)('0' + (number % 10u));
    for (n = 0; plan[n]; ++n) path[i++] = plan[n];
    for (n = 0; n < id_len; ++n) path[i++] = (CHAR16)id[n];
    for (n = 0; suffix[n]; ++n) path[i++] = suffix[n];
    path[i] = 0;
}

static EFI_STATUS dual_plan_write_exact(DUAL_PLAN_REPORT *report,
                                        const CHAR8 *data, UINTN size)
{
    UINTN written = size;
    EFI_STATUS s;
    if (!report || !report->Open || !report->File || !report->File->Write)
        return EFI_NOT_READY;
    if (size > DUAL_JOURNAL_MAX_BYTES - report->BytesWritten)
        return EFI_OUT_OF_RESOURCES;
    s = uefi_call_wrapper(report->File->Write, 3, report->File, &written,
                          (VOID *)data);
    if (EFI_ERROR(s)) return s;
    report->BytesWritten += written;
    if (written != size) {
        report->Truncated = TRUE;
        return EFI_BAD_BUFFER_SIZE;
    }
    return EFI_SUCCESS;
}

static void dual_plan_start_detail(const DUAL_PLAN_REPORT *report, char *detail,
                                   UINTN cap)
{
    static const char prefix[] = "plan_id=";
    static const char session[] = " session_number=";
    static const char type[] = " report_type=RESOURCE_PROBE";
    UINTN n = 0, i, number;
    for (i = 0; prefix[i] && n + 1u < cap; ++i) detail[n++] = prefix[i];
    for (i = 0; report->PlanId[i] && n + 1u < cap; ++i) detail[n++] = report->PlanId[i];
    for (i = 0; session[i] && n + 1u < cap; ++i) detail[n++] = session[i];
    number = report->SessionNumber;
    if (cap > n + 4u) {
        detail[n++] = (char)('0' + ((number / 1000u) % 10u));
        detail[n++] = (char)('0' + ((number / 100u) % 10u));
        detail[n++] = (char)('0' + ((number / 10u) % 10u));
        detail[n++] = (char)('0' + (number % 10u));
    }
    for (i = 0; type[i] && n + 1u < cap; ++i) detail[n++] = type[i];
    detail[n < cap ? n : cap - 1u] = 0;
}

EFI_STATUS DualPlanReportOpen(const DUAL_JOURNAL *parent, const char *plan_id,
                              DUAL_PLAN_REPORT *report)
{
    EFI_FILE_PROTOCOL *existing = NULL, *file = NULL;
    UINTN id_len, i;
    UINT64 file_size = 0;
    EFI_STATUS s, close_s;
    if (!parent || !parent->Open || !parent->Root || !parent->File || !report ||
        !dual_plan_id_valid(plan_id, &id_len)) return EFI_INVALID_PARAMETER;
    memset(report, 0, sizeof(*report));
    report->FirstError = EFI_SUCCESS;
    report->SessionNumber = parent->SessionNumber;
    report->Root = parent->Root;
    for (i = 0; i < id_len; ++i) report->PlanId[i] = (CHAR8)plan_id[i];
    report->PlanId[id_len] = 0;
    dual_plan_set_path(report->SessionNumber, plan_id, id_len, report->Path);

    /* Refuse stale/duplicate names; CREATE must never be used to overwrite. */
    s = uefi_call_wrapper(report->Root->Open, 5, report->Root, &existing,
                          report->Path, EFI_FILE_MODE_READ, 0);
    if (!EFI_ERROR(s) && existing) {
        close_s = uefi_call_wrapper(existing->Close, 1, existing);
        return EFI_ERROR(close_s) ? close_s : EFI_ACCESS_DENIED;
    }
    if (s != EFI_NOT_FOUND) return EFI_ERROR(s) ? s : EFI_DEVICE_ERROR;
    s = uefi_call_wrapper(report->Root->Open, 5, report->Root, &file,
                          report->Path, EFI_FILE_MODE_CREATE | EFI_FILE_MODE_READ |
                          EFI_FILE_MODE_WRITE, EFI_FILE_ARCHIVE);
    if (EFI_ERROR(s) || !file) return EFI_ERROR(s) ? s : EFI_DEVICE_ERROR;
    s = dual_file_info_size(file, &file_size);
    if (EFI_ERROR(s) || file_size != 0) {
        close_s = uefi_call_wrapper(file->Close, 1, file);
        if (EFI_ERROR(s)) return s;
        return EFI_ERROR(close_s) ? close_s : EFI_ACCESS_DENIED;
    }
    if (!file->Write || !file->Flush || !file->Close) {
        (void)uefi_call_wrapper(file->Close, 1, file);
        return EFI_UNSUPPORTED;
    }
    report->File = file;
    report->Open = TRUE;
    {
        char detail[96];
        UINTN header_size = sizeof(DUAL_PLAN_HEADER) - 1u;
        s = dual_plan_write_exact(report, (const CHAR8 *)DUAL_PLAN_HEADER,
                                  header_size);
        if (!EFI_ERROR(s)) s = uefi_call_wrapper(report->File->Flush, 1, report->File);
        if (!EFI_ERROR(s)) {
            dual_plan_start_detail(report, detail, sizeof(detail));
            s = DualPlanReportRecord(report, "PLAN_START", EFI_SUCCESS, detail);
        }
    }
    if (EFI_ERROR(s)) {
        report->FirstError = s;
        (void)uefi_call_wrapper(report->File->Close, 1, report->File);
        report->File = NULL;
        report->Open = FALSE;
        return s;
    }
    return EFI_SUCCESS;
}

EFI_STATUS DualPlanReportRecord(DUAL_PLAN_REPORT *report, const char *stage,
                                EFI_STATUS status, const char *detail)
{
    CHAR8 line[DUAL_JOURNAL_MAX_LINE];
    static const CHAR8 hex[] = "0123456789abcdef";
    UINTN n = 0, stage_len, detail_len, i, k;
    EFI_STATUS s, flush_s;
    BOOLEAN is_start, is_result;
    UINT64 value = (UINT64)status;
    if (!report || !report->Open || !report->File) return EFI_NOT_READY;
    stage_len = dual_strlen(stage, DUAL_STAGE_CAP);
    detail_len = dual_strlen(detail, DUAL_JOURNAL_MAX_DETAIL + 1u);
    if (!dual_valid_stage(stage, stage_len) || detail_len > DUAL_JOURNAL_MAX_DETAIL) {
        s = EFI_INVALID_PARAMETER;
        if (!EFI_ERROR(report->FirstError)) report->FirstError = s;
        return s;
    }
    is_start = dual_stage_equal(stage, stage_len, "PLAN_START", sizeof("PLAN_START") - 1u);
    is_result = dual_stage_equal(stage, stage_len, "PLAN_RESULT", sizeof("PLAN_RESULT") - 1u);
    if ((is_start && report->StartWritten) ||
        (is_result &&
         (!report->Closing || report->ResultWritten))) {
        return EFI_ALREADY_STARTED;
    }
    if (report->BytesWritten + stage_len + 1u + 18u + 1u + detail_len + 1u >
        DUAL_JOURNAL_MAX_BYTES - DUAL_JOURNAL_RESERVED_FAILURE_BYTES) {
        report->Truncated = TRUE;
        report->FirstError = EFI_OUT_OF_RESOURCES;
        return EFI_OUT_OF_RESOURCES;
    }
    for (i = 0; i < stage_len; ++i) line[n++] = (CHAR8)stage[i];
    line[n++] = '\t'; line[n++] = '0'; line[n++] = 'x';
    for (i = 0; i < 16u; ++i)
        line[n++] = hex[(value >> ((15u - i) * 4u)) & 0x0fu];
    line[n++] = '\t';
    for (k = 0; k < detail_len; ++k) {
        UINT8 ch = (UINT8)detail[k];
        line[n++] = (ch >= 0x20u && ch <= 0x7eu && ch != '\t') ?
                    (CHAR8)ch : (CHAR8)' ';
    }
    line[n++] = '\n';
    s = dual_plan_write_exact(report, line, n);
    if (EFI_ERROR(s)) {
        report->FirstError = s;
        return s;
    }
    if (!report->File->Flush) {
        report->FirstError = EFI_UNSUPPORTED;
        return EFI_UNSUPPORTED;
    }
    flush_s = uefi_call_wrapper(report->File->Flush, 1, report->File);
    if (EFI_ERROR(flush_s)) report->FirstError = flush_s;
    if (!EFI_ERROR(flush_s)) {
        if (is_start) report->StartWritten = TRUE;
        if (is_result) report->ResultWritten = TRUE;
    }
    return flush_s;
}

EFI_STATUS DualPlanReportClose(DUAL_PLAN_REPORT *report, EFI_STATUS final_status,
                               const char *detail)
{
    EFI_STATUS first, s;
    if (!report) return EFI_INVALID_PARAMETER;
    if (!report->Open || !report->File) return EFI_ALREADY_STARTED;
    first = report->FirstError;
    if (report->Open && report->File) {
        report->Closing = TRUE;
        s = DualPlanReportRecord(report, "PLAN_RESULT", final_status, detail);
        if (EFI_ERROR(s) && !EFI_ERROR(first)) first = s;
        if (report->File->Flush) {
            s = uefi_call_wrapper(report->File->Flush, 1, report->File);
            if (EFI_ERROR(s) && !EFI_ERROR(first)) first = s;
        }
        s = uefi_call_wrapper(report->File->Close, 1, report->File);
        if (EFI_ERROR(s) && !EFI_ERROR(first)) first = s;
        report->File = NULL;
        report->Open = FALSE;
    }
    report->FirstError = first;
    return first;
}

static BOOLEAN dual_ascii_contains_nvgop(const CHAR8 *text)
{
    static const CHAR8 needle[] = "nvgop-tu1xx";
    UINTN i, j;
    if (!text) return FALSE;
    for (i = 0; text[i]; ++i) {
        for (j = 0; needle[j] && text[i+j]; ++j) {
            UINT8 a = (UINT8)text[i+j], b = (UINT8)needle[j];
            if (a >= 'A' && a <= 'Z') a = (UINT8)(a - 'A' + 'a');
            if (a != b) break;
        }
        if (!needle[j]) return TRUE;
    }
    return FALSE;
}

static EFI_STATUS dual_device_path_text(EFI_SYSTEM_TABLE *st,
                                        EFI_DEVICE_PATH_PROTOCOL *path,
                                        CHAR8 *out, UINTN cap)
{
    EFI_DEVICE_PATH_TO_TEXT_PROTOCOL *to_text = NULL;
    CHAR16 *text;
    EFI_STATUS s;
    UINTN i;
    if (!st->BootServices->LocateProtocol || !st->BootServices->FreePool)
        return EFI_UNSUPPORTED;
    s = uefi_call_wrapper(st->BootServices->LocateProtocol, 3,
                          &g_device_path_to_text_guid, NULL, (VOID **)&to_text);
    if (EFI_ERROR(s) || !to_text || !to_text->ConvertDevicePathToText)
        return EFI_ERROR(s) ? s : EFI_UNSUPPORTED;
    text = uefi_call_wrapper(to_text->ConvertDevicePathToText, 3, path, TRUE, TRUE);
    if (!text) return EFI_DEVICE_ERROR;
    for (i = 0; i + 1u < cap && text[i]; ++i)
        out[i] = (text[i] >= 0x20 && text[i] <= 0x7e) ? (CHAR8)text[i] : '?';
    out[i] = 0;
    s = uefi_call_wrapper(st->BootServices->FreePool, 1, text);
    return s;
}

static EFI_STATUS dual_driver_name_from_handle(EFI_SYSTEM_TABLE *st, EFI_HANDLE handle,
                                   CHAR8 *out, UINTN cap)
{
    EFI_COMPONENT_NAME2_PROTOCOL *name_proto = NULL;
    CHAR16 *name = NULL;
    CHAR8 language[24];
    EFI_STATUS s;
    UINTN n = 0, i;
    if (!st->BootServices->HandleProtocol) return EFI_UNSUPPORTED;
    s = uefi_call_wrapper(st->BootServices->HandleProtocol, 3, handle,
                          &g_component_name2_guid, (VOID **)&name_proto);
    if (EFI_ERROR(s) || !name_proto || !name_proto->GetDriverName ||
        !name_proto->SupportedLanguages) return EFI_ERROR(s) ? s : EFI_UNSUPPORTED;
    while (n + 1u < sizeof(language) && name_proto->SupportedLanguages[n] &&
           name_proto->SupportedLanguages[n] != ';') {
        language[n] = name_proto->SupportedLanguages[n]; ++n;
    }
    language[n] = 0;
    if (!n) return EFI_UNSUPPORTED;
    s = uefi_call_wrapper(name_proto->GetDriverName, 3, name_proto, language, &name);
    if (EFI_ERROR(s) || !name) return EFI_ERROR(s) ? s : EFI_UNSUPPORTED;
    for (i = 0; i + 1u < cap && name[i]; ++i)
        out[i] = (name[i] >= 0x20 && name[i] <= 0x7e) ? (CHAR8)name[i] : '?';
    out[i] = 0;
    return EFI_SUCCESS;
}

static BOOLEAN dual_name_is_nvidia(const CHAR8 *name)
{
    static const CHAR8 expected[] = "NVIDIA GPU UEFI Driver";
    UINTN i;
    for (i = 0; expected[i] && name[i]; ++i) {
        UINT8 a = name[i], b = expected[i];
        if (a >= 'a' && a <= 'z') a = (UINT8)(a - 'a' + 'A');
        if (b >= 'a' && b <= 'z') b = (UINT8)(b - 'a' + 'A');
        if (a != b) return FALSE;
    }
    return expected[i] == 0 && name[i] == 0;
}

static EFI_STATUS dual_is_nvgop_agent(EFI_SYSTEM_TABLE *st, EFI_HANDLE agent,
                                      BOOLEAN *matches)
{
    EFI_LOADED_IMAGE_PROTOCOL *loaded = NULL;
    EFI_DRIVER_BINDING_PROTOCOL *binding = NULL;
    CHAR8 path[256], name[128];
    EFI_STATUS path_status, name_status, s;
    EFI_HANDLE image_handle;
    *matches = FALSE;
    s = uefi_call_wrapper(st->BootServices->HandleProtocol, 3, agent,
                          &g_driver_binding_guid, (VOID **)&binding);
    if (EFI_ERROR(s) || !binding || !binding->ImageHandle ||
        binding->DriverBindingHandle != agent) return EFI_ACCESS_DENIED;
    image_handle = binding->ImageHandle;
    s = uefi_call_wrapper(st->BootServices->HandleProtocol, 3, image_handle,
                          &g_loaded_image_guid, (VOID **)&loaded);
    if (EFI_ERROR(s) || !loaded || !loaded->FilePath) {
        loaded = NULL;
        s = uefi_call_wrapper(st->BootServices->HandleProtocol, 3, agent,
                              &g_loaded_image_guid, (VOID **)&loaded);
        if (EFI_ERROR(s) || !loaded || !loaded->FilePath) loaded = NULL;
    }
    path[0] = 0; name[0] = 0;
    path_status = loaded ? dual_device_path_text(st, loaded->FilePath, path, sizeof(path)) : EFI_UNSUPPORTED;
    if (!EFI_ERROR(path_status)) {
        *matches = dual_ascii_contains_nvgop(path);
        return EFI_SUCCESS;
    }
    name_status = dual_driver_name_from_handle(st, agent, name, sizeof(name));
    if (EFI_ERROR(name_status))
        name_status = dual_driver_name_from_handle(st, image_handle, name, sizeof(name));
    if (!EFI_ERROR(name_status)) {
        *matches = dual_name_is_nvidia(name);
        return EFI_SUCCESS;
    }
    return EFI_ACCESS_DENIED;
}

static EFI_STATUS dual_verify_identity(EFI_SYSTEM_TABLE *st, EFI_HANDLE controller,
                                       UINTN *profile_index)
{
    EFI_PCI_IO_PROTOCOL *pci = NULL;
    UINTN seg, bus, dev, fn, i;
    UINT32 id = 0, subsystem = 0;
    EFI_STATUS s;
    s = uefi_call_wrapper(st->BootServices->HandleProtocol, 3, controller,
                          &g_pci_io_guid, (VOID **)&pci);
    if (EFI_ERROR(s) || !pci || !pci->GetLocation || !pci->Pci.Read) return EFI_UNSUPPORTED;
    s = uefi_call_wrapper(pci->GetLocation, 5, pci, &seg, &bus, &dev, &fn);
    if (EFI_ERROR(s)) return s;
    s = uefi_call_wrapper(pci->Pci.Read, 5, pci, EfiPciIoWidthUint32, 0, 1, &id);
    if (EFI_ERROR(s)) return s;
    s = uefi_call_wrapper(pci->Pci.Read, 5, pci, EfiPciIoWidthUint32, 0x2c, 1, &subsystem);
    if (EFI_ERROR(s)) return s;
    for (i = 0; i < 2; ++i) {
        const DUAL_PROFILE *p = &g_profiles[i];
        if (seg == p->Segment && bus == p->Bus && dev == p->Device && fn == p->Function &&
            (UINT16)id == p->Vendor && (UINT16)(id >> 16) == p->Product &&
            (UINT16)subsystem == p->SubVendor && (UINT16)(subsystem >> 16) == p->SubProduct) {
            *profile_index = i;
            return EFI_SUCCESS;
        }
    }
    return EFI_SECURITY_VIOLATION;
}

typedef struct {
    UINTN ByDriverCount;
    UINTN ByChildCount;
    UINTN NvgopByDriverCount;
    EFI_HANDLE NvgopAgent;
    BOOLEAN UnknownBinding;
} DUAL_BINDING_STATE;

static EFI_STATUS dual_binding_state(EFI_SYSTEM_TABLE *st, EFI_HANDLE controller,
                                     DUAL_BINDING_STATE *state)
{
    EFI_OPEN_PROTOCOL_INFORMATION_ENTRY *entries = NULL;
    UINTN count = 0, i;
    EFI_STATUS s, free_s;
    BOOLEAN nvgop;
    memset(state, 0, sizeof(*state));
    if (!st->BootServices->OpenProtocolInformation || !st->BootServices->FreePool)
        return EFI_UNSUPPORTED;
    s = uefi_call_wrapper(st->BootServices->OpenProtocolInformation, 4, controller,
                          &g_pci_io_guid, &entries, &count);
    if (EFI_ERROR(s)) return s;
    if ((count && !entries) || count > DUAL_MAX_OPEN_INFO) {
        if (entries) (void)uefi_call_wrapper(st->BootServices->FreePool, 1, entries);
        return count > DUAL_MAX_OPEN_INFO ? EFI_OUT_OF_RESOURCES : EFI_COMPROMISED_DATA;
    }
    for (i = 0; i < count; ++i) {
        if (!entries[i].OpenCount) continue;
        if (entries[i].Attributes & EFI_OPEN_PROTOCOL_BY_DRIVER) {
            ++state->ByDriverCount;
            s = dual_is_nvgop_agent(st, entries[i].AgentHandle, &nvgop);
            if (EFI_ERROR(s) || !nvgop) state->UnknownBinding = TRUE;
            else {
                ++state->NvgopByDriverCount;
                state->NvgopAgent = entries[i].AgentHandle;
            }
        }
        if (entries[i].Attributes & EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER) {
            ++state->ByChildCount;
            s = dual_is_nvgop_agent(st, entries[i].AgentHandle, &nvgop);
            if (EFI_ERROR(s) || !nvgop) state->UnknownBinding = TRUE;
        }
    }
    free_s = uefi_call_wrapper(st->BootServices->FreePool, 1, entries);
    if (EFI_ERROR(free_s)) return free_s;
    return EFI_SUCCESS;
}

EFI_STATUS DualFindTargetAgent(EFI_SYSTEM_TABLE *st, EFI_HANDLE controller,
                               EFI_HANDLE *agent)
{
    DUAL_BINDING_STATE state;
    UINTN profile_index;
    EFI_STATUS s;
    if (!st || !st->BootServices || !controller || !agent) return EFI_INVALID_PARAMETER;
    *agent = NULL;
    s = dual_verify_identity(st, controller, &profile_index);
    (void)profile_index;
    if (EFI_ERROR(s)) return s;
    s = dual_binding_state(st, controller, &state);
    if (EFI_ERROR(s)) return s;
    if (state.UnknownBinding || state.ByDriverCount != 1u ||
        state.NvgopByDriverCount != 1u || !state.NvgopAgent)
        return state.UnknownBinding || state.ByDriverCount ? EFI_ACCESS_DENIED : EFI_NOT_FOUND;
    *agent = state.NvgopAgent;
    return EFI_SUCCESS;
}

EFI_STATUS DualDetachTarget(EFI_SYSTEM_TABLE *st, EFI_HANDLE controller,
                            EFI_HANDLE expected_agent, DUAL_DETACH_RESULT *result)
{
    DUAL_BINDING_STATE before, after;
    UINTN profile_index;
    EFI_STATUS s, disconnect_s, after_s;
    if (!st || !st->BootServices || !controller || !expected_agent || !result)
        return EFI_INVALID_PARAMETER;
    memset(result, 0, sizeof(*result));
    result->BeforeStatus = result->DisconnectStatus = result->AfterStatus = EFI_NOT_READY;
    result->Agent = expected_agent;
    s = dual_verify_identity(st, controller, &profile_index);
    result->IdentityMatched = !EFI_ERROR(s);
    if (EFI_ERROR(s)) { result->BeforeStatus = s; return s; }
    s = dual_binding_state(st, controller, &before);
    result->BeforeStatus = s;
    result->BeforeByDriverCount = before.ByDriverCount;
    if (EFI_ERROR(s)) return s;
    if (before.UnknownBinding || before.ByDriverCount != 1u ||
        before.NvgopByDriverCount != 1u || before.NvgopAgent != expected_agent) {
        result->BeforeStatus = EFI_ACCESS_DENIED;
        return result->BeforeStatus;
    }
    result->TargetAgentMatched = TRUE;
    if (!st->BootServices->DisconnectController) {
        result->DisconnectStatus = EFI_UNSUPPORTED;
        return result->DisconnectStatus;
    }
    /* NULL ChildHandle deliberately allows this exact driver to stop and remove
     * its own children; DriverImageHandle is never NULL (never disconnect all). */
    disconnect_s = uefi_call_wrapper(st->BootServices->DisconnectController, 3,
                                     controller, expected_agent, NULL);
    result->DisconnectStatus = disconnect_s;
    after_s = dual_binding_state(st, controller, &after);
    result->AfterStatus = after_s;
    result->AfterByDriverCount = after.ByDriverCount;
    if (EFI_ERROR(after_s)) return after_s;
    s = dual_verify_identity(st, controller, &profile_index);
    result->IdentityMatched = result->IdentityMatched && !EFI_ERROR(s);
    if (EFI_ERROR(s)) { result->AfterStatus = s; return s; }
    if (after.UnknownBinding || after.ByDriverCount != 0u || after.ByChildCount != 0u) {
        result->AfterStatus = EFI_ACCESS_DENIED;
        return EFI_ERROR(disconnect_s) ? disconnect_s : result->AfterStatus;
    }
    if (EFI_ERROR(disconnect_s)) return disconnect_s;
    result->AfterStatus = EFI_SUCCESS;
    result->Detached = TRUE;
    return EFI_SUCCESS;
}

static EFI_STATUS dual_get_unbound_pci(EFI_SYSTEM_TABLE *st, EFI_HANDLE controller,
                                       EFI_STATUS *identity_status,
                                       EFI_STATUS *binding_status,
                                       EFI_PCI_IO_PROTOCOL **pci_out)
{
    DUAL_BINDING_STATE binding;
    UINTN profile_index;
    EFI_PCI_IO_PROTOCOL *pci = NULL;
    EFI_STATUS s;
    *pci_out = NULL;
    *identity_status = dual_verify_identity(st, controller, &profile_index);
    if (EFI_ERROR(*identity_status)) return *identity_status;
    s = uefi_call_wrapper(st->BootServices->HandleProtocol, 3, controller,
                          &g_pci_io_guid, (VOID **)&pci);
    if (EFI_ERROR(s) || !pci || !pci->Attributes || !pci->Pci.Read)
        return EFI_ERROR(s) ? s : EFI_UNSUPPORTED;
    *binding_status = dual_binding_state(st, controller, &binding);
    if (EFI_ERROR(*binding_status)) return *binding_status;
    if (binding.UnknownBinding || binding.ByDriverCount || binding.ByChildCount)
        return *binding_status = EFI_ACCESS_DENIED;
    *pci_out = pci;
    return EFI_SUCCESS;
}

static EFI_STATUS dual_read_command16(EFI_PCI_IO_PROTOCOL *pci, UINT16 *command)
{
    EFI_STATUS s;
    if (!pci || !pci->Pci.Read || !command) return EFI_INVALID_PARAMETER;
    *command = 0;
    s = uefi_call_wrapper(pci->Pci.Read, 5, pci, EfiPciIoWidthUint16,
                          DUAL_PCI_COMMAND_OFFSET, 1, command);
    return s;
}

EFI_STATUS DualPrepareMemoryAccess(EFI_SYSTEM_TABLE *st, EFI_HANDLE controller,
                                   DUAL_PCI_STATE *state)
{
    EFI_PCI_IO_PROTOCOL *pci = NULL;
    UINT64 current = 0, after = 0, supported = 0;
    BOOLEAN unexpected_bus_master;
    EFI_STATUS s;
    if (!st || !st->BootServices || !controller || !state)
        return EFI_INVALID_PARAMETER;
    memset(state, 0, sizeof(*state));
    state->IdentityStatus = state->BindingStatus = EFI_NOT_READY;
    state->SupportedStatus = state->OriginalGetStatus = EFI_NOT_READY;
    state->BeforeCommandStatus = EFI_NOT_READY;
    state->MemoryEnableStatus = state->MemoryGetStatus = EFI_NOT_READY;
    state->MemoryCommandStatus = EFI_NOT_READY;
    state->BusMasterSupportedStatus = state->BusMasterBeforeGetStatus = EFI_NOT_READY;
    state->BusMasterBeforeCommandStatus = EFI_NOT_READY;
    state->BusMasterEnableStatus = state->BusMasterGetStatus = EFI_NOT_READY;
    state->BusMasterCommandStatus = EFI_NOT_READY;

    s = dual_get_unbound_pci(st, controller, &state->IdentityStatus,
                             &state->BindingStatus, &pci);
    if (EFI_ERROR(s)) return s;
    state->BeforeCommand16 = 0;
    state->BeforeCommandStatus = dual_read_command16(pci, &state->BeforeCommand16);
    if (EFI_ERROR(state->BeforeCommandStatus)) return state->BeforeCommandStatus;
    if (state->BeforeCommand16 & DUAL_PCI_COMMAND_BUS_MASTER) {
        state->ResourceStateUncertain = TRUE;
        return EFI_ACCESS_DENIED;
    }

    state->SupportedStatus = uefi_call_wrapper(pci->Attributes, 4, pci,
        EfiPciIoAttributeOperationSupported, 0, &supported);
    state->Supported = supported;
    if (EFI_ERROR(state->SupportedStatus)) return state->SupportedStatus;
    if ((supported & DUAL_PCI_MEMORY_ATTRIBUTES) != DUAL_PCI_MEMORY_ATTRIBUTES)
        return EFI_UNSUPPORTED;

    state->OriginalGetStatus = uefi_call_wrapper(pci->Attributes, 4, pci,
        EfiPciIoAttributeOperationGet, 0, &current);
    state->OriginalAttributes = current;
    if (EFI_ERROR(state->OriginalGetStatus)) return state->OriginalGetStatus;
    if (current & EFI_PCI_IO_ATTRIBUTE_BUS_MASTER) {
        state->ResourceStateUncertain = TRUE;
        return EFI_ACCESS_DENIED;
    }
    state->MemoryEnableMask = DUAL_PCI_MEMORY_ATTRIBUTES & ~current;
    if (state->MemoryEnableMask) {
        state->MemoryEnableStatus = uefi_call_wrapper(pci->Attributes, 4, pci,
            EfiPciIoAttributeOperationEnable, state->MemoryEnableMask, NULL);
        if (EFI_ERROR(state->MemoryEnableStatus)) {
            state->ResourceStateUncertain = TRUE;
            return state->MemoryEnableStatus;
        }
    } else {
        state->MemoryEnableStatus = EFI_SUCCESS;
    }
    state->MemoryGetStatus = uefi_call_wrapper(pci->Attributes, 4, pci,
        EfiPciIoAttributeOperationGet, 0, &after);
    state->AfterMemoryAttributes = after;
    if (EFI_ERROR(state->MemoryGetStatus)) {
        if (state->MemoryEnableMask) state->ResourceStateUncertain = TRUE;
        return state->MemoryGetStatus;
    }
    unexpected_bus_master = (after & EFI_PCI_IO_ATTRIBUTE_BUS_MASTER) != 0;
    state->MemoryCommandStatus = dual_read_command16(pci, &state->AfterMemoryCommand16);
    if (EFI_ERROR(state->MemoryCommandStatus)) {
        state->ResourceStateUncertain = TRUE;
        return state->MemoryCommandStatus;
    }
    if (unexpected_bus_master ||
        (state->AfterMemoryCommand16 & DUAL_PCI_COMMAND_BUS_MASTER)) {
        state->ResourceStateUncertain = TRUE;
        return EFI_ACCESS_DENIED;
    }
    if ((after & DUAL_PCI_MEMORY_ATTRIBUTES) != DUAL_PCI_MEMORY_ATTRIBUTES) {
        state->ResourceStateUncertain = TRUE;
        return EFI_DEVICE_ERROR;
    }
    if (!(state->AfterMemoryCommand16 & DUAL_PCI_COMMAND_MEMORY)) {
        state->ResourceStateUncertain = TRUE;
        return EFI_DEVICE_ERROR;
    }
    state->MemoryPrepared = TRUE;
    return EFI_SUCCESS;
}

EFI_STATUS DualEnableBusMaster(EFI_SYSTEM_TABLE *st, EFI_HANDLE controller,
                               DUAL_PCI_STATE *state)
{
    EFI_PCI_IO_PROTOCOL *pci = NULL;
    UINT64 current = 0, supported = 0, after = 0;
    BOOLEAN attributes_match;
    EFI_STATUS s;
    if (!st || !st->BootServices || !controller || !state)
        return EFI_INVALID_PARAMETER;
    if (!state->MemoryPrepared) return EFI_NOT_READY;
    if (state->BusMasterEnabled) return EFI_ALREADY_STARTED;
    if (state->ResourceStateUncertain ||
        EFI_ERROR(state->IdentityStatus) || EFI_ERROR(state->BindingStatus) ||
        EFI_ERROR(state->BeforeCommandStatus) || EFI_ERROR(state->SupportedStatus) ||
        EFI_ERROR(state->OriginalGetStatus) || EFI_ERROR(state->MemoryEnableStatus) ||
        EFI_ERROR(state->MemoryGetStatus) || EFI_ERROR(state->MemoryCommandStatus) ||
        (state->Supported & DUAL_PCI_MEMORY_ATTRIBUTES) != DUAL_PCI_MEMORY_ATTRIBUTES ||
        (state->AfterMemoryAttributes & DUAL_PCI_MEMORY_ATTRIBUTES) !=
            DUAL_PCI_MEMORY_ATTRIBUTES ||
        !(state->AfterMemoryCommand16 & DUAL_PCI_COMMAND_MEMORY) ||
        (state->AfterMemoryCommand16 & DUAL_PCI_COMMAND_BUS_MASTER))
        return EFI_ACCESS_DENIED;
    s = dual_get_unbound_pci(st, controller, &state->IdentityStatus,
                             &state->BindingStatus, &pci);
    if (EFI_ERROR(s)) return s;
    state->BusMasterBeforeCommandStatus = dual_read_command16(pci,
                                                    &state->BeforeBusMasterCommand16);
    if (EFI_ERROR(state->BusMasterBeforeCommandStatus))
        return state->BusMasterBeforeCommandStatus;
    if (state->BeforeBusMasterCommand16 & DUAL_PCI_COMMAND_BUS_MASTER) {
        state->ResourceStateUncertain = TRUE;
        return EFI_ACCESS_DENIED;
    }
    if (!(state->BeforeBusMasterCommand16 & DUAL_PCI_COMMAND_MEMORY))
        return EFI_DEVICE_ERROR;

    state->BusMasterSupportedStatus = uefi_call_wrapper(pci->Attributes, 4, pci,
        EfiPciIoAttributeOperationSupported, 0, &supported);
    if (EFI_ERROR(state->BusMasterSupportedStatus)) return state->BusMasterSupportedStatus;
    state->Supported = supported;
    if ((supported & DUAL_PCI_MEMORY_ATTRIBUTES) != DUAL_PCI_MEMORY_ATTRIBUTES ||
        !(supported & EFI_PCI_IO_ATTRIBUTE_BUS_MASTER)) return EFI_UNSUPPORTED;
    state->BusMasterBeforeGetStatus = uefi_call_wrapper(pci->Attributes, 4, pci,
        EfiPciIoAttributeOperationGet, 0, &current);
    state->BeforeBusMasterAttributes = current;
    if (EFI_ERROR(state->BusMasterBeforeGetStatus)) return state->BusMasterBeforeGetStatus;
    if (current & EFI_PCI_IO_ATTRIBUTE_BUS_MASTER) {
        state->ResourceStateUncertain = TRUE;
        return EFI_ACCESS_DENIED;
    }
    if ((current & DUAL_PCI_MEMORY_ATTRIBUTES) != DUAL_PCI_MEMORY_ATTRIBUTES)
        return EFI_DEVICE_ERROR;
    state->BusMasterEnableMask = EFI_PCI_IO_ATTRIBUTE_BUS_MASTER & ~current;
    if (state->BusMasterEnableMask) {
        state->BusMasterEnableStatus = uefi_call_wrapper(pci->Attributes, 4, pci,
            EfiPciIoAttributeOperationEnable, state->BusMasterEnableMask, NULL);
        if (EFI_ERROR(state->BusMasterEnableStatus)) {
            state->ResourceStateUncertain = TRUE;
            return state->BusMasterEnableStatus;
        }
    } else {
        state->BusMasterEnableStatus = EFI_SUCCESS;
    }
    state->BusMasterGetStatus = uefi_call_wrapper(pci->Attributes, 4, pci,
        EfiPciIoAttributeOperationGet, 0, &after);
    state->AfterBusMasterAttributes = after;
    if (EFI_ERROR(state->BusMasterGetStatus)) {
        if (state->BusMasterEnableMask) state->ResourceStateUncertain = TRUE;
        return state->BusMasterGetStatus;
    }
    attributes_match =
        (after & (DUAL_PCI_MEMORY_ATTRIBUTES | EFI_PCI_IO_ATTRIBUTE_BUS_MASTER)) ==
        (DUAL_PCI_MEMORY_ATTRIBUTES | EFI_PCI_IO_ATTRIBUTE_BUS_MASTER);
    state->BusMasterCommandStatus = dual_read_command16(pci,
                                                       &state->AfterBusMasterCommand16);
    if (EFI_ERROR(state->BusMasterCommandStatus)) {
        state->ResourceStateUncertain = TRUE;
        return state->BusMasterCommandStatus;
    }
    if (!attributes_match || (state->AfterBusMasterCommand16 &
         (DUAL_PCI_COMMAND_MEMORY | DUAL_PCI_COMMAND_BUS_MASTER)) !=
        (DUAL_PCI_COMMAND_MEMORY | DUAL_PCI_COMMAND_BUS_MASTER))
    {
        state->ResourceStateUncertain = TRUE;
        return EFI_DEVICE_ERROR;
    }
    state->BusMasterEnabled = TRUE;
    return EFI_SUCCESS;
}

EFI_STATUS DualDisableBusMaster(EFI_SYSTEM_TABLE *st, EFI_HANDLE controller,
                                BOOLEAN quiescent_verified,
                                DUAL_BME_RELEASE_RESULT *result)
{
    EFI_PCI_IO_PROTOCOL *pci = NULL;
    UINT64 before_attributes = 0, after_attributes = 0;
    UINT16 before_command = 0, after_command = 0;
    EFI_STATUS s;
    BOOLEAN after_valid, state_matches;

    if (!result) return EFI_INVALID_PARAMETER;
    memset(result, 0, sizeof(*result));
    result->IdentityStatus = result->BindingStatus = EFI_NOT_READY;
    result->BeforeGetStatus = result->BeforeCommandStatus = EFI_NOT_READY;
    result->DisableStatus = result->AfterGetStatus = EFI_NOT_READY;
    result->AfterCommandStatus = result->FlushStatus = EFI_NOT_READY;
    result->QuiescentVerified = quiescent_verified;
    if (!st || !st->BootServices || !controller) return EFI_INVALID_PARAMETER;
    if (!quiescent_verified) return EFI_ACCESS_DENIED;

    s = dual_get_unbound_pci(st, controller, &result->IdentityStatus,
                             &result->BindingStatus, &pci);
    if (EFI_ERROR(s)) return s;
    if (!pci->Flush) return result->FlushStatus = EFI_UNSUPPORTED;

    result->BeforeGetStatus = uefi_call_wrapper(pci->Attributes, 4, pci,
        EfiPciIoAttributeOperationGet, 0, &before_attributes);
    result->BeforeAttributes = before_attributes;
    result->BeforeCommandStatus = dual_read_command16(pci, &before_command);
    result->BeforeCommand16 = before_command;
    if (EFI_ERROR(result->BeforeGetStatus)) return result->BeforeGetStatus;
    if (EFI_ERROR(result->BeforeCommandStatus)) return result->BeforeCommandStatus;

    /* Releasing bus mastering is only valid while memory decoding remains
     * active. Do not attempt a partial PCI state repair here. */
    if ((before_attributes & DUAL_PCI_MEMORY_ATTRIBUTES) != DUAL_PCI_MEMORY_ATTRIBUTES ||
        !(before_command & DUAL_PCI_COMMAND_MEMORY))
        return EFI_DEVICE_ERROR;

    if ((before_attributes & EFI_PCI_IO_ATTRIBUTE_BUS_MASTER) ||
        (before_command & DUAL_PCI_COMMAND_BUS_MASTER)) {
        result->DisableAttempted = TRUE;
        result->DisableStatus = uefi_call_wrapper(pci->Attributes, 4, pci,
            EfiPciIoAttributeOperationDisable,
            EFI_PCI_IO_ATTRIBUTE_BUS_MASTER, NULL);
    } else {
        result->DisableStatus = EFI_SUCCESS;
    }

    /* Gather both readbacks and flush even if Disable reported an error so
     * the caller has an auditable final state. A good readback never converts
     * a failed Disable operation into success. */
    result->AfterGetStatus = uefi_call_wrapper(pci->Attributes, 4, pci,
        EfiPciIoAttributeOperationGet, 0, &after_attributes);
    result->AfterAttributes = after_attributes;
    result->AfterCommandStatus = dual_read_command16(pci, &after_command);
    result->AfterCommand16 = after_command;
    result->FlushStatus = uefi_call_wrapper(pci->Flush, 1, pci);

    after_valid = !EFI_ERROR(result->AfterGetStatus) &&
                  !EFI_ERROR(result->AfterCommandStatus);
    state_matches = after_valid &&
        !(after_attributes & EFI_PCI_IO_ATTRIBUTE_BUS_MASTER) &&
        !(after_command & DUAL_PCI_COMMAND_BUS_MASTER) &&
        (after_attributes & DUAL_PCI_MEMORY_ATTRIBUTES) == DUAL_PCI_MEMORY_ATTRIBUTES &&
        (after_command & DUAL_PCI_COMMAND_MEMORY) &&
        ((after_attributes & ~EFI_PCI_IO_ATTRIBUTE_BUS_MASTER) ==
         (before_attributes & ~EFI_PCI_IO_ATTRIBUTE_BUS_MASTER)) &&
        ((after_command & (UINT16)~DUAL_PCI_COMMAND_BUS_MASTER) ==
         (before_command & (UINT16)~DUAL_PCI_COMMAND_BUS_MASTER));

    if (EFI_ERROR(result->DisableStatus)) return result->DisableStatus;
    if (EFI_ERROR(result->AfterGetStatus)) return result->AfterGetStatus;
    if (EFI_ERROR(result->AfterCommandStatus)) return result->AfterCommandStatus;
    if (!state_matches) return EFI_DEVICE_ERROR;
    if (EFI_ERROR(result->FlushStatus)) return result->FlushStatus;

    result->BusMasterDisabled = TRUE;
    return EFI_SUCCESS;
}


EFI_STATUS DualRestoreBusMaster(EFI_SYSTEM_TABLE *st, EFI_HANDLE controller)

{

    EFI_PCI_IO_PROTOCOL *pci = NULL;

    EFI_STATUS s;

    if (!st || !st->BootServices || !controller) return EFI_INVALID_PARAMETER;

    s = uefi_call_wrapper(st->BootServices->HandleProtocol, 3,

                          controller, &g_pci_io_guid, (void **)&pci);

    if (EFI_ERROR(s) || !pci) return s;

    s = uefi_call_wrapper(pci->Attributes, 4, pci,

                          EfiPciIoAttributeOperationEnable,

                          EFI_PCI_IO_ATTRIBUTE_BUS_MASTER, NULL);

    if (pci->Flush) (void)uefi_call_wrapper(pci->Flush, 1, pci);

    return s;

}

