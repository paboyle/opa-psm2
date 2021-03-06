/*

  This file is provided under a dual BSD/GPLv2 license.  When using or
  redistributing this file, you may do so under either license.

  GPL LICENSE SUMMARY

  Copyright(c) 2015 Intel Corporation.

  This program is free software; you can redistribute it and/or modify
  it under the terms of version 2 of the GNU General Public License as
  published by the Free Software Foundation.

  This program is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  General Public License for more details.

  Contact Information:
  Intel Corporation, www.intel.com

  BSD LICENSE

  Copyright(c) 2015 Intel Corporation.

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions
  are met:

    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in
      the documentation and/or other materials provided with the
      distribution.
    * Neither the name of Intel Corporation nor the names of its
      contributors may be used to endorse or promote products derived
      from this software without specific prior written permission.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
  A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
  OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
  LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
  DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
  THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

*/

/* Copyright (c) 2003-2015 Intel Corporation. All rights reserved. */

#include "psm_user.h"
#include "psm_mq_internal.h"
#include "psm_am_internal.h"
#include "cmarw.h"

static
psm2_error_t
ptl_handle_rtsmatch_request(psm2_mq_req_t req, int was_posted,
			    amsh_am_token_t *tok)
{
	psm2_amarg_t args[5];
	psm2_epaddr_t epaddr = req->rts_peer;
	ptl_t *ptl = epaddr->ptlctl->ptl;
	int pid = 0;

	PSM2_LOG_MSG("entering.");
	psmi_assert((tok != NULL && was_posted)
		    || (tok == NULL && !was_posted));

	_HFI_VDBG("[shm][rndv][recv] req=%p dest=%p len=%d tok=%p\n",
		  req, req->buf, req->recv_msglen, tok);

	if ((ptl->psmi_kassist_mode & PSMI_KASSIST_GET)
	    && req->recv_msglen > 0
	    && (pid = psmi_epaddr_pid(epaddr))) {
		/* cma can be done in handler context or not. */
		size_t nbytes = cma_get(pid, (void *)req->rts_sbuf,
					req->buf, req->recv_msglen);
		psmi_assert_always(nbytes == req->recv_msglen);
	}

	args[0].u64w0 = (uint64_t) (uintptr_t) req->ptl_req_ptr;
	args[1].u64w0 = (uint64_t) (uintptr_t) req;
	args[2].u64w0 = (uint64_t) (uintptr_t) req->buf;
	args[3].u32w0 = req->recv_msglen;
	args[3].u32w1 = tok != NULL ? 1 : 0;
	args[4].u64w0 = 0;

	if (tok != NULL) {
		psmi_am_reqq_add(AMREQUEST_SHORT, tok->ptl,
				 tok->tok.epaddr_from, mq_handler_rtsmatch_hidx,
				 args, 5, NULL, 0, NULL, 0);
	} else
		psmi_amsh_short_request(ptl, epaddr, mq_handler_rtsmatch_hidx,
					args, 5, NULL, 0, 0);

	/* 0-byte completion or we used kassist */
	if (pid || req->recv_msglen == 0)
		psmi_mq_handle_rts_complete(req);
	PSM2_LOG_MSG("leaving.");
	return PSM2_OK;
}

static
psm2_error_t
ptl_handle_rtsmatch(psm2_mq_req_t req, int was_posted)
{
	/* was_posted == 0 allows us to assume that we're not running this callback
	 * within am handler context (i.e. we can poll) */
	psmi_assert(was_posted == 0);
	return ptl_handle_rtsmatch_request(req, 0, NULL);
}

