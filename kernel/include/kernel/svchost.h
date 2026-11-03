/* The service host: one task that runs small periodic jobs, so each does not need a task and a
 * stack of its own (kernel/svchost.c).
 *
 * A job is a step function called every period_ms from the host's task. A step must return: it
 * may block briefly (an I2C transfer, a flash rewrite), and every other job waits that long, so
 * nothing that waits on the network, a user or the card belongs here. Its deepest path must fit
 * the host's stack, SVC_STACK_PAGES. The watchdog's feed is a job too, so a step that never
 * returns is caught by the watchdog like any hung task.
 *
 * Ownership: the job and its state belong to its module. svc_register() keeps the pointer; the
 * job must be static and is never unregistered. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Two pages: the deepest step is the setup button's owner reset, an identity rewrite with the
 * flash write under it (4900 bytes measured on an ESP32-P4). */
#define SVC_STACK_PAGES 2
#define SVC_MAX_JOBS    6

typedef struct svc_job {
    const char *name;
    uint32_t period_ms;
    void (*init)(void);              /* on the host's task, before the first step; may be NULL */
    void (*step)(uint64_t now_ms);
    /* The host's own, single task: the host. */
    uint64_t next_ms;
    uint32_t runs;
    uint32_t max_us;                 /* the longest step so far */
    bool started;
} svc_job_t;

/* Adds a job; starts the host on the first. False if the table is full or the host could not
 * be started: the caller then has no job running and says so. Any task, any time. */
bool svc_register(svc_job_t *job);

/* `svc`: the jobs, their periods and their longest steps. */
void svc_report(void);
