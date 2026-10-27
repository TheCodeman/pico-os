/**
 * @file core1_job.h
 * @brief Work a program can give core 1 on top of USB and the sound (main.c).
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#pragma once

/// Core 0: core 1 calls fn each time round its loop until core1_job_stop. fn
/// must return soon (well under a millisecond): USB waits while it runs.
void core1_job_start(void (*fn)(void));
/// Core 0: stops calling it, and returns once core 1 is out of it.
void core1_job_stop(void);
/// The job core 1 is running now (NULL if none).
void (*core1_job_now(void))(void);
