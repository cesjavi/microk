#ifndef AI_HOOKS_H
#define AI_HOOKS_H

#include <stdint.h>

/* 
 * MicroK AI Observation Hooks
 * These structures allow an "AI Optimizer" server to monitor 
 * kernel performance and adjust scheduling/memory policies.
 */

struct kernel_metrics {
    uint64_t total_cycles;
    uint32_t context_switches;
    uint32_t page_faults;
    uint32_t ipc_messages_sent;
    uint32_t idle_time_percentage;
};

/* 
 * Exposed to the AI Server via a special read-only memory mapping 
 */
void ai_expose_metrics(struct kernel_metrics *metrics);

#endif
