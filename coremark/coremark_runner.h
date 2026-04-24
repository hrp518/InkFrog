/*
 * coremark_runner.h - CoreMark Runner Interface
 *
 * Runs CoreMark benchmark directly (no separate thread).
 * Uses caller's stack - requires at least ~3KB free stack.
 */

#ifndef COREMARK_RUNNER_H
#define COREMARK_RUNNER_H

/**
 * Start CoreMark benchmark - blocks until complete (~10-20 seconds)
 * Runs in caller's thread context, no extra memory needed.
 * Returns 0 on success.
 */
int coremark_runner_start(void);

#endif /* COREMARK_RUNNER_H */