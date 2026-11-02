/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef TU102_DUAL_ATTEMPT_SUPPORT_H
#define TU102_DUAL_ATTEMPT_SUPPORT_H

#include <efi.h>
#include <efipciio.h>
#include <efiprot.h>

#define DUAL_JOURNAL_MAX_BYTES (128u * 1024u)
#define DUAL_JOURNAL_RESERVED_FAILURE_BYTES 1024u
#define DUAL_JOURNAL_MAX_SESSION_NUMBER 9999u
#define DUAL_JOURNAL_MAX_DETAIL 1023u
#define DUAL_JOURNAL_MAX_LINE 1200u

typedef struct {
    EFI_HANDLE Volume;
    EFI_FILE_PROTOCOL *Root;
    EFI_FILE_PROTOCOL *File;
    CHAR16 Path[40];
    UINTN SessionNumber;
    UINTN BytesWritten;
    EFI_STATUS FirstError;
    BOOLEAN Open;
    BOOLEAN Truncated;
} DUAL_JOURNAL;

/* One bounded allocator/resource-probe report belonging to a parent session.
 * Root is borrowed from the parent journal and is never closed here. */
typedef struct {
    EFI_FILE_PROTOCOL *Root;
    EFI_FILE_PROTOCOL *File;
    CHAR16 Path[64];
    CHAR8 PlanId[5];
    UINTN SessionNumber;
    UINTN BytesWritten;
    EFI_STATUS FirstError;
    BOOLEAN Open;
    BOOLEAN Truncated;
    BOOLEAN StartWritten;
    BOOLEAN Closing;
    BOOLEAN ResultWritten;
} DUAL_PLAN_REPORT;

typedef struct {
    EFI_STATUS BeforeStatus;
    EFI_STATUS DisconnectStatus;
    EFI_STATUS AfterStatus;
    UINTN BeforeByDriverCount;
    UINTN AfterByDriverCount;
    EFI_HANDLE Agent;
    BOOLEAN IdentityMatched;
    BOOLEAN TargetAgentMatched;
    BOOLEAN Detached;
} DUAL_DETACH_RESULT;

typedef struct {
    UINT64 OriginalAttributes;
    UINT64 Supported;
    UINT64 AfterMemoryAttributes;
    UINT64 BeforeBusMasterAttributes;
    UINT64 AfterBusMasterAttributes;
    UINT64 MemoryEnableMask;
    UINT64 BusMasterEnableMask;
    UINT16 BeforeCommand16;
    UINT16 AfterMemoryCommand16;
    UINT16 BeforeBusMasterCommand16;
    UINT16 AfterBusMasterCommand16;
    EFI_STATUS BeforeCommandStatus;
    EFI_STATUS IdentityStatus;
    EFI_STATUS BindingStatus;
    EFI_STATUS SupportedStatus;
    EFI_STATUS OriginalGetStatus;
    EFI_STATUS MemoryEnableStatus;
    EFI_STATUS MemoryGetStatus;
    EFI_STATUS MemoryCommandStatus;
    EFI_STATUS BusMasterSupportedStatus;
    EFI_STATUS BusMasterBeforeGetStatus;
    EFI_STATUS BusMasterBeforeCommandStatus;
    EFI_STATUS BusMasterEnableStatus;
    EFI_STATUS BusMasterGetStatus;
    EFI_STATUS BusMasterCommandStatus;
    BOOLEAN MemoryPrepared;
    BOOLEAN BusMasterEnabled;
    BOOLEAN ResourceStateUncertain;
} DUAL_PCI_STATE;

/* Release PCI bus mastering only after the caller has verified that the GPU
 * is quiescent. This helper never writes PCI config space and never changes
 * MEMORY or DUAL_ADDRESS_CYCLE attributes. On any error, callers must retain
 * DMA backing because the final hardware state is not certified. */
