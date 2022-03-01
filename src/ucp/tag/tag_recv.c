/**
 * Copyright (C) Mellanox Technologies Ltd. 2001-2015.  ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "eager.h"
#include "rndv.h"
#include "tag_match.inl"
#include "offload.h"

#include <ucp/core/ucp_worker.h>
#include <ucp/core/ucp_request.inl>
#include <ucs/datastruct/mpool.inl>
#include <ucs/datastruct/queue.h>


static UCS_F_ALWAYS_INLINE void
ucp_tag_recv_request_completed(ucp_worker_h worker, ucp_request_t *req,
                               void *buffer, ucs_status_t status,
                               ucp_tag_recv_info_t *info, const char *function)
{
    ucs_trace_req("%s returning completed request %p (%p) stag 0x%"PRIx64" len %zu, %s",
                  function, req, req + 1, info->sender_tag, info->length,
                  ucs_status_string(status));

    if (ucs_unlikely(worker->tm.rndv_debug.queue_length > 0)) {
        ucp_tag_rndv_debug_entry_t *entry =
                ucp_worker_rndv_debug_entry(worker, req->recv.req_id);
        entry->send_tag   = info->sender_tag;
        entry->status     = "recv_completed_unexp";
        entry->recvd_size = info->length;
        memcpy(entry->udata, buffer, ucs_min(UCP_TAG_MAX_DATA, info->length));
    }

    req->status = status;
    if ((req->flags |= UCP_REQUEST_FLAG_COMPLETED) & UCP_REQUEST_FLAG_RELEASED) {
        ucp_request_put(req);
    }
    UCS_PROFILE_REQUEST_EVENT(req, "complete_recv", 0);
}

static void
ucp_tag_recv_add_debug_entry(ucp_worker_h worker, void *buffer, size_t length,
                             ucp_tag_t tag, ucp_request_t *req)
{
    ucp_tag_rndv_debug_entry_t *entry = ucp_worker_rndv_debug_entry(worker,
                                                                    req->recv.req_id);
    entry->id             = req->recv.req_id;
    entry->type           = "tag_recv";
    entry->ep             = NULL;
    entry->local_address  = buffer;
    entry->size           = length;
    entry->rts_seq        = 0;
    entry->send_tag       = 0;
    entry->recv_tag       = tag;
    entry->remote_address = 0;
    entry->remote_reqptr  = 0;
    entry->rndv_get_req   = NULL;
    entry->recv_req       = req;
    entry->send_req       = NULL;
}

static UCS_F_ALWAYS_INLINE ucs_status_ptr_t
ucp_tag_recv_common(ucp_worker_h worker, void *buffer, size_t count,
                    uintptr_t datatype, ucp_tag_t tag, ucp_tag_t tag_mask,
                    ucp_request_t *req, const ucp_request_param_t *param,
                    ucp_recv_desc_t *rdesc, const char *debug_name)
{
    unsigned common_flags = UCP_REQUEST_FLAG_RECV | UCP_REQUEST_FLAG_EXPECTED;
    ucp_eager_first_hdr_t *eagerf_hdr;
    ucp_request_queue_t *req_queue;
    ucs_memory_type_t mem_type;
    size_t hdr_len, recv_len;
    ucs_status_t status;
    uint64_t msg_id;

    ucp_trace_req(req, "%s buffer %p dt 0x%lx count %zu tag %"PRIx64"/%"PRIx64,
                  debug_name, buffer, datatype, count, tag, tag_mask);

    /* set request id */
    req->recv.req_id = worker->rndv_req_id;
    worker->rndv_req_id++;

    if (ucs_unlikely(worker->tm.rndv_debug.queue_length > 0)) {
        ucp_tag_recv_add_debug_entry(worker, buffer,
                                     ucp_contig_dt_length(datatype, count),
                                     tag, req);
    }

    /* First, check the fast path case - single fragment
     * in this case avoid initializing most of request fields
     * */
    if (ucs_likely((rdesc != NULL) && (rdesc->flags & UCP_RECV_DESC_FLAG_EAGER_ONLY))) {
        UCS_PROFILE_REQUEST_EVENT(req, "eager_only_match", 0);
        UCP_WORKER_STAT_EAGER_MSG(worker, rdesc->flags);
        UCP_WORKER_STAT_EAGER_CHUNK(worker, UNEXP);

        if (ucs_unlikely(rdesc->flags & UCP_RECV_DESC_FLAG_EAGER_SYNC)) {
            ucp_tag_eager_sync_send_ack(worker, rdesc + 1, rdesc->flags);
        }

        req->recv.prev_flags          = req->flags;
        req->flags                    = UCP_REQUEST_FLAG_RECV;
        hdr_len                       = rdesc->payload_offset;
        recv_len                      = rdesc->length - hdr_len;
        req->recv.tag.info.sender_tag = ucp_rdesc_get_tag(rdesc);
        req->recv.tag.info.length     = recv_len;
        mem_type                      = ucp_memory_type_detect(worker->context,
                                                               buffer, recv_len);

        status = ucp_dt_unpack_only(worker, buffer, count, datatype, mem_type,
                                    UCS_PTR_BYTE_OFFSET(rdesc + 1, hdr_len),
                                    recv_len, 1);
        ucp_recv_desc_release(rdesc);

        if (param->op_attr_mask & UCP_OP_ATTR_FIELD_CALLBACK) {
            param->cb.recv(req + 1, status, &req->recv.tag.info,
                           ucp_request_param_user_data(param));
        }

        ucp_tag_recv_request_completed(worker, req, buffer, status,
                                       &req->recv.tag.info, debug_name);
        return req + 1;
    }

    /* Initialize receive request */
    req->status             = UCS_OK;
    req->recv.worker        = worker;
    req->recv.buffer        = buffer;
    req->recv.datatype      = datatype;

    ucp_dt_recv_state_init(&req->recv.state, buffer, datatype, count);

    if (!UCP_DT_IS_CONTIG(datatype)) {
        common_flags       |= UCP_REQUEST_FLAG_BLOCK_OFFLOAD;
    }

    if (param->op_attr_mask & UCP_OP_ATTR_FIELD_CALLBACK) {
        req->flags       = common_flags | UCP_REQUEST_FLAG_CALLBACK;
        req->user_data   = ucp_request_param_user_data(param);
        req->recv.tag.cb = param->cb.recv;
    } else {
        req->flags       = common_flags;
        req->user_data   = NULL;
        req->recv.tag.cb = NULL;
    }

    req->recv.length        = ucp_dt_length(datatype, count, buffer,
                                            &req->recv.state);
    req->recv.mem_type      = ucp_memory_type_detect(worker->context, buffer,
                                                     req->recv.length);
    req->recv.tag.tag       = tag;
    req->recv.tag.tag_mask  = tag_mask;
    req->recv.tag.rndv_req  = NULL;
    if (ucs_log_is_enabled(UCS_LOG_LEVEL_TRACE_REQ)) {
        req->recv.tag.info.sender_tag = 0;
    }

    status = ucp_recv_request_set_user_memh(req, param);
    if (status != UCS_OK) {
        ucp_request_put_param(param, req);
        return UCS_STATUS_PTR(status);
    }

    if (ucs_unlikely(rdesc == NULL)) {
        /* If not found on unexpected, wait until it arrives.
         * If was found but need this receive request for later completion, save it */
        req_queue = ucp_tag_exp_get_queue(&worker->tm, tag, tag_mask);

        /* If offload supported, post this tag to transport as well.
         * TODO: need to distinguish the cases when posting is not needed. */
        ucp_tag_offload_try_post(worker, req, req_queue);

        ucp_tag_exp_push(&worker->tm, req_queue, req);

        ucs_trace_req("%s returning expected request %p (%p)", debug_name, req,
                      req + 1);
        return req + 1;
    }

    /* Check rendezvous case */
    if (ucs_unlikely(rdesc->flags & UCP_RECV_DESC_FLAG_RNDV)) {
        ucp_rndv_matched(worker, req, (void*)(rdesc + 1), rdesc->rndv_rts_seq);
        UCP_WORKER_STAT_RNDV(worker, UNEXP);
        ucp_recv_desc_release(rdesc);
        return req + 1;
    }

    if (ucs_unlikely(rdesc->flags & UCP_RECV_DESC_FLAG_EAGER_SYNC)) {
        ucp_tag_eager_sync_send_ack(worker, rdesc + 1, rdesc->flags);
    }

    UCP_WORKER_STAT_EAGER_MSG(worker, rdesc->flags);
    ucs_assert(rdesc->flags & UCP_RECV_DESC_FLAG_EAGER);
    eagerf_hdr                    = (void*)(rdesc + 1);
    req->recv.tag.info.sender_tag = ucp_rdesc_get_tag(rdesc);
    req->recv.tag.info.length     =
    req->recv.tag.remaining       = eagerf_hdr->total_len;

    /* process first fragment */
    UCP_WORKER_STAT_EAGER_CHUNK(worker, UNEXP);
    msg_id = eagerf_hdr->msg_id;
    status = ucp_tag_recv_request_process_rdesc(req, rdesc, 0);
    ucs_assert((status == UCS_OK) || (status == UCS_INPROGRESS));

    /* process additional fragments */
    ucp_tag_frag_list_process_queue(&worker->tm, req, msg_id, eagerf_hdr->super.ep_ptr
                                    UCS_STATS_ARG(UCP_WORKER_STAT_TAG_RX_EAGER_CHUNK_UNEXP));

    return req + 1;
}