void
psmi_am_mq_handler(void *toki, psm2_amarg_t *args, int narg, void *buf,
		   size_t len)
{
	amsh_am_token_t *tok = (amsh_am_token_t *) toki;
	psm2_mq_req_t req;
	psm2_mq_tag_t tag;
	int rc;
	uint32_t opcode = args[0].u32w0;
	uint32_t msglen = opcode <= MQ_MSG_SHORT ? len : args[0].u32w1;

	tag.tag[0] = args[1].u32w1;
	tag.tag[1] = args[1].u32w0;
	tag.tag[2] = args[2].u32w1;
	psmi_assert(toki != NULL);
	_HFI_VDBG("mq=%p opcode=%d, len=%d, msglen=%d\n",
		  tok->mq, opcode, (int)len, msglen);

	switch (opcode) {
	case MQ_MSG_TINY:
	case MQ_MSG_SHORT:
	case MQ_MSG_EAGER:
		rc = psmi_mq_handle_envelope(tok->mq, tok->tok.epaddr_from,
					     &tag, msglen, 0, buf,
					     (uint32_t) len, 1, opcode, &req);

		/* for eager matching */
		req->ptl_req_ptr = (void *)tok->tok.epaddr_from;
		req->msg_seqnum = 0;	/* using seqnum 0 */
		break;
	default:{
			void *sreq = (void *)(uintptr_t) args[3].u64w0;
			uintptr_t sbuf = (uintptr_t) args[4].u64w0;
			psmi_assert(narg == 5);
			psmi_assert_always(opcode == MQ_MSG_LONGRTS);
			rc = psmi_mq_handle_rts(tok->mq, tok->tok.epaddr_from,
						&tag, msglen, NULL, 0, 1,
						ptl_handle_rtsmatch, &req);

			req->rts_peer = tok->tok.epaddr_from;
			req->ptl_req_ptr = sreq;
			req->rts_sbuf = sbuf;

			if (rc == MQ_RET_MATCH_OK)	/* we are in handler context, issue a reply */
				ptl_handle_rtsmatch_request(req, 1, tok);
			/* else will be called later */
			break;
		}
	}
	return;
}

void
psmi_am_mq_handler_data(void *toki, psm2_amarg_t *args, int narg, void *buf,
			size_t len)
{
	amsh_am_token_t *tok = (amsh_am_token_t *) toki;

	psmi_assert(toki != NULL);

	psm2_epaddr_t epaddr = (psm2_epaddr_t) tok->tok.epaddr_from;
	psm2_mq_req_t req = mq_eager_match(tok->mq, epaddr, 0);	/* using seqnum 0 */
	psmi_assert_always(req != NULL);
	psmi_mq_handle_data(tok->mq, req, args[2].u32w0, buf, len);

	return;
}

void
psmi_am_mq_handler_rtsmatch(void *toki, psm2_amarg_t *args, int narg, void *buf,
			    size_t len)
{
	amsh_am_token_t *tok = (amsh_am_token_t *) toki;

	psmi_assert(toki != NULL);

	ptl_t *ptl = tok->ptl;
	psm2_mq_req_t sreq = (psm2_mq_req_t) (uintptr_t) args[0].u64w0;
	void *dest = (void *)(uintptr_t) args[2].u64w0;
	uint32_t msglen = args[3].u32w0;
	psm2_amarg_t rarg[1];

	_HFI_VDBG("[rndv][send] req=%p dest_req=%p src=%p dest=%p len=%d\n",
		  sreq, (void *)(uintptr_t) args[1].u64w0, sreq->buf, dest,
		  msglen);

	if (msglen > 0) {
		rarg[0].u64w0 = args[1].u64w0;	/* rreq */
		int kassist_mode = ptl->psmi_kassist_mode;

		if (kassist_mode & PSMI_KASSIST_PUT) {
			int pid = psmi_epaddr_pid(tok->tok.epaddr_from);

			size_t nbytes = cma_put(sreq->buf, pid, dest, msglen);
			psmi_assert_always(nbytes == msglen);

			/* Send response that PUT is complete */
			psmi_amsh_short_reply(tok, mq_handler_rtsdone_hidx,
					      rarg, 1, NULL, 0, 0);
		} else if (!(kassist_mode & PSMI_KASSIST_MASK)) {
			/* Only transfer if kassist is off, i.e. neither GET nor PUT. */
			psmi_amsh_long_reply(tok, mq_handler_rtsdone_hidx, rarg,
					     1, sreq->buf, msglen, dest, 0);
		}

	}
	psmi_mq_handle_rts_complete(sreq);
}

void
psmi_am_mq_handler_rtsdone(void *toki, psm2_amarg_t *args, int narg, void *buf,
			   size_t len)
{
	psm2_mq_req_t rreq = (psm2_mq_req_t) (uintptr_t) args[0].u64w0;
	psmi_assert(narg == 1);
	_HFI_VDBG("[rndv][recv] req=%p dest=%p len=%d\n", rreq, rreq->buf,
		  rreq->recv_msglen);
	psmi_mq_handle_rts_complete(rreq);
}

void
psmi_am_handler(void *toki, psm2_amarg_t *args, int narg, void *buf, size_t len)
{
	amsh_am_token_t *tok = (amsh_am_token_t *) toki;
	psm2_am_handler_fn_t hfn;

	psmi_assert(toki != NULL);

	hfn = psm_am_get_handler_function(tok->mq->ep,
					  (psm2_handler_t) args[0].u32w0);

	/* Invoke handler function. For AM we do not support break functionality */
	hfn(toki, args + 1, narg - 1, buf, len);

	return;
}
