/* SPDX-License-Identifier: GPL-2.0-only */
#include "attempt_allocator.h"

static const TU102_DMA_PLAN_STEP gDmaPlans[4][TU102_ATTEMPT_PLAN_STEP_COUNT] = {
    { {0, Tu102DmaAllocate}, {0, Tu102DmaMap},
      {1, Tu102DmaAllocate}, {1, Tu102DmaMap} },
    { {1, Tu102DmaAllocate}, {1, Tu102DmaMap},
      {0, Tu102DmaAllocate}, {0, Tu102DmaMap} },
    { {0, Tu102DmaAllocate}, {1, Tu102DmaAllocate},
      {0, Tu102DmaMap}, {1, Tu102DmaMap} },
    { {1, Tu102DmaAllocate}, {0, Tu102DmaAllocate},
      {1, Tu102DmaMap}, {0, Tu102DmaMap} }
};

static const char *const gPlanIds[TU102_ATTEMPT_PLAN_COUNT] = {
    "AUTO", "A", "B", "C", "D"
};

static const char *const gPlanDescriptions[TU102_ATTEMPT_PLAN_COUNT] = {
    "auto A-to-D until one clean allocator candidate is selected",
    "allocate0-map0-allocate1-map1",
    "allocate1-map1-allocate0-map0",
    "allocate0-allocate1-map0-map1",
    "allocate1-allocate0-map1-map0"
};

const char *tu102_attempt_plan_id(UINTN plan_value)
{
    if (plan_value >= TU102_ATTEMPT_PLAN_COUNT) return NULL;
    return gPlanIds[plan_value];
}

const char *tu102_attempt_plan_description(UINTN plan_value)
{
    if (plan_value >= TU102_ATTEMPT_PLAN_COUNT) return NULL;
    return gPlanDescriptions[plan_value];
}

UINTN tu102_attempt_auto_candidate(UINTN candidate_index)
{
    if (candidate_index >= 4u) return 0;
    return candidate_index + 1u;
}

EFI_STATUS tu102_attempt_plan_step(UINTN plan_value, UINTN step_index,
                                   TU102_DMA_PLAN_STEP *step)
{
    if (!step || plan_value < TU102_ATTEMPT_PLAN_A ||
        plan_value > TU102_ATTEMPT_PLAN_D ||
        step_index >= TU102_ATTEMPT_PLAN_STEP_COUNT)
        return EFI_INVALID_PARAMETER;
    *step = gDmaPlans[plan_value - 1u][step_index];
    return EFI_SUCCESS;
}

BOOLEAN tu102_attempt_plan_fallback_allowed(
    TU102_DMA_PLAN_ACTION failed_action, EFI_STATUS status,
    BOOLEAN map_succeeded, BOOLEAN output_state_uncertain,
    BOOLEAN cleanup_confirmed, BOOLEAN bus_master_attempted,
    BOOLEAN gpu_write_started)
{
    if (output_state_uncertain || !cleanup_confirmed ||
        bus_master_attempted || gpu_write_started)
        return FALSE;
    if (failed_action == Tu102DmaAllocate)
        return status == EFI_OUT_OF_RESOURCES;
    /* A Map error is never a retry condition. UEFI specifies output mapping
     * values only on success; even a NULL error token cannot prove no mapping
     * was created. EFI_UNSUPPORTED here is the local successful-Map geometry
     * rejection, and is eligible only after exact Unmap+Free confirmation. */
    return failed_action == Tu102DmaMap && map_succeeded &&
           status == EFI_UNSUPPORTED;
}
