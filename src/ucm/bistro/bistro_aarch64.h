/**
 * SPDX-FileCopyrightText: NVIDIA CORPORATION & AFFILIATES
 * Copyright (c) 2018-2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * See file LICENSE for terms.
 */


#ifndef UCM_BISTRO_BISTRO_AARCH64_H_
#define UCM_BISTRO_BISTRO_AARCH64_H_

#include <stdint.h>

#include <ucs/type/status.h>
#include <ucs/sys/compiler_def.h>

#define UCM_BISTRO_PROLOGUE
#define UCM_BISTRO_EPILOGUE

/**
 * Set library function call hook using Binary Instrumentation
 * method (BISTRO): replace function body by user defined call
 *
 * @param func_ptr     Pointer to function to patch.
 * @param hook         User-defined function-replacer.
 * @param symbol       Function name to replace.
 * @param orig_func_p  Unsupported on this architecture and must be NULL.
 *                     If set to a non-NULL value, this function returns
 *                     @ref UCS_ERR_UNSUPPORTED.
 * @param rp           Restore point used to restore original function.
 *                     Optional, may be NULL.
 *
 * @return Error code as defined by @ref ucs_status_t
 */
ucs_status_t ucm_bistro_patch(void *func_ptr, void *hook, const char *symbol,
                              void **orig_func_p,
                              ucm_bistro_restore_point_t **rp);

/* Instruction type */
typedef uint32_t ucm_bistro_inst_t;

/* Lock implementation */
typedef struct {
    ucm_bistro_inst_t b; /* either self branch, or no-op branch */
} UCS_S_PACKED ucm_bistro_lock_t;

/**
 * Helper functions to improve atomicity of function patching
 */
void ucm_bistro_patch_lock(void *dst);

#endif
