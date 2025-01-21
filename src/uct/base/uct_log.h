/**
 * SPDX-FileCopyrightText: NVIDIA CORPORATION & AFFILIATES
 * Copyright (c) 2001-2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
*
* See file LICENSE for terms.
*/

#ifndef UCT_LOG_H_
#define UCT_LOG_H_

#include "uct_iface.h"

#include <uct/api/uct.h>
#include <ucs/debug/log.h>


/**
 * In debug mode, print packet description to the log.
 */
#define uct_log_data(_file, _line, _function, _info) \
    ucs_log_dispatch(_file, _line, _function, UCS_LOG_LEVEL_TRACE_DATA,\
                     &ucs_global_opts.log_component, "%s", buf);


/**
 * Log callback which prints information about transport headers.
 */
typedef void (*uct_log_data_dump_func_t)(uct_base_iface_t *iface,
                                         uct_am_trace_type_t type, void *data,
                                         size_t length, size_t valid_length,
                                         char *buffer, size_t max);

#endif
