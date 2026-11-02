/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef TU102_ATTEMPT_ALLOCATOR_H
#define TU102_ATTEMPT_ALLOCATOR_H

#include <efi.h>

#define TU102_ATTEMPT_PLAN_AUTO 0u
#define TU102_ATTEMPT_PLAN_A 1u
#define TU102_ATTEMPT_PLAN_B 2u
#define TU102_ATTEMPT_PLAN_C 3u
#define TU102_ATTEMPT_PLAN_D 4u
#define TU102_ATTEMPT_PLAN_COUNT 5u
#define TU102_ATTEMPT_PLAN_STEP_COUNT 4u

typedef enum {
    Tu102DmaAllocate = 1,
    Tu102DmaMap = 2
} TU102_DMA_PLAN_ACTION;

typedef struct {
    UINT8 Card;
    TU102_DMA_PLAN_ACTION Action;
} TU102_DMA_PLAN_STEP;

const char *tu102_attempt_plan_id(UINTN plan_value);
const char *tu102_attempt_plan_description(UINTN plan_value);
UINTN tu102_attempt_auto_candidate(UINTN candidate_index);
EFI_STATUS tu102_attempt_plan_step(UINTN plan_value, UINTN step_index,
                                   TU102_DMA_PLAN_STEP *step);
BOOLEAN tu102_attempt_plan_fallback_allowed(
    TU102_DMA_PLAN_ACTION failed_action, EFI_STATUS status,
    BOOLEAN map_succeeded, BOOLEAN output_state_uncertain,
    BOOLEAN cleanup_confirmed, BOOLEAN bus_master_attempted,
    BOOLEAN gpu_write_started);

#endif