UCS_PROFILE_FUNC(ucs_status_t, ucp_tag_recv_nbr,
                 (worker, buffer, count, datatype, tag, tag_mask, request),
                 ucp_worker_h worker, void *buffer, size_t count,
                 uintptr_t datatype, ucp_tag_t tag, ucp_tag_t tag_mask,
                 void *request)
{
    ucp_request_param_t param = {
        .op_attr_mask = UCP_OP_ATTR_FIELD_DATATYPE |
                        UCP_OP_ATTR_FIELD_REQUEST  |
                        UCP_OP_ATTR_FLAG_NO_IMM_CMPL,
        .request      = request,
        .datatype     = datatype
    };
    ucs_status_ptr_t status;

    status = ucp_tag_recv_nbx(worker, buffer, count, tag, tag_mask, &param);
    return UCS_PTR_IS_ERR(status) ? UCS_PTR_STATUS(status) : UCS_OK;
}

UCS_PROFILE_FUNC(ucs_status_ptr_t, ucp_tag_recv_nb,
                 (worker, buffer, count, datatype, tag, tag_mask, cb),
                 ucp_worker_h worker, void *buffer, size_t count,
                 uintptr_t datatype, ucp_tag_t tag, ucp_tag_t tag_mask,
                 ucp_tag_recv_callback_t cb)
{
    ucp_request_param_t param = {
        .op_attr_mask = UCP_OP_ATTR_FIELD_DATATYPE |
                        UCP_OP_ATTR_FIELD_CALLBACK |
                        UCP_OP_ATTR_FLAG_NO_IMM_CMPL,
        .cb.recv      = (ucp_tag_recv_nbx_callback_t)cb,
        .datatype     = datatype
    };

    return ucp_tag_recv_nbx(worker, buffer, count, tag, tag_mask, &param);
}

