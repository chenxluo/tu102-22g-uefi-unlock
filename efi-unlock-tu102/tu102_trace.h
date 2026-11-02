/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef TU102_TRACE_H
#define TU102_TRACE_H

/*
 * Small, freestanding RAM trace primitives for the TU102 transaction path.
 * This header performs no EFI calls, hardware access, I/O, or allocation.
 * Callers own the trace storage and may format/flush it at safe boundaries.
 */

#include <stddef.h>
#include <stdint.h>

#define TU102_TRACE_CAPACITY 64u
#define TU102_TRACE_MICROSECONDS_PER_SECOND UINT64_C(1000000)

enum tu102_trace_op {
    TU102_TRACE_OP_READ = 1,
    TU102_TRACE_OP_WRITE = 2,
    TU102_TRACE_OP_FLUSH = 3,
    TU102_TRACE_OP_STALL = 4,
    TU102_TRACE_OP_POLL = 5,
    TU102_TRACE_OP_UPLOAD = 6
};

enum tu102_trace_time_state {
    TU102_TRACE_TIME_UNKNOWN = 0,
    TU102_TRACE_TIME_KNOWN = 1,
    TU102_TRACE_TIME_CLOCK_UNAVAILABLE = 2,
    TU102_TRACE_TIME_FREQUENCY_ZERO = 3,
    TU102_TRACE_TIME_FREQUENCY_CHANGED = 4,
    TU102_TRACE_TIME_COUNTER_WRAPPED = 5,
    TU102_TRACE_TIME_OVERFLOW = 6
};

/*
 * A tick source is only a caller-side integration contract. The trace helpers
 * never invoke it. Callers may sample a trusted clock around an operation and
 * pass both samples to tu102_trace_set_observed_elapsed().
 */
typedef int (*tu102_trace_read_ticks_fn)(void *context,
                                         uint64_t *ticks,
                                         uint64_t *frequency_hz);

struct tu102_trace_event {
    uint64_t op_idx;
    uint64_t offset;
    uint64_t value;
    uint64_t status; /* Raw EFI_STATUS bits, without interpretation. */
    uint64_t effective_status; /* Includes any local qualification failure. */

    uint64_t poll_limit;
    uint64_t poll_count;
    uint64_t last_value;
    uint64_t mask;
    uint64_t expected;

    uint64_t stall_requested_us;
    uint64_t observed_elapsed_us;

    enum tu102_trace_op operation;
    enum tu102_trace_time_state observed_elapsed_state;
    uint8_t poll_valid;
    uint8_t is_failure;
    uint8_t protocol_called;
};

struct tu102_trace {
    struct tu102_trace_event events[TU102_TRACE_CAPACITY];
    struct tu102_trace_event first_failure;
    uint64_t next_op_idx;
    uint64_t total_events;
    uint64_t overwritten_events;
    uint64_t requested_stall_us_total;
    size_t write_index;
    size_t count;
    uint8_t first_failure_valid;
    uint8_t ring_overwritten;
    uint8_t truncated;
    uint8_t op_idx_saturated;
    uint8_t event_count_saturated;
    uint8_t overwritten_count_saturated;
    uint8_t stall_total_overflow;
};

static inline void tu102_trace_init(struct tu102_trace *trace)
{
    if (trace == NULL) {
        return;
    }

    trace->first_failure = (struct tu102_trace_event){0};
    trace->next_op_idx = UINT64_C(1);
    trace->total_events = UINT64_C(0);
    trace->overwritten_events = UINT64_C(0);
    trace->requested_stall_us_total = UINT64_C(0);
    trace->write_index = 0u;
    trace->count = 0u;
    trace->first_failure_valid = 0u;
    trace->ring_overwritten = 0u;
    trace->truncated = 0u;
    trace->op_idx_saturated = 0u;
    trace->event_count_saturated = 0u;
    trace->overwritten_count_saturated = 0u;
    trace->stall_total_overflow = 0u;
}

static inline int tu102_trace_valid_operation(enum tu102_trace_op operation)
{
    return operation >= TU102_TRACE_OP_READ &&
           operation <= TU102_TRACE_OP_UPLOAD;
}

static inline void tu102_trace_saturating_increment(uint64_t *value,
                                                     uint8_t *saturated)
{
    if (*value == UINT64_MAX) {
        *saturated = 1u;
    } else {
        ++*value;
    }
}

