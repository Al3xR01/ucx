/**
 * SPDX-FileCopyrightText: NVIDIA CORPORATION & AFFILIATES
 * Copyright (c) 2019-2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * Copyright (C) Intel Corporation, 2023. ALL RIGHTS RESERVED.
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * See file LICENSE for terms.
 */


#ifndef UCS_MEMORY_TYPE_H_
#define UCS_MEMORY_TYPE_H_

#include <ucs/sys/compiler_def.h>

BEGIN_C_DECLS


/**
 * Memory types accessible from CPU
 */
#define UCS_MEMORY_TYPES_CPU_ACCESSIBLE \
    (UCS_BIT(UCS_MEMORY_TYPE_HOST) | UCS_BIT(UCS_MEMORY_TYPE_ROCM_MANAGED) | \
     UCS_BIT(UCS_MEMORY_TYPE_ZE_HOST) | UCS_BIT(UCS_MEMORY_TYPE_ZE_MANAGED))


/**
 * Enumerate all memory types
 */
#define ucs_memory_type_for_each(_x) \
    for ((_x) = UCS_MEMORY_TYPE_HOST; (_x) < UCS_MEMORY_TYPE_LAST; (_x)++)


/**
 * @ingroup UCS_RESOURCE
 * @brief Memory types
 *
 * List of supported memory types.
 */
typedef enum ucs_memory_type {
    UCS_MEMORY_TYPE_HOST,          /**< Default system memory */
    UCS_MEMORY_TYPE_CUDA,          /**< NVIDIA CUDA memory */
    UCS_MEMORY_TYPE_CUDA_MANAGED,  /**< NVIDIA CUDA managed (or unified) memory */
    UCS_MEMORY_TYPE_ROCM,          /**< AMD ROCM memory */
    UCS_MEMORY_TYPE_ROCM_MANAGED,  /**< AMD ROCM managed system memory */
    UCS_MEMORY_TYPE_RDMA,          /**< RDMA device memory */
    UCS_MEMORY_TYPE_ZE_HOST,       /**< Intel ZE memory (USM host) */
    UCS_MEMORY_TYPE_ZE_DEVICE,     /**< Intel ZE memory (USM device) */
    UCS_MEMORY_TYPE_ZE_MANAGED,    /**< Intel ZE managed memory (USM shared) */
    UCS_MEMORY_TYPE_LAST,
    UCS_MEMORY_TYPE_UNKNOWN = UCS_MEMORY_TYPE_LAST
} ucs_memory_type_t;


/**
 * Array of string names for each memory type
 */
extern const char *ucs_memory_type_names[];

/**
 * Array of string descriptions for each memory type
 */
extern const char *ucs_memory_type_descs[];


END_C_DECLS

#endif