typedef struct {
    EFI_STATUS IdentityStatus;
    EFI_STATUS BindingStatus;
    EFI_STATUS BeforeGetStatus;
    UINT64 BeforeAttributes;
    EFI_STATUS BeforeCommandStatus;
    UINT16 BeforeCommand16;
    EFI_STATUS DisableStatus;
    EFI_STATUS AfterGetStatus;
    UINT64 AfterAttributes;
    EFI_STATUS AfterCommandStatus;
    UINT16 AfterCommand16;
    EFI_STATUS FlushStatus;
    BOOLEAN QuiescentVerified;
    BOOLEAN DisableAttempted;
    BOOLEAN BusMasterDisabled;
} DUAL_BME_RELEASE_RESULT;

/* Journal is plain, line-oriented text, independent of console output. */
EFI_STATUS DualJournalOpen(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable,
                           DUAL_JOURNAL *Journal);
EFI_STATUS DualJournalRecord(DUAL_JOURNAL *Journal, const char *Stage,
                             EFI_STATUS Status, const char *Detail);
EFI_STATUS DualJournalFlush(DUAL_JOURNAL *Journal);
EFI_STATUS DualJournalClose(DUAL_JOURNAL *Journal);

/* Create a non-overwriting RESOURCE_PLAN child log on the parent journal's
 * already validated USB volume. PlanId is exactly A/B/C/D/AUTO. Open writes
 * the header and unique PLAN_START; Close writes unique PLAN_RESULT then
 * flushes/closes. The returned Close status represents I/O failure only. */
EFI_STATUS DualPlanReportOpen(const DUAL_JOURNAL *Parent, const char *PlanId,
                              DUAL_PLAN_REPORT *Report);
EFI_STATUS DualPlanReportRecord(DUAL_PLAN_REPORT *Report, const char *Stage,
                                EFI_STATUS Status, const char *Detail);
EFI_STATUS DualPlanReportClose(DUAL_PLAN_REPORT *Report, EFI_STATUS FinalStatus,
                               const char *Detail);

/* Controller must identify exactly one of the two recorded TU102 boards.
 * Returns the sole matching NVIDIA nvgop BY_DRIVER image handle. */
EFI_STATUS DualFindTargetAgent(EFI_SYSTEM_TABLE *SystemTable,
                               EFI_HANDLE Controller, EFI_HANDLE *Agent);

/* Rechecks identity and the exact nvgop agent, disconnects only that agent,
 * then verifies that it no longer has an active BY_DRIVER open. Never prints,
 * reads console input, reconnects, or resets the machine. */
EFI_STATUS DualDetachTarget(EFI_SYSTEM_TABLE *SystemTable,
                            EFI_HANDLE Controller, EFI_HANDLE ExpectedAgent,
                            DUAL_DETACH_RESULT *Result);

/* Prepare PCI memory decoding and DAC, without enabling bus mastering. */
EFI_STATUS DualPrepareMemoryAccess(EFI_SYSTEM_TABLE *SystemTable,
                                   EFI_HANDLE Controller,
                                   DUAL_PCI_STATE *State);

/* Enable bus mastering only after the memory/DAC phase has succeeded. */
EFI_STATUS DualEnableBusMaster(EFI_SYSTEM_TABLE *SystemTable,
                               EFI_HANDLE Controller,
                               DUAL_PCI_STATE *State);

/* Disable PCI bus mastering after the caller proves the GPU quiescent. */
EFI_STATUS DualDisableBusMaster(EFI_SYSTEM_TABLE *SystemTable,
                                EFI_HANDLE Controller,
                                BOOLEAN QuiescentVerified,
                                DUAL_BME_RELEASE_RESULT *Result);

/* Restore PCI bus mastering before handing off to the operating system bootloader. */
EFI_STATUS DualRestoreBusMaster(EFI_SYSTEM_TABLE *SystemTable,
                                EFI_HANDLE Controller);

#endif