UCS_PROFILE_FUNC(ucs_status_ptr_t, ucp_tag_recv_nbx,
                 (worker, buffer, count, tag, tag_mask, param),
                 ucp_worker_h worker, void *buffer, size_t count,
                 ucp_tag_t tag, ucp_tag_t tag_mask,
                 const ucp_request_param_t *param)
{
    ucp_recv_desc_t *rdesc;
    ucs_status_ptr_t ret;
    ucp_request_t *req;
    ucp_datatype_t datatype;

    UCP_CONTEXT_CHECK_FEATURE_FLAGS(worker->context, UCP_FEATURE_TAG,
                                    return UCS_STATUS_PTR(UCS_ERR_INVALID_PARAM));

    UCP_WORKER_THREAD_CS_ENTER_CONDITIONAL(worker);

    datatype = ucp_request_param_datatype(param);
    req      = ucp_request_get_param(worker, param, "tag_recv_nbx",
                                     {ret = UCS_STATUS_PTR(UCS_ERR_NO_MEMORY);
                                      goto out;});
    rdesc    = ucp_tag_unexp_search(&worker->tm, tag, tag_mask, 1, "recv_nbx");
    ret      = ucp_tag_recv_common(worker, buffer, count, datatype, tag,
                                   tag_mask, req, param, rdesc, "recv_nbx");

out:
    UCP_WORKER_THREAD_CS_EXIT_CONDITIONAL(worker);
    return ret;
}

UCS_PROFILE_FUNC(ucs_status_ptr_t, ucp_tag_msg_recv_nb,
                 (worker, buffer, count, datatype, message, cb),
                 ucp_worker_h worker, void *buffer, size_t count,
                 uintptr_t datatype, ucp_tag_message_h message,
                 ucp_tag_recv_callback_t cb)
{
    ucp_request_param_t param = {
        .op_attr_mask = UCP_OP_ATTR_FIELD_DATATYPE |
                        UCP_OP_ATTR_FIELD_CALLBACK |
                        UCP_OP_ATTR_FLAG_NO_IMM_CMPL,
        .cb.recv      = (ucp_tag_recv_nbx_callback_t)cb,
        .datatype     = datatype
    };
    ucp_recv_desc_t *rdesc = message;
    ucs_status_ptr_t ret;
    ucp_request_t *req;

    UCP_CONTEXT_CHECK_FEATURE_FLAGS(worker->context, UCP_FEATURE_TAG,
                                    return UCS_STATUS_PTR(UCS_ERR_INVALID_PARAM));
    UCP_WORKER_THREAD_CS_ENTER_CONDITIONAL(worker);

    req = ucp_request_get(worker, "tag_msg_recv_nb");
    if (ucs_likely(req != NULL)) {
        ret = ucp_tag_recv_common(worker, buffer, count, datatype,
                            ucp_rdesc_get_tag(rdesc), UCP_TAG_MASK_FULL, req,
                            &param, rdesc, "msg_recv_nb");
    } else {
        ret = UCS_STATUS_PTR(UCS_ERR_NO_MEMORY);
    }

    UCP_WORKER_THREAD_CS_EXIT_CONDITIONAL(worker);
    return ret;
}
