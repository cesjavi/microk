#include "ai_hooks.h"

static struct kernel_metrics *exposed_metrics;

void ai_expose_metrics(struct kernel_metrics *metrics) {
    exposed_metrics = metrics;
    (void)exposed_metrics;
}