/* Returns 1 when recorded, 0 for a null trace/event or invalid operation. */
static inline int tu102_trace_record(struct tu102_trace *trace,
                                     const struct tu102_trace_event *input)
{
    struct tu102_trace_event event;

    if (trace == NULL || input == NULL ||
        !tu102_trace_valid_operation(input->operation)) {
        return 0;
    }

    /* Copy first so a caller can safely record an event already in the ring. */
    event = *input;
    event.op_idx = trace->next_op_idx;
    if (trace->next_op_idx == UINT64_MAX) {
        trace->op_idx_saturated = 1u;
    } else {
        ++trace->next_op_idx;
    }

    if (event.operation == TU102_TRACE_OP_STALL) {
        if (UINT64_MAX - trace->requested_stall_us_total <
            event.stall_requested_us) {
            trace->requested_stall_us_total = UINT64_MAX;
            trace->stall_total_overflow = 1u;
        } else {
            trace->requested_stall_us_total += event.stall_requested_us;
        }
    }

    trace->events[trace->write_index] = event;
    if (trace->count < TU102_TRACE_CAPACITY) {
        ++trace->count;
    } else {
        trace->ring_overwritten = 1u;
        trace->truncated = 1u;
        tu102_trace_saturating_increment(&trace->overwritten_events,
                                         &trace->overwritten_count_saturated);
    }
    ++trace->write_index;
    if (trace->write_index == TU102_TRACE_CAPACITY) {
        trace->write_index = 0u;
    }
    tu102_trace_saturating_increment(&trace->total_events,
                                     &trace->event_count_saturated);

    if (event.is_failure != 0u && trace->first_failure_valid == 0u) {
        trace->first_failure = event;
        trace->first_failure_valid = 1u;
    }
    return 1;
}

/* Number of events retained, in chronological order, at a safe boundary. */
static inline size_t tu102_trace_count(const struct tu102_trace *trace)
{
    return trace == NULL ? 0u : trace->count;
}

/* Returns NULL for an invalid ordinal; otherwise returns oldest-first entry. */
static inline const struct tu102_trace_event *
tu102_trace_at_oldest(const struct tu102_trace *trace, size_t ordinal)
{
    size_t oldest;
    size_t index;

    if (trace == NULL || ordinal >= trace->count) {
        return NULL;
    }

    oldest = trace->write_index + TU102_TRACE_CAPACITY - trace->count;
    if (oldest >= TU102_TRACE_CAPACITY) {
        oldest -= TU102_TRACE_CAPACITY;
    }
    index = oldest + ordinal;
    if (index >= TU102_TRACE_CAPACITY) {
        index -= TU102_TRACE_CAPACITY;
    }
    return &trace->events[index];
}

/* Converts caller-supplied, same-source samples to microseconds. */
static inline enum tu102_trace_time_state
tu102_trace_elapsed_us(uint64_t start_ticks,
                       uint64_t end_ticks,
                       uint64_t start_frequency_hz,
                       uint64_t end_frequency_hz,
                       uint64_t *elapsed_us)
{
    uint64_t delta_ticks;
    uint64_t whole_seconds;
    uint64_t remaining_ticks;
    uint64_t whole_microseconds;
    uint64_t fractional_microseconds;

    if (elapsed_us != NULL) {
        *elapsed_us = UINT64_C(0);
    }
    if (start_frequency_hz == 0u || end_frequency_hz == 0u) {
        return TU102_TRACE_TIME_FREQUENCY_ZERO;
    }
    if (start_frequency_hz != end_frequency_hz) {
        return TU102_TRACE_TIME_FREQUENCY_CHANGED;
    }
    if (end_ticks < start_ticks) {
        return TU102_TRACE_TIME_COUNTER_WRAPPED;
    }
    if (elapsed_us == NULL) {
        return TU102_TRACE_TIME_UNKNOWN;
    }

    delta_ticks = end_ticks - start_ticks;
    whole_seconds = delta_ticks / start_frequency_hz;
    remaining_ticks = delta_ticks % start_frequency_hz;
    if (whole_seconds > UINT64_MAX / TU102_TRACE_MICROSECONDS_PER_SECOND ||
        remaining_ticks > UINT64_MAX / TU102_TRACE_MICROSECONDS_PER_SECOND) {
        return TU102_TRACE_TIME_OVERFLOW;
    }

    whole_microseconds =
        whole_seconds * TU102_TRACE_MICROSECONDS_PER_SECOND;
    fractional_microseconds =
        (remaining_ticks * TU102_TRACE_MICROSECONDS_PER_SECOND) /
        start_frequency_hz;
    if (UINT64_MAX - whole_microseconds < fractional_microseconds) {
        return TU102_TRACE_TIME_OVERFLOW;
    }

    *elapsed_us = whole_microseconds + fractional_microseconds;
    return TU102_TRACE_TIME_KNOWN;
}

static inline enum tu102_trace_time_state
tu102_trace_set_observed_elapsed(struct tu102_trace_event *event,
                                 int clock_samples_available,
                                 uint64_t start_ticks,
                                 uint64_t end_ticks,
                                 uint64_t start_frequency_hz,
                                 uint64_t end_frequency_hz)
{
    enum tu102_trace_time_state state;
    uint64_t elapsed_us = UINT64_C(0);

    if (event == NULL) {
        return TU102_TRACE_TIME_UNKNOWN;
    }
    if (clock_samples_available == 0) {
        state = TU102_TRACE_TIME_CLOCK_UNAVAILABLE;
    } else {
        state = tu102_trace_elapsed_us(start_ticks,
                                       end_ticks,
                                       start_frequency_hz,
                                       end_frequency_hz,
                                       &elapsed_us);
    }
    event->observed_elapsed_state = state;
    event->observed_elapsed_us =
        state == TU102_TRACE_TIME_KNOWN ? elapsed_us : UINT64_C(0);
    return state;
}

#endif /* TU102_TRACE_H */
