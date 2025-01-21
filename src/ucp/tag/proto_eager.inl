/**
 * SPDX-FileCopyrightText: NVIDIA CORPORATION & AFFILIATES
 * Copyright (c) 2021-2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * See file LICENSE for terms.
 */

#ifndef PROTO_EAGER_INL_
#define PROTO_EAGER_INL_

#include "eager.h"

#include <ucp/proto/proto_common.inl>


static UCS_F_ALWAYS_INLINE void
ucp_proto_eager_sync_send_completed_common(ucp_request_t *req)
{
    UCP_EP_STAT_TAG_OP(req->send.ep, EAGER_SYNC);
    req->flags |= UCP_REQUEST_FLAG_SYNC_LOCAL_COMPLETED;
    if (req->flags & UCP_REQUEST_FLAG_SYNC_REMOTE_COMPLETED) {
        ucp_request_complete_send(req, UCS_OK);
    }
}

static UCS_F_ALWAYS_INLINE ucs_status_t
ucp_proto_eager_sync_bcopy_send_completed(ucp_request_t *req)
{
    ucp_datatype_iter_cleanup(&req->send.state.dt_iter, 0, UCP_DT_MASK_ALL);
    ucp_proto_eager_sync_send_completed_common(req);
    return UCS_OK;
}

#endif
