/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation. All rights reserved.
 *   Copyright (c) 2019-2021 Mellanox Technologies LTD. All rights reserved.
 *   Copyright (c) 2022 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "spdk/stdinc.h"

#include "bdev_nvme.h"

#include "spdk/config.h"

#include "spdk/string.h"
#include "spdk/rpc.h"
#include "spdk/util.h"
#include "spdk/env.h"
#include "spdk/nvme.h"
#include "spdk/nvme_spec.h"

#include "spdk/log.h"
#include "spdk/bdev_module.h"

struct open_descriptors {
	void *desc;
	struct  spdk_bdev *bdev;
	TAILQ_ENTRY(open_descriptors) tqlst;
	struct spdk_thread *thread;
};
typedef TAILQ_HEAD(, open_descriptors) open_descriptors_t;

static int
rpc_decode_action_on_timeout(const struct spdk_json_val *val, void *out)
{
	enum spdk_bdev_timeout_action *action = out;

	if (spdk_json_strequal(val, "none") == true) {
		*action = SPDK_BDEV_NVME_TIMEOUT_ACTION_NONE;
	} else if (spdk_json_strequal(val, "abort") == true) {
		*action = SPDK_BDEV_NVME_TIMEOUT_ACTION_ABORT;
	} else if (spdk_json_strequal(val, "reset") == true) {
		*action = SPDK_BDEV_NVME_TIMEOUT_ACTION_RESET;
	} else {
		SPDK_NOTICELOG("Invalid parameter value: action_on_timeout\n");
		return -EINVAL;
	}

	return 0;
}

static const struct spdk_json_object_decoder rpc_bdev_nvme_options_decoders[] = {
	{"action_on_timeout", offsetof(struct spdk_bdev_nvme_opts, action_on_timeout), rpc_decode_action_on_timeout, true},
	{"timeout_us", offsetof(struct spdk_bdev_nvme_opts, timeout_us), spdk_json_decode_uint64, true},
	{"timeout_admin_us", offsetof(struct spdk_bdev_nvme_opts, timeout_admin_us), spdk_json_decode_uint64, true},
	{"keep_alive_timeout_ms", offsetof(struct spdk_bdev_nvme_opts, keep_alive_timeout_ms), spdk_json_decode_uint32, true},
	{"retry_count", offsetof(struct spdk_bdev_nvme_opts, transport_retry_count), spdk_json_decode_uint32, true},
	{"arbitration_burst", offsetof(struct spdk_bdev_nvme_opts, arbitration_burst), spdk_json_decode_uint32, true},
	{"low_priority_weight", offsetof(struct spdk_bdev_nvme_opts, low_priority_weight), spdk_json_decode_uint32, true},
	{"medium_priority_weight", offsetof(struct spdk_bdev_nvme_opts, medium_priority_weight), spdk_json_decode_uint32, true},
	{"high_priority_weight", offsetof(struct spdk_bdev_nvme_opts, high_priority_weight), spdk_json_decode_uint32, true},
	{"nvme_adminq_poll_period_us", offsetof(struct spdk_bdev_nvme_opts, nvme_adminq_poll_period_us), spdk_json_decode_uint64, true},
	{"nvme_ioq_poll_period_us", offsetof(struct spdk_bdev_nvme_opts, nvme_ioq_poll_period_us), spdk_json_decode_uint64, true},
	{"io_queue_requests", offsetof(struct spdk_bdev_nvme_opts, io_queue_requests), spdk_json_decode_uint32, true},
	{"delay_cmd_submit", offsetof(struct spdk_bdev_nvme_opts, delay_cmd_submit), spdk_json_decode_bool, true},
	{"transport_retry_count", offsetof(struct spdk_bdev_nvme_opts, transport_retry_count), spdk_json_decode_uint32, true},
	{"bdev_retry_count", offsetof(struct spdk_bdev_nvme_opts, bdev_retry_count), spdk_json_decode_int32, true},
	{"transport_ack_timeout", offsetof(struct spdk_bdev_nvme_opts, transport_ack_timeout), spdk_json_decode_uint8, true},
	{"ctrlr_loss_timeout_sec", offsetof(struct spdk_bdev_nvme_opts, ctrlr_loss_timeout_sec), spdk_json_decode_int32, true},
	{"reconnect_delay_sec", offsetof(struct spdk_bdev_nvme_opts, reconnect_delay_sec), spdk_json_decode_uint32, true},
	{"fast_io_fail_timeout_sec", offsetof(struct spdk_bdev_nvme_opts, fast_io_fail_timeout_sec), spdk_json_decode_uint32, true},
};

static void
rpc_bdev_nvme_set_options(struct spdk_jsonrpc_request *request,
			  const struct spdk_json_val *params)
{
	struct spdk_bdev_nvme_opts opts;
	int rc;

	bdev_nvme_get_opts(&opts);
	if (params && spdk_json_decode_object(params, rpc_bdev_nvme_options_decoders,
					      SPDK_COUNTOF(rpc_bdev_nvme_options_decoders),
					      &opts)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		return;
	}

	rc = bdev_nvme_set_opts(&opts);
	if (rc) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		return;
	}

	spdk_jsonrpc_send_bool_response(request, true);

	return;
}
SPDK_RPC_REGISTER("bdev_nvme_set_options", rpc_bdev_nvme_set_options,
		  SPDK_RPC_STARTUP | SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(bdev_nvme_set_options, set_bdev_nvme_options)

struct rpc_bdev_nvme_hotplug {
	bool enabled;
	uint64_t period_us;
};

static const struct spdk_json_object_decoder rpc_bdev_nvme_hotplug_decoders[] = {
	{"enable", offsetof(struct rpc_bdev_nvme_hotplug, enabled), spdk_json_decode_bool, false},
	{"period_us", offsetof(struct rpc_bdev_nvme_hotplug, period_us), spdk_json_decode_uint64, true},
};

static void
rpc_bdev_nvme_set_hotplug_done(void *ctx)
{
	struct spdk_jsonrpc_request *request = ctx;

	spdk_jsonrpc_send_bool_response(request, true);
}

static void
rpc_bdev_nvme_set_hotplug(struct spdk_jsonrpc_request *request,
			  const struct spdk_json_val *params)
{
	struct rpc_bdev_nvme_hotplug req = {false, 0};
	int rc;

	if (spdk_json_decode_object(params, rpc_bdev_nvme_hotplug_decoders,
				    SPDK_COUNTOF(rpc_bdev_nvme_hotplug_decoders), &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		rc = -EINVAL;
		goto invalid;
	}

	rc = bdev_nvme_set_hotplug(req.enabled, req.period_us, rpc_bdev_nvme_set_hotplug_done,
				   request);
	if (rc) {
		goto invalid;
	}

	return;
invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, spdk_strerror(-rc));
}
SPDK_RPC_REGISTER("bdev_nvme_set_hotplug", rpc_bdev_nvme_set_hotplug, SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(bdev_nvme_set_hotplug, set_bdev_nvme_hotplug)

struct rpc_bdev_nvme_attach_controller {
	char *name;
	char *trtype;
	char *adrfam;
	char *traddr;
	char *trsvcid;
	char *priority;
	char *subnqn;
	char *hostnqn;
	char *hostaddr;
	char *hostsvcid;
	char *multipath;
	struct nvme_ctrlr_opts bdev_opts;
	struct spdk_nvme_ctrlr_opts drv_opts;
};

static void
free_rpc_bdev_nvme_attach_controller(struct rpc_bdev_nvme_attach_controller *req)
{
	free(req->name);
	free(req->trtype);
	free(req->adrfam);
	free(req->traddr);
	free(req->trsvcid);
	free(req->priority);
	free(req->subnqn);
	free(req->hostnqn);
	free(req->hostaddr);
	free(req->hostsvcid);
	free(req->multipath);
}

static int
bdev_nvme_decode_reftag(const struct spdk_json_val *val, void *out)
{
	uint32_t *flag = out;
	bool reftag;
	int rc;

	rc = spdk_json_decode_bool(val, &reftag);
	if (rc == 0 && reftag == true) {
		*flag |= SPDK_NVME_IO_FLAGS_PRCHK_REFTAG;
	}

	return rc;
}

static int
bdev_nvme_decode_guard(const struct spdk_json_val *val, void *out)
{
	uint32_t *flag = out;
	bool guard;
	int rc;

	rc = spdk_json_decode_bool(val, &guard);
	if (rc == 0 && guard == true) {
		*flag |= SPDK_NVME_IO_FLAGS_PRCHK_GUARD;
	}

	return rc;
}

static const struct spdk_json_object_decoder rpc_bdev_nvme_attach_controller_decoders[] = {
	{"name", offsetof(struct rpc_bdev_nvme_attach_controller, name), spdk_json_decode_string},
	{"trtype", offsetof(struct rpc_bdev_nvme_attach_controller, trtype), spdk_json_decode_string},
	{"traddr", offsetof(struct rpc_bdev_nvme_attach_controller, traddr), spdk_json_decode_string},

	{"adrfam", offsetof(struct rpc_bdev_nvme_attach_controller, adrfam), spdk_json_decode_string, true},
	{"trsvcid", offsetof(struct rpc_bdev_nvme_attach_controller, trsvcid), spdk_json_decode_string, true},
	{"priority", offsetof(struct rpc_bdev_nvme_attach_controller, priority), spdk_json_decode_string, true},
	{"subnqn", offsetof(struct rpc_bdev_nvme_attach_controller, subnqn), spdk_json_decode_string, true},
	{"hostnqn", offsetof(struct rpc_bdev_nvme_attach_controller, hostnqn), spdk_json_decode_string, true},
	{"hostaddr", offsetof(struct rpc_bdev_nvme_attach_controller, hostaddr), spdk_json_decode_string, true},
	{"hostsvcid", offsetof(struct rpc_bdev_nvme_attach_controller, hostsvcid), spdk_json_decode_string, true},

	{"prchk_reftag", offsetof(struct rpc_bdev_nvme_attach_controller, bdev_opts.prchk_flags), bdev_nvme_decode_reftag, true},
	{"prchk_guard", offsetof(struct rpc_bdev_nvme_attach_controller, bdev_opts.prchk_flags), bdev_nvme_decode_guard, true},
	{"hdgst", offsetof(struct rpc_bdev_nvme_attach_controller, drv_opts.header_digest), spdk_json_decode_bool, true},
	{"ddgst", offsetof(struct rpc_bdev_nvme_attach_controller, drv_opts.data_digest), spdk_json_decode_bool, true},
	{"fabrics_connect_timeout_us", offsetof(struct rpc_bdev_nvme_attach_controller, drv_opts.fabrics_connect_timeout_us), spdk_json_decode_uint64, true},
	{"multipath", offsetof(struct rpc_bdev_nvme_attach_controller, multipath), spdk_json_decode_string, true},
	{"num_io_queues", offsetof(struct rpc_bdev_nvme_attach_controller, drv_opts.num_io_queues), spdk_json_decode_uint32, true},
	{"ctrlr_loss_timeout_sec", offsetof(struct rpc_bdev_nvme_attach_controller, bdev_opts.ctrlr_loss_timeout_sec), spdk_json_decode_int32, true},
	{"reconnect_delay_sec", offsetof(struct rpc_bdev_nvme_attach_controller, bdev_opts.reconnect_delay_sec), spdk_json_decode_uint32, true},
	{"fast_io_fail_timeout_sec", offsetof(struct rpc_bdev_nvme_attach_controller, bdev_opts.fast_io_fail_timeout_sec), spdk_json_decode_uint32, true},
};

#define NVME_MAX_BDEVS_PER_RPC 128

struct rpc_bdev_nvme_attach_controller_ctx {
	struct rpc_bdev_nvme_attach_controller req;
	uint32_t count;
	size_t bdev_count;
	const char *names[NVME_MAX_BDEVS_PER_RPC];
	struct spdk_jsonrpc_request *request;
};

static void
rpc_bdev_nvme_attach_controller_examined(void *cb_ctx)
{
	struct rpc_bdev_nvme_attach_controller_ctx *ctx = cb_ctx;
	struct spdk_jsonrpc_request *request = ctx->request;
	struct spdk_json_write_ctx *w;
	size_t i;

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_array_begin(w);
	for (i = 0; i < ctx->bdev_count; i++) {
		spdk_json_write_string(w, ctx->names[i]);
	}
	spdk_json_write_array_end(w);
	spdk_jsonrpc_end_result(request, w);

	free_rpc_bdev_nvme_attach_controller(&ctx->req);
	free(ctx);
}

static void
rpc_bdev_nvme_attach_controller_done(void *cb_ctx, size_t bdev_count, int rc)
{
	struct rpc_bdev_nvme_attach_controller_ctx *ctx = cb_ctx;
	struct spdk_jsonrpc_request *request = ctx->request;

	if (rc < 0) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		free_rpc_bdev_nvme_attach_controller(&ctx->req);
		free(ctx);
		return;
	}

	ctx->bdev_count = bdev_count;
	spdk_bdev_wait_for_examine(rpc_bdev_nvme_attach_controller_examined, ctx);
}

static void
rpc_bdev_nvme_attach_controller(struct spdk_jsonrpc_request *request,
				const struct spdk_json_val *params)
{
	struct rpc_bdev_nvme_attach_controller_ctx *ctx;
	struct spdk_nvme_transport_id trid = {};
	const struct spdk_nvme_ctrlr_opts *drv_opts;
	const struct spdk_nvme_transport_id *ctrlr_trid;
	struct nvme_ctrlr *ctrlr = NULL;
	size_t len, maxlen;
	bool multipath = false;
	int rc;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		spdk_jsonrpc_send_error_response(request, -ENOMEM, spdk_strerror(ENOMEM));
		return;
	}

	spdk_nvme_ctrlr_get_default_ctrlr_opts(&ctx->req.drv_opts, sizeof(ctx->req.drv_opts));
	bdev_nvme_get_default_ctrlr_opts(&ctx->req.bdev_opts);

	if (spdk_json_decode_object(params, rpc_bdev_nvme_attach_controller_decoders,
				    SPDK_COUNTOF(rpc_bdev_nvme_attach_controller_decoders),
				    &ctx->req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	/* Parse trstring */
	rc = spdk_nvme_transport_id_populate_trstring(&trid, ctx->req.trtype);
	if (rc < 0) {
		SPDK_ERRLOG("Failed to parse trtype: %s\n", ctx->req.trtype);
		spdk_jsonrpc_send_error_response_fmt(request, -EINVAL, "Failed to parse trtype: %s",
						     ctx->req.trtype);
		goto cleanup;
	}

	/* Parse trtype */
	rc = spdk_nvme_transport_id_parse_trtype(&trid.trtype, ctx->req.trtype);
	assert(rc == 0);

	/* Parse traddr */
	maxlen = sizeof(trid.traddr);
	len = strnlen(ctx->req.traddr, maxlen);
	if (len == maxlen) {
		spdk_jsonrpc_send_error_response_fmt(request, -EINVAL, "traddr too long: %s",
						     ctx->req.traddr);
		goto cleanup;
	}
	memcpy(trid.traddr, ctx->req.traddr, len + 1);

	/* Parse adrfam */
	if (ctx->req.adrfam) {
		rc = spdk_nvme_transport_id_parse_adrfam(&trid.adrfam, ctx->req.adrfam);
		if (rc < 0) {
			SPDK_ERRLOG("Failed to parse adrfam: %s\n", ctx->req.adrfam);
			spdk_jsonrpc_send_error_response_fmt(request, -EINVAL, "Failed to parse adrfam: %s",
							     ctx->req.adrfam);
			goto cleanup;
		}
	}

	/* Parse trsvcid */
	if (ctx->req.trsvcid) {
		maxlen = sizeof(trid.trsvcid);
		len = strnlen(ctx->req.trsvcid, maxlen);
		if (len == maxlen) {
			spdk_jsonrpc_send_error_response_fmt(request, -EINVAL, "trsvcid too long: %s",
							     ctx->req.trsvcid);
			goto cleanup;
		}
		memcpy(trid.trsvcid, ctx->req.trsvcid, len + 1);
	}

	/* Parse priority for the NVMe-oF transport connection */
	if (ctx->req.priority) {
		trid.priority = spdk_strtol(ctx->req.priority, 10);
	}

	/* Parse subnqn */
	if (ctx->req.subnqn) {
		maxlen = sizeof(trid.subnqn);
		len = strnlen(ctx->req.subnqn, maxlen);
		if (len == maxlen) {
			spdk_jsonrpc_send_error_response_fmt(request, -EINVAL, "subnqn too long: %s",
							     ctx->req.subnqn);
			goto cleanup;
		}
		memcpy(trid.subnqn, ctx->req.subnqn, len + 1);
	}

	if (ctx->req.hostnqn) {
		snprintf(ctx->req.drv_opts.hostnqn, sizeof(ctx->req.drv_opts.hostnqn), "%s",
			 ctx->req.hostnqn);
	}

	if (ctx->req.hostaddr) {
		maxlen = sizeof(ctx->req.drv_opts.src_addr);
		len = strnlen(ctx->req.hostaddr, maxlen);
		if (len == maxlen) {
			spdk_jsonrpc_send_error_response_fmt(request, -EINVAL, "hostaddr too long: %s",
							     ctx->req.hostaddr);
			goto cleanup;
		}
		snprintf(ctx->req.drv_opts.src_addr, maxlen, "%s", ctx->req.hostaddr);
	}

	if (ctx->req.hostsvcid) {
		maxlen = sizeof(ctx->req.drv_opts.src_svcid);
		len = strnlen(ctx->req.hostsvcid, maxlen);
		if (len == maxlen) {
			spdk_jsonrpc_send_error_response_fmt(request, -EINVAL, "hostsvcid too long: %s",
							     ctx->req.hostsvcid);
			goto cleanup;
		}
		snprintf(ctx->req.drv_opts.src_svcid, maxlen, "%s", ctx->req.hostsvcid);
	}

	ctrlr = nvme_ctrlr_get_by_name(ctx->req.name);

	if (ctrlr) {
		if (ctx->req.multipath == NULL) {
			/* For now, this means add a failover path. This maintains backward compatibility
			 * with past behavior. In the future, this behavior will change to "disable". */
			SPDK_ERRLOG("The multipath parameter was not specified to bdev_nvme_attach_controller but "
				    "it was used to add a failover path. This behavior will default to rejecting "
				    "the request in the future. Specify the 'multipath' parameter to control the behavior\n");
			ctx->req.multipath = strdup("failover");
			if (ctx->req.multipath == NULL) {
				SPDK_ERRLOG("cannot allocate multipath failover string\n");
				goto cleanup;
			}
		}

		/* This controller already exists. Check what the user wants to do. */
		if (strcasecmp(ctx->req.multipath, "disable") == 0) {
			/* The user does not want to do any form of multipathing. */
			spdk_jsonrpc_send_error_response_fmt(request, -EALREADY,
							     "A controller named %s already exists and multipath is disabled\n",
							     ctx->req.name);
			goto cleanup;

		} else if (strcasecmp(ctx->req.multipath, "failover") != 0 &&
			   strcasecmp(ctx->req.multipath, "multipath") != 0) {
			/* Invalid multipath option */
			spdk_jsonrpc_send_error_response_fmt(request, -EINVAL,
							     "Invalid multipath parameter: %s\n",
							     ctx->req.multipath);
			goto cleanup;
		}

		/* The user wants to add this as a failover path or add this to create multipath. */
		drv_opts = spdk_nvme_ctrlr_get_opts(ctrlr->ctrlr);
		ctrlr_trid = spdk_nvme_ctrlr_get_transport_id(ctrlr->ctrlr);

		if (strncmp(trid.traddr, ctrlr_trid->traddr, sizeof(trid.traddr)) == 0 &&
		    strncmp(trid.trsvcid, ctrlr_trid->trsvcid, sizeof(trid.trsvcid)) == 0 &&
		    strncmp(ctx->req.drv_opts.src_addr, drv_opts->src_addr, sizeof(drv_opts->src_addr)) == 0 &&
		    strncmp(ctx->req.drv_opts.src_svcid, drv_opts->src_svcid, sizeof(drv_opts->src_svcid)) == 0) {
			/* Exactly same network path can't be added a second time */
			spdk_jsonrpc_send_error_response_fmt(request, -EALREADY,
							     "A controller named %s already exists with the specified network path\n",
							     ctx->req.name);
			goto cleanup;
		}

		if (strncmp(trid.subnqn,
			    ctrlr_trid->subnqn,
			    SPDK_NVMF_NQN_MAX_LEN) != 0) {
			/* Different SUBNQN is not allowed when specifying the same controller name. */
			spdk_jsonrpc_send_error_response_fmt(request, -EINVAL,
							     "A controller named %s already exists, but uses a different subnqn (%s)\n",
							     ctx->req.name, ctrlr_trid->subnqn);
			goto cleanup;
		}

		if (strncmp(ctx->req.drv_opts.hostnqn, drv_opts->hostnqn, SPDK_NVMF_NQN_MAX_LEN) != 0) {
			/* Different HOSTNQN is not allowed when specifying the same controller name. */
			spdk_jsonrpc_send_error_response_fmt(request, -EINVAL,
							     "A controller named %s already exists, but uses a different hostnqn (%s)\n",
							     ctx->req.name, drv_opts->hostnqn);
			goto cleanup;
		}

		if (ctx->req.bdev_opts.prchk_flags) {
			spdk_jsonrpc_send_error_response_fmt(request, -EINVAL,
							     "A controller named %s already exists. To add a path, do not specify PI options.\n",
							     ctx->req.name);
			goto cleanup;
		}

		ctx->req.bdev_opts.prchk_flags = ctrlr->opts.prchk_flags;
	}

	if (ctx->req.multipath != NULL && strcasecmp(ctx->req.multipath, "multipath") == 0) {
		multipath = true;
	}

	if (ctx->req.drv_opts.num_io_queues == 0 || ctx->req.drv_opts.num_io_queues > UINT16_MAX + 1) {
		spdk_jsonrpc_send_error_response_fmt(request, -EINVAL,
						     "num_io_queues out of bounds, min: %u max: %u\n",
						     1, UINT16_MAX + 1);
		goto cleanup;
	}

	ctx->request = request;
	ctx->count = NVME_MAX_BDEVS_PER_RPC;
	/* Should already be zero due to the calloc(), but set explicitly for clarity. */
	ctx->req.bdev_opts.from_discovery_service = false;
	rc = bdev_nvme_create(&trid, ctx->req.name, ctx->names, ctx->count,
			      rpc_bdev_nvme_attach_controller_done, ctx, &ctx->req.drv_opts,
			      &ctx->req.bdev_opts, multipath);
	if (rc) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	return;

cleanup:
	free_rpc_bdev_nvme_attach_controller(&ctx->req);
	free(ctx);
}
SPDK_RPC_REGISTER("bdev_nvme_attach_controller", rpc_bdev_nvme_attach_controller,
		  SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(bdev_nvme_attach_controller, construct_nvme_bdev)

static const char *
nvme_ctrlr_get_state_str(struct nvme_ctrlr *nvme_ctrlr)
{
	if (nvme_ctrlr->destruct) {
		return "deleting";
	} else if (spdk_nvme_ctrlr_is_failed(nvme_ctrlr->ctrlr)) {
		return "failed";
	} else if (nvme_ctrlr->resetting) {
		return "resetting";
	} else if (nvme_ctrlr->reconnect_is_delayed > 0) {
		return "reconnect_is_delayed";
	} else {
		return "enabled";
	}
}

static void
rpc_dump_nvme_bdev_controller_info(struct nvme_bdev_ctrlr *nbdev_ctrlr, void *ctx)
{
	struct spdk_json_write_ctx	*w = ctx;
	struct spdk_nvme_transport_id   *trid;
	struct nvme_ctrlr		*nvme_ctrlr;
	const struct spdk_nvme_ctrlr_opts *opts;

	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "name", nbdev_ctrlr->name);

	spdk_json_write_named_array_begin(w, "ctrlrs");
	TAILQ_FOREACH(nvme_ctrlr, &nbdev_ctrlr->ctrlrs, tailq) {
		spdk_json_write_object_begin(w);

		spdk_json_write_named_string(w, "state", nvme_ctrlr_get_state_str(nvme_ctrlr));

#ifdef SPDK_CONFIG_NVME_CUSE
		size_t cuse_name_size = 128;
		char cuse_name[cuse_name_size];

		int rc = spdk_nvme_cuse_get_ctrlr_name(nvme_ctrlr->ctrlr, cuse_name, &cuse_name_size);
		if (rc == 0) {
			spdk_json_write_named_string(w, "cuse_device", cuse_name);
		}
#endif
		trid = &nvme_ctrlr->active_path_id->trid;
		spdk_json_write_named_object_begin(w, "trid");
		nvme_bdev_dump_trid_json(trid, w);
		spdk_json_write_object_end(w);

		opts = spdk_nvme_ctrlr_get_opts(nvme_ctrlr->ctrlr);
		spdk_json_write_named_object_begin(w, "host");
		spdk_json_write_named_string(w, "nqn", opts->hostnqn);
		spdk_json_write_named_string(w, "addr", opts->src_addr);
		spdk_json_write_named_string(w, "svcid", opts->src_svcid);
		spdk_json_write_object_end(w);
		spdk_json_write_object_end(w);
	}
	spdk_json_write_array_end(w);
	spdk_json_write_object_end(w);
}

struct rpc_bdev_nvme_get_controllers {
	char *name;
};

static void
free_rpc_bdev_nvme_get_controllers(struct rpc_bdev_nvme_get_controllers *r)
{
	free(r->name);
}

static const struct spdk_json_object_decoder rpc_bdev_nvme_get_controllers_decoders[] = {
	{"name", offsetof(struct rpc_bdev_nvme_get_controllers, name), spdk_json_decode_string, true},
};

static void
rpc_bdev_nvme_get_controllers(struct spdk_jsonrpc_request *request,
			      const struct spdk_json_val *params)
{
	struct rpc_bdev_nvme_get_controllers req = {};
	struct spdk_json_write_ctx *w;
	struct nvme_bdev_ctrlr *nbdev_ctrlr = NULL;

	if (params && spdk_json_decode_object(params, rpc_bdev_nvme_get_controllers_decoders,
					      SPDK_COUNTOF(rpc_bdev_nvme_get_controllers_decoders),
					      &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	if (req.name) {
		nbdev_ctrlr = nvme_bdev_ctrlr_get_by_name(req.name);
		if (nbdev_ctrlr == NULL) {
			SPDK_ERRLOG("ctrlr '%s' does not exist\n", req.name);
			spdk_jsonrpc_send_error_response_fmt(request, EINVAL, "Controller %s does not exist", req.name);
			goto cleanup;
		}
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_array_begin(w);

	if (nbdev_ctrlr != NULL) {
		rpc_dump_nvme_bdev_controller_info(nbdev_ctrlr, w);
	} else {
		nvme_bdev_ctrlr_for_each(rpc_dump_nvme_bdev_controller_info, w);
	}

	spdk_json_write_array_end(w);

	spdk_jsonrpc_end_result(request, w);

cleanup:
	free_rpc_bdev_nvme_get_controllers(&req);
}
SPDK_RPC_REGISTER("bdev_nvme_get_controllers", rpc_bdev_nvme_get_controllers, SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(bdev_nvme_get_controllers, get_nvme_controllers)

struct rpc_bdev_nvme_detach_controller {
	char *name;
	char *trtype;
	char *adrfam;
	char *traddr;
	char *trsvcid;
	char *subnqn;
	char *hostaddr;
	char *hostsvcid;
};

static void
free_rpc_bdev_nvme_detach_controller(struct rpc_bdev_nvme_detach_controller *req)
{
	free(req->name);
	free(req->trtype);
	free(req->adrfam);
	free(req->traddr);
	free(req->trsvcid);
	free(req->subnqn);
	free(req->hostaddr);
	free(req->hostsvcid);
}

static const struct spdk_json_object_decoder rpc_bdev_nvme_detach_controller_decoders[] = {
	{"name", offsetof(struct rpc_bdev_nvme_detach_controller, name), spdk_json_decode_string},
	{"trtype", offsetof(struct rpc_bdev_nvme_detach_controller, trtype), spdk_json_decode_string, true},
	{"traddr", offsetof(struct rpc_bdev_nvme_detach_controller, traddr), spdk_json_decode_string, true},
	{"adrfam", offsetof(struct rpc_bdev_nvme_detach_controller, adrfam), spdk_json_decode_string, true},
	{"trsvcid", offsetof(struct rpc_bdev_nvme_detach_controller, trsvcid), spdk_json_decode_string, true},
	{"subnqn", offsetof(struct rpc_bdev_nvme_detach_controller, subnqn), spdk_json_decode_string, true},
	{"hostaddr", offsetof(struct rpc_bdev_nvme_detach_controller, hostaddr), spdk_json_decode_string, true},
	{"hostsvcid", offsetof(struct rpc_bdev_nvme_detach_controller, hostsvcid), spdk_json_decode_string, true},
};

static void
rpc_bdev_nvme_detach_controller(struct spdk_jsonrpc_request *request,
				const struct spdk_json_val *params)
{
	struct rpc_bdev_nvme_detach_controller req = {NULL};
	struct nvme_path_id path = {};
	size_t len, maxlen;
	int rc = 0;

	if (spdk_json_decode_object(params, rpc_bdev_nvme_detach_controller_decoders,
				    SPDK_COUNTOF(rpc_bdev_nvme_detach_controller_decoders),
				    &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	if (req.trtype != NULL) {
		rc = spdk_nvme_transport_id_populate_trstring(&path.trid, req.trtype);
		if (rc < 0) {
			SPDK_ERRLOG("Failed to parse trtype: %s\n", req.trtype);
			spdk_jsonrpc_send_error_response_fmt(request, -EINVAL, "Failed to parse trtype: %s",
							     req.trtype);
			goto cleanup;
		}

		rc = spdk_nvme_transport_id_parse_trtype(&path.trid.trtype, req.trtype);
		if (rc < 0) {
			SPDK_ERRLOG("Failed to parse trtype: %s\n", req.trtype);
			spdk_jsonrpc_send_error_response_fmt(request, -EINVAL, "Failed to parse trtype: %s",
							     req.trtype);
			goto cleanup;
		}
	}

	if (req.traddr != NULL) {
		maxlen = sizeof(path.trid.traddr);
		len = strnlen(req.traddr, maxlen);
		if (len == maxlen) {
			spdk_jsonrpc_send_error_response_fmt(request, -EINVAL, "traddr too long: %s",
							     req.traddr);
			goto cleanup;
		}
		memcpy(path.trid.traddr, req.traddr, len + 1);
	}

	if (req.adrfam != NULL) {
		rc = spdk_nvme_transport_id_parse_adrfam(&path.trid.adrfam, req.adrfam);
		if (rc < 0) {
			SPDK_ERRLOG("Failed to parse adrfam: %s\n", req.adrfam);
			spdk_jsonrpc_send_error_response_fmt(request, -EINVAL, "Failed to parse adrfam: %s",
							     req.adrfam);
			goto cleanup;
		}
	}

	if (req.trsvcid != NULL) {
		maxlen = sizeof(path.trid.trsvcid);
		len = strnlen(req.trsvcid, maxlen);
		if (len == maxlen) {
			spdk_jsonrpc_send_error_response_fmt(request, -EINVAL, "trsvcid too long: %s",
							     req.trsvcid);
			goto cleanup;
		}
		memcpy(path.trid.trsvcid, req.trsvcid, len + 1);
	}

	/* Parse subnqn */
	if (req.subnqn != NULL) {
		maxlen = sizeof(path.trid.subnqn);
		len = strnlen(req.subnqn, maxlen);
		if (len == maxlen) {
			spdk_jsonrpc_send_error_response_fmt(request, -EINVAL, "subnqn too long: %s",
							     req.subnqn);
			goto cleanup;
		}
		memcpy(path.trid.subnqn, req.subnqn, len + 1);
	}

	if (req.hostaddr) {
		maxlen = sizeof(path.hostid.hostaddr);
		len = strnlen(req.hostaddr, maxlen);
		if (len == maxlen) {
			spdk_jsonrpc_send_error_response_fmt(request, -EINVAL, "hostaddr too long: %s",
							     req.hostaddr);
			goto cleanup;
		}
		snprintf(path.hostid.hostaddr, maxlen, "%s", req.hostaddr);
	}

	if (req.hostsvcid) {
		maxlen = sizeof(path.hostid.hostsvcid);
		len = strnlen(req.hostsvcid, maxlen);
		if (len == maxlen) {
			spdk_jsonrpc_send_error_response_fmt(request, -EINVAL, "hostsvcid too long: %s",
							     req.hostsvcid);
			goto cleanup;
		}
		snprintf(path.hostid.hostsvcid, maxlen, "%s", req.hostsvcid);
	}

	rc = bdev_nvme_delete(req.name, &path);

	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	spdk_jsonrpc_send_bool_response(request, true);

cleanup:
	free_rpc_bdev_nvme_detach_controller(&req);
}
SPDK_RPC_REGISTER("bdev_nvme_detach_controller", rpc_bdev_nvme_detach_controller,
		  SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(bdev_nvme_detach_controller, delete_nvme_controller)

struct rpc_apply_firmware {
	char *filename;
	char *bdev_name;
};

static void
free_rpc_apply_firmware(struct rpc_apply_firmware *req)
{
	free(req->filename);
	free(req->bdev_name);
}

static const struct spdk_json_object_decoder rpc_apply_firmware_decoders[] = {
	{"filename", offsetof(struct rpc_apply_firmware, filename), spdk_json_decode_string},
	{"bdev_name", offsetof(struct rpc_apply_firmware, bdev_name), spdk_json_decode_string},
};

struct firmware_update_info {
	void				*fw_image;
	void				*p;
	unsigned int			size;
	unsigned int			size_remaining;
	unsigned int			offset;
	unsigned int			transfer;

	void				*desc;
	struct spdk_io_channel		*ch;
	struct spdk_jsonrpc_request	*request;
	struct spdk_nvme_ctrlr		*ctrlr;
	open_descriptors_t		desc_head;
	struct rpc_apply_firmware	*req;
};

static void
_apply_firmware_cleanup(void *ctx)
{
	struct spdk_bdev_desc *desc = ctx;

	spdk_bdev_close(desc);
}

static void
apply_firmware_cleanup(void *cb_arg)
{
	struct open_descriptors			*opt, *tmp;
	struct firmware_update_info *firm_ctx = cb_arg;

	if (!firm_ctx) {
		return;
	}

	if (firm_ctx->fw_image) {
		spdk_free(firm_ctx->fw_image);
	}

	if (firm_ctx->req) {
		free_rpc_apply_firmware(firm_ctx->req);
		free(firm_ctx->req);
	}

	if (firm_ctx->ch) {
		spdk_put_io_channel(firm_ctx->ch);
	}

	TAILQ_FOREACH_SAFE(opt, &firm_ctx->desc_head, tqlst, tmp) {
		TAILQ_REMOVE(&firm_ctx->desc_head, opt, tqlst);
		/* Close the underlying bdev on its same opened thread. */
		if (opt->thread && opt->thread != spdk_get_thread()) {
			spdk_thread_send_msg(opt->thread, _apply_firmware_cleanup, opt->desc);
		} else {
			spdk_bdev_close(opt->desc);
		}
		free(opt);
	}
	free(firm_ctx);
}

static void
apply_firmware_complete_reset(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct spdk_json_write_ctx		*w;
	struct firmware_update_info *firm_ctx = cb_arg;

	spdk_bdev_free_io(bdev_io);

	if (!success) {
		spdk_jsonrpc_send_error_response(firm_ctx->request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "firmware commit failed.");
		apply_firmware_cleanup(firm_ctx);
		return;
	}

	if (spdk_nvme_ctrlr_reset(firm_ctx->ctrlr) != 0) {
		spdk_jsonrpc_send_error_response(firm_ctx->request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Controller reset failed.");
		apply_firmware_cleanup(firm_ctx);
		return;
	}

	w = spdk_jsonrpc_begin_result(firm_ctx->request);
	spdk_json_write_string(w, "firmware commit succeeded. Controller reset in progress.");
	spdk_jsonrpc_end_result(firm_ctx->request, w);
	apply_firmware_cleanup(firm_ctx);
}

static void
apply_firmware_complete(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct spdk_nvme_cmd			cmd = {};
	struct spdk_nvme_fw_commit		fw_commit;
	int					slot = 0;
	int					rc;
	struct firmware_update_info *firm_ctx = cb_arg;
	enum spdk_nvme_fw_commit_action commit_action = SPDK_NVME_FW_COMMIT_REPLACE_AND_ENABLE_IMG;

	spdk_bdev_free_io(bdev_io);

	if (!success) {
		spdk_jsonrpc_send_error_response(firm_ctx->request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "firmware download failed .");
		apply_firmware_cleanup(firm_ctx);
		return;
	}

	firm_ctx->p += firm_ctx->transfer;
	firm_ctx->offset += firm_ctx->transfer;
	firm_ctx->size_remaining -= firm_ctx->transfer;

	switch (firm_ctx->size_remaining) {
	case 0:
		/* firmware download completed. Commit firmware */
		memset(&fw_commit, 0, sizeof(struct spdk_nvme_fw_commit));
		fw_commit.fs = slot;
		fw_commit.ca = commit_action;

		cmd.opc = SPDK_NVME_OPC_FIRMWARE_COMMIT;
		memcpy(&cmd.cdw10, &fw_commit, sizeof(uint32_t));
		rc = spdk_bdev_nvme_admin_passthru(firm_ctx->desc, firm_ctx->ch, &cmd, NULL, 0,
						   apply_firmware_complete_reset, firm_ctx);
		if (rc) {
			spdk_jsonrpc_send_error_response(firm_ctx->request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
							 "firmware commit failed.");
			apply_firmware_cleanup(firm_ctx);
			return;
		}
		break;
	default:
		firm_ctx->transfer = spdk_min(firm_ctx->size_remaining, 4096);
		cmd.opc = SPDK_NVME_OPC_FIRMWARE_IMAGE_DOWNLOAD;

		cmd.cdw10 = spdk_nvme_bytes_to_numd(firm_ctx->transfer);
		cmd.cdw11 = firm_ctx->offset >> 2;
		rc = spdk_bdev_nvme_admin_passthru(firm_ctx->desc, firm_ctx->ch, &cmd, firm_ctx->p,
						   firm_ctx->transfer, apply_firmware_complete, firm_ctx);
		if (rc) {
			spdk_jsonrpc_send_error_response(firm_ctx->request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
							 "firmware download failed.");
			apply_firmware_cleanup(firm_ctx);
			return;
		}
		break;
	}
}

static void
apply_firmware_open_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev, void *event_ctx)
{
}

static void
rpc_bdev_nvme_apply_firmware(struct spdk_jsonrpc_request *request,
			     const struct spdk_json_val *params)
{
	int					rc;
	int					fd = -1;
	struct stat				fw_stat;
	struct spdk_nvme_ctrlr			*ctrlr;
	char					msg[1024];
	struct spdk_bdev			*bdev;
	struct spdk_bdev			*bdev2;
	struct open_descriptors			*opt;
	struct spdk_bdev_desc			*desc;
	struct spdk_nvme_cmd			*cmd;
	struct firmware_update_info		*firm_ctx;

	firm_ctx = calloc(1, sizeof(struct firmware_update_info));
	if (!firm_ctx) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Memory allocation error.");
		return;
	}
	firm_ctx->fw_image = NULL;
	TAILQ_INIT(&firm_ctx->desc_head);
	firm_ctx->request = request;

	firm_ctx->req = calloc(1, sizeof(struct rpc_apply_firmware));
	if (!firm_ctx->req) {
		snprintf(msg, sizeof(msg), "Memory allocation error.");
		goto err;
	}

	if (spdk_json_decode_object(params, rpc_apply_firmware_decoders,
				    SPDK_COUNTOF(rpc_apply_firmware_decoders), firm_ctx->req)) {
		snprintf(msg, sizeof(msg), "spdk_json_decode_object failed.");
		goto err;
	}

	if ((bdev = spdk_bdev_get_by_name(firm_ctx->req->bdev_name)) == NULL) {
		snprintf(msg, sizeof(msg), "bdev %s were not found", firm_ctx->req->bdev_name);
		goto err;
	}

	if ((ctrlr = bdev_nvme_get_ctrlr(bdev)) == NULL) {
		snprintf(msg, sizeof(msg), "Controller information for %s were not found.",
			 firm_ctx->req->bdev_name);
		goto err;
	}
	firm_ctx->ctrlr = ctrlr;

	for (bdev2 = spdk_bdev_first(); bdev2; bdev2 = spdk_bdev_next(bdev2)) {

		if (bdev_nvme_get_ctrlr(bdev2) != ctrlr) {
			continue;
		}

		if (!(opt = malloc(sizeof(struct open_descriptors)))) {
			snprintf(msg, sizeof(msg), "Memory allocation error.");
			goto err;
		}

		if (spdk_bdev_open_ext(spdk_bdev_get_name(bdev2), true, apply_firmware_open_cb, NULL, &desc) != 0) {
			snprintf(msg, sizeof(msg), "Device %s is in use.", firm_ctx->req->bdev_name);
			free(opt);
			goto err;
		}

		/* Save the thread where the base device is opened */
		opt->thread = spdk_get_thread();

		opt->desc = desc;
		opt->bdev = bdev;
		TAILQ_INSERT_TAIL(&firm_ctx->desc_head, opt, tqlst);
	}

	/*
	 * find a descriptor associated with our bdev
	 */
	firm_ctx->desc = NULL;
	TAILQ_FOREACH(opt, &firm_ctx->desc_head, tqlst) {
		if (opt->bdev == bdev) {
			firm_ctx->desc = opt->desc;
			break;
		}
	}

	if (!firm_ctx->desc) {
		snprintf(msg, sizeof(msg), "No descriptor were found.");
		goto err;
	}

	firm_ctx->ch = spdk_bdev_get_io_channel(firm_ctx->desc);
	if (!firm_ctx->ch) {
		snprintf(msg, sizeof(msg), "No channels were found.");
		goto err;
	}

	fd = open(firm_ctx->req->filename, O_RDONLY);
	if (fd < 0) {
		snprintf(msg, sizeof(msg), "open file failed.");
		goto err;
	}

	rc = fstat(fd, &fw_stat);
	if (rc < 0) {
		close(fd);
		snprintf(msg, sizeof(msg), "fstat failed.");
		goto err;
	}

	firm_ctx->size = fw_stat.st_size;
	if (fw_stat.st_size % 4) {
		close(fd);
		snprintf(msg, sizeof(msg), "Firmware image size is not multiple of 4.");
		goto err;
	}

	firm_ctx->fw_image = spdk_zmalloc(firm_ctx->size, 4096, NULL,
					  SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	if (!firm_ctx->fw_image) {
		close(fd);
		snprintf(msg, sizeof(msg), "Memory allocation error.");
		goto err;
	}
	firm_ctx->p = firm_ctx->fw_image;

	if (read(fd, firm_ctx->p, firm_ctx->size) != ((ssize_t)(firm_ctx->size))) {
		close(fd);
		snprintf(msg, sizeof(msg), "Read firmware image failed!");
		goto err;
	}
	close(fd);

	firm_ctx->offset = 0;
	firm_ctx->size_remaining = firm_ctx->size;
	firm_ctx->transfer = spdk_min(firm_ctx->size_remaining, 4096);

	cmd = malloc(sizeof(struct spdk_nvme_cmd));
	if (!cmd) {
		snprintf(msg, sizeof(msg), "Memory allocation error.");
		goto err;
	}
	memset(cmd, 0, sizeof(struct spdk_nvme_cmd));
	cmd->opc = SPDK_NVME_OPC_FIRMWARE_IMAGE_DOWNLOAD;

	cmd->cdw10 = spdk_nvme_bytes_to_numd(firm_ctx->transfer);
	cmd->cdw11 = firm_ctx->offset >> 2;

	rc = spdk_bdev_nvme_admin_passthru(firm_ctx->desc, firm_ctx->ch, cmd, firm_ctx->p,
					   firm_ctx->transfer, apply_firmware_complete, firm_ctx);
	if (rc == 0) {
		/* normal return here. */
		return;
	}

	free(cmd);
	snprintf(msg, sizeof(msg), "Read firmware image failed!");
err:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, msg);
	apply_firmware_cleanup(firm_ctx);
}
SPDK_RPC_REGISTER("bdev_nvme_apply_firmware", rpc_bdev_nvme_apply_firmware, SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(bdev_nvme_apply_firmware, apply_nvme_firmware)

struct rpc_bdev_nvme_transport_stat_ctx {
	struct spdk_jsonrpc_request *request;
	struct spdk_json_write_ctx *w;
};

static void
rpc_bdev_nvme_rdma_stats(struct spdk_json_write_ctx *w,
			 struct spdk_nvme_transport_poll_group_stat *stat)
{
	struct spdk_nvme_rdma_device_stat *device_stats;
	uint32_t i;

	spdk_json_write_named_array_begin(w, "devices");

	for (i = 0; i < stat->rdma.num_devices; i++) {
		device_stats = &stat->rdma.device_stats[i];
		spdk_json_write_object_begin(w);
		spdk_json_write_named_string(w, "dev_name", device_stats->name);
		spdk_json_write_named_uint64(w, "polls", device_stats->polls);
		spdk_json_write_named_uint64(w, "idle_polls", device_stats->idle_polls);
		spdk_json_write_named_uint64(w, "completions", device_stats->completions);
		spdk_json_write_named_uint64(w, "queued_requests", device_stats->queued_requests);
		spdk_json_write_named_uint64(w, "total_send_wrs", device_stats->total_send_wrs);
		spdk_json_write_named_uint64(w, "send_doorbell_updates", device_stats->send_doorbell_updates);
		spdk_json_write_named_uint64(w, "total_recv_wrs", device_stats->total_recv_wrs);
		spdk_json_write_named_uint64(w, "recv_doorbell_updates", device_stats->recv_doorbell_updates);
		spdk_json_write_object_end(w);
	}
	spdk_json_write_array_end(w);
}

static void
rpc_bdev_nvme_pcie_stats(struct spdk_json_write_ctx *w,
			 struct spdk_nvme_transport_poll_group_stat *stat)
{
	spdk_json_write_named_uint64(w, "polls", stat->pcie.polls);
	spdk_json_write_named_uint64(w, "idle_polls", stat->pcie.idle_polls);
	spdk_json_write_named_uint64(w, "completions", stat->pcie.completions);
	spdk_json_write_named_uint64(w, "cq_mmio_doorbell_updates", stat->pcie.cq_mmio_doorbell_updates);
	spdk_json_write_named_uint64(w, "cq_shadow_doorbell_updates",
				     stat->pcie.cq_shadow_doorbell_updates);
	spdk_json_write_named_uint64(w, "queued_requests", stat->pcie.queued_requests);
	spdk_json_write_named_uint64(w, "submitted_requests", stat->pcie.submitted_requests);
	spdk_json_write_named_uint64(w, "sq_mmio_doorbell_updates", stat->pcie.sq_mmio_doorbell_updates);
	spdk_json_write_named_uint64(w, "sq_shadow_doorbell_updates",
				     stat->pcie.sq_shadow_doorbell_updates);
}

static void
rpc_bdev_nvme_tcp_stats(struct spdk_json_write_ctx *w,
			struct spdk_nvme_transport_poll_group_stat *stat)
{
	spdk_json_write_named_uint64(w, "polls", stat->tcp.polls);
	spdk_json_write_named_uint64(w, "idle_polls", stat->tcp.idle_polls);
	spdk_json_write_named_uint64(w, "socket_completions", stat->tcp.socket_completions);
	spdk_json_write_named_uint64(w, "nvme_completions", stat->tcp.nvme_completions);
	spdk_json_write_named_uint64(w, "queued_requests", stat->tcp.queued_requests);
	spdk_json_write_named_uint64(w, "submitted_requests", stat->tcp.submitted_requests);
}

static void
rpc_bdev_nvme_stats_per_channel(struct spdk_io_channel_iter *i)
{
	struct rpc_bdev_nvme_transport_stat_ctx *ctx;
	struct spdk_io_channel *ch;
	struct nvme_poll_group *group;
	struct spdk_nvme_poll_group_stat *stat;
	struct spdk_nvme_transport_poll_group_stat *tr_stat;
	uint32_t j;
	int rc;

	ctx = spdk_io_channel_iter_get_ctx(i);
	ch = spdk_io_channel_iter_get_channel(i);
	group = spdk_io_channel_get_ctx(ch);

	rc = spdk_nvme_poll_group_get_stats(group->group, &stat);
	if (rc) {
		spdk_for_each_channel_continue(i, rc);
		return;
	}

	spdk_json_write_object_begin(ctx->w);
	spdk_json_write_named_string(ctx->w, "thread", spdk_thread_get_name(spdk_get_thread()));
	spdk_json_write_named_array_begin(ctx->w, "transports");

	for (j = 0; j < stat->num_transports; j++) {
		tr_stat = stat->transport_stat[j];
		spdk_json_write_object_begin(ctx->w);
		spdk_json_write_named_string(ctx->w, "trname", spdk_nvme_transport_id_trtype_str(tr_stat->trtype));

		switch (stat->transport_stat[j]->trtype) {
		case SPDK_NVME_TRANSPORT_RDMA:
			rpc_bdev_nvme_rdma_stats(ctx->w, tr_stat);
			break;
		case SPDK_NVME_TRANSPORT_PCIE:
			rpc_bdev_nvme_pcie_stats(ctx->w, tr_stat);
			break;
		case SPDK_NVME_TRANSPORT_TCP:
			rpc_bdev_nvme_tcp_stats(ctx->w, tr_stat);
			break;
		default:
			SPDK_WARNLOG("Can't handle trtype %d %s\n", tr_stat->trtype,
				     spdk_nvme_transport_id_trtype_str(tr_stat->trtype));
		}
		spdk_json_write_object_end(ctx->w);
	}
	/* transports array */
	spdk_json_write_array_end(ctx->w);
	spdk_json_write_object_end(ctx->w);

	spdk_nvme_poll_group_free_stats(group->group, stat);
	spdk_for_each_channel_continue(i, 0);
}

static void
rpc_bdev_nvme_stats_done(struct spdk_io_channel_iter *i, int status)
{
	struct rpc_bdev_nvme_transport_stat_ctx *ctx = spdk_io_channel_iter_get_ctx(i);

	spdk_json_write_array_end(ctx->w);
	spdk_json_write_object_end(ctx->w);
	spdk_jsonrpc_end_result(ctx->request, ctx->w);
	free(ctx);
}

static void
rpc_bdev_nvme_get_transport_statistics(struct spdk_jsonrpc_request *request,
				       const struct spdk_json_val *params)
{
	struct rpc_bdev_nvme_transport_stat_ctx *ctx;

	if (params) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "'bdev_nvme_get_transport_statistics' requires no arguments");
		return;
	}

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Memory allocation error");
		return;
	}
	ctx->request = request;
	ctx->w = spdk_jsonrpc_begin_result(ctx->request);
	spdk_json_write_object_begin(ctx->w);
	spdk_json_write_named_array_begin(ctx->w, "poll_groups");

	spdk_for_each_channel(&g_nvme_bdev_ctrlrs,
			      rpc_bdev_nvme_stats_per_channel,
			      ctx,
			      rpc_bdev_nvme_stats_done);
}
SPDK_RPC_REGISTER("bdev_nvme_get_transport_statistics", rpc_bdev_nvme_get_transport_statistics,
		  SPDK_RPC_RUNTIME)

struct rpc_bdev_nvme_reset_controller_req {
	char *name;
};

static void
free_rpc_bdev_nvme_reset_controller_req(struct rpc_bdev_nvme_reset_controller_req *r)
{
	free(r->name);
}

static const struct spdk_json_object_decoder rpc_bdev_nvme_reset_controller_req_decoders[] = {
	{"name", offsetof(struct rpc_bdev_nvme_reset_controller_req, name), spdk_json_decode_string},
};

struct rpc_bdev_nvme_reset_controller_ctx {
	struct spdk_jsonrpc_request *request;
	bool success;
	struct spdk_thread *orig_thread;
};

static void
_rpc_bdev_nvme_reset_controller_cb(void *_ctx)
{
	struct rpc_bdev_nvme_reset_controller_ctx *ctx = _ctx;

	spdk_jsonrpc_send_bool_response(ctx->request, ctx->success);

	free(ctx);
}

static void
rpc_bdev_nvme_reset_controller_cb(void *cb_arg, bool success)
{
	struct rpc_bdev_nvme_reset_controller_ctx *ctx = cb_arg;

	ctx->success = success;

	spdk_thread_send_msg(ctx->orig_thread, _rpc_bdev_nvme_reset_controller_cb, ctx);
}

static void
rpc_bdev_nvme_reset_controller(struct spdk_jsonrpc_request *request,
			       const struct spdk_json_val *params)
{
	struct rpc_bdev_nvme_reset_controller_req req = {NULL};
	struct rpc_bdev_nvme_reset_controller_ctx *ctx;
	struct nvme_ctrlr *nvme_ctrlr;
	int rc;

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		SPDK_ERRLOG("Memory allocation failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Memory allocation failed");
		return;
	}

	if (spdk_json_decode_object(params, rpc_bdev_nvme_reset_controller_req_decoders,
				    SPDK_COUNTOF(rpc_bdev_nvme_reset_controller_req_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, spdk_strerror(EINVAL));
		goto err;
	}

	nvme_ctrlr = nvme_ctrlr_get_by_name(req.name);
	if (nvme_ctrlr == NULL) {
		SPDK_ERRLOG("Failed at device lookup\n");
		spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
		goto err;
	}

	ctx->request = request;
	ctx->orig_thread = spdk_get_thread();

	rc = bdev_nvme_reset_rpc(nvme_ctrlr, rpc_bdev_nvme_reset_controller_cb, ctx);
	if (rc != 0) {
		SPDK_NOTICELOG("Failed at bdev_nvme_reset_rpc\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, spdk_strerror(-rc));
		goto err;
	}

	free_rpc_bdev_nvme_reset_controller_req(&req);
	return;

err:
	free_rpc_bdev_nvme_reset_controller_req(&req);
	free(ctx);
}
SPDK_RPC_REGISTER("bdev_nvme_reset_controller", rpc_bdev_nvme_reset_controller, SPDK_RPC_RUNTIME)

struct rpc_get_controller_health_info {
	char *name;
};

struct spdk_nvme_health_info_context {
	struct spdk_jsonrpc_request *request;
	struct spdk_nvme_ctrlr *ctrlr;
	struct spdk_nvme_health_information_page health_page;
};

static void
free_rpc_get_controller_health_info(struct rpc_get_controller_health_info *r)
{
	free(r->name);
}

static const struct spdk_json_object_decoder rpc_get_controller_health_info_decoders[] = {
	{"name", offsetof(struct rpc_get_controller_health_info, name), spdk_json_decode_string, true},
};

static void nvme_health_info_cleanup(struct spdk_nvme_health_info_context *context, bool response)
{
	if (response == true) {
		spdk_jsonrpc_send_error_response(context->request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Internal error.");
	}

	free(context);
}

static void
get_health_log_page_completion(void *cb_arg, const struct spdk_nvme_cpl *cpl)
{
	int i;
	char buf[128];
	struct spdk_nvme_health_info_context *context = cb_arg;
	struct spdk_jsonrpc_request *request = context->request;
	struct spdk_json_write_ctx *w;
	struct spdk_nvme_ctrlr *ctrlr = context->ctrlr;
	const struct spdk_nvme_transport_id *trid = NULL;
	const struct spdk_nvme_ctrlr_data *cdata = NULL;
	struct spdk_nvme_health_information_page *health_page = NULL;

	if (spdk_nvme_cpl_is_error(cpl)) {
		nvme_health_info_cleanup(context, true);
		SPDK_ERRLOG("get log page failed\n");
		return;
	}

	if (ctrlr == NULL) {
		nvme_health_info_cleanup(context, true);
		SPDK_ERRLOG("ctrlr is NULL\n");
		return;
	} else {
		trid = spdk_nvme_ctrlr_get_transport_id(ctrlr);
		cdata = spdk_nvme_ctrlr_get_data(ctrlr);
		health_page = &(context->health_page);
	}

	w = spdk_jsonrpc_begin_result(request);

	spdk_json_write_object_begin(w);
	snprintf(buf, sizeof(cdata->mn) + 1, "%s", cdata->mn);
	spdk_str_trim(buf);
	spdk_json_write_named_string(w, "model_number", buf);
	snprintf(buf, sizeof(cdata->sn) + 1, "%s", cdata->sn);
	spdk_str_trim(buf);
	spdk_json_write_named_string(w, "serial_number", buf);
	snprintf(buf, sizeof(cdata->fr) + 1, "%s", cdata->fr);
	spdk_str_trim(buf);
	spdk_json_write_named_string(w, "firmware_revision", buf);
	spdk_json_write_named_string(w, "traddr", trid->traddr);
	spdk_json_write_named_uint64(w, "temperature_celsius", health_page->temperature - 273);
	spdk_json_write_named_uint64(w, "available_spare_percentage", health_page->available_spare);
	spdk_json_write_named_uint64(w, "available_spare_threshold_percentage",
				     health_page->available_spare_threshold);
	spdk_json_write_named_uint64(w, "percentage_used", health_page->percentage_used);
	spdk_json_write_named_uint128(w, "data_units_read",
				      health_page->data_units_read[0], health_page->data_units_read[1]);
	spdk_json_write_named_uint128(w, "data_units_written",
				      health_page->data_units_written[0], health_page->data_units_written[1]);
	spdk_json_write_named_uint128(w, "host_read_commands",
				      health_page->host_read_commands[0], health_page->host_read_commands[1]);
	spdk_json_write_named_uint128(w, "host_write_commands",
				      health_page->host_write_commands[0], health_page->host_write_commands[1]);
	spdk_json_write_named_uint128(w, "controller_busy_time",
				      health_page->controller_busy_time[0], health_page->controller_busy_time[1]);
	spdk_json_write_named_uint128(w, "power_cycles",
				      health_page->power_cycles[0], health_page->power_cycles[1]);
	spdk_json_write_named_uint128(w, "power_on_hours",
				      health_page->power_on_hours[0], health_page->power_on_hours[1]);
	spdk_json_write_named_uint128(w, "unsafe_shutdowns",
				      health_page->unsafe_shutdowns[0], health_page->unsafe_shutdowns[1]);
	spdk_json_write_named_uint128(w, "media_errors",
				      health_page->media_errors[0], health_page->media_errors[1]);
	spdk_json_write_named_uint128(w, "num_err_log_entries",
				      health_page->num_error_info_log_entries[0], health_page->num_error_info_log_entries[1]);
	spdk_json_write_named_uint64(w, "warning_temperature_time_minutes", health_page->warning_temp_time);
	spdk_json_write_named_uint64(w, "critical_composite_temperature_time_minutes",
				     health_page->critical_temp_time);
	for (i = 0; i < 8; i++) {
		if (health_page->temp_sensor[i] != 0) {
			spdk_json_write_named_uint64(w, "temperature_sensor_celsius", health_page->temp_sensor[i] - 273);
		}
	}
	spdk_json_write_object_end(w);

	spdk_jsonrpc_end_result(request, w);
	nvme_health_info_cleanup(context, false);
}

static void
get_health_log_page(struct spdk_nvme_health_info_context *context)
{
	struct spdk_nvme_ctrlr *ctrlr = context->ctrlr;

	if (spdk_nvme_ctrlr_cmd_get_log_page(ctrlr, SPDK_NVME_LOG_HEALTH_INFORMATION,
					     SPDK_NVME_GLOBAL_NS_TAG,
					     &(context->health_page), sizeof(context->health_page), 0,
					     get_health_log_page_completion, context)) {
		nvme_health_info_cleanup(context, true);
		SPDK_ERRLOG("spdk_nvme_ctrlr_cmd_get_log_page() failed\n");
	}
}

static void
get_temperature_threshold_feature_completion(void *cb_arg, const struct spdk_nvme_cpl *cpl)
{
	struct spdk_nvme_health_info_context *context = cb_arg;

	if (spdk_nvme_cpl_is_error(cpl)) {
		nvme_health_info_cleanup(context, true);
		SPDK_ERRLOG("feature SPDK_NVME_FEAT_TEMPERATURE_THRESHOLD failed in completion\n");
	} else {
		get_health_log_page(context);
	}
}

static int
get_temperature_threshold_feature(struct spdk_nvme_health_info_context *context)
{
	struct spdk_nvme_cmd cmd = {};

	cmd.opc = SPDK_NVME_OPC_GET_FEATURES;
	cmd.cdw10 = SPDK_NVME_FEAT_TEMPERATURE_THRESHOLD;

	return spdk_nvme_ctrlr_cmd_admin_raw(context->ctrlr, &cmd, NULL, 0,
					     get_temperature_threshold_feature_completion, context);
}

static void
get_controller_health_info(struct spdk_jsonrpc_request *request, struct spdk_nvme_ctrlr *ctrlr)
{
	struct spdk_nvme_health_info_context *context;

	context = calloc(1, sizeof(struct spdk_nvme_health_info_context));
	if (!context) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Memory allocation error.");
		return;
	}

	context->request = request;
	context->ctrlr = ctrlr;

	if (get_temperature_threshold_feature(context)) {
		nvme_health_info_cleanup(context, true);
		SPDK_ERRLOG("feature SPDK_NVME_FEAT_TEMPERATURE_THRESHOLD failed to submit\n");
	}

	return;
}

static void
rpc_bdev_nvme_get_controller_health_info(struct spdk_jsonrpc_request *request,
		const struct spdk_json_val *params)
{
	struct rpc_get_controller_health_info req = {};
	struct nvme_ctrlr *nvme_ctrlr = NULL;

	if (!params) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Missing device name");

		return;
	}
	if (spdk_json_decode_object(params, rpc_get_controller_health_info_decoders,
				    SPDK_COUNTOF(rpc_get_controller_health_info_decoders), &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		free_rpc_get_controller_health_info(&req);
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Invalid parameters");

		return;
	}

	nvme_ctrlr = nvme_ctrlr_get_by_name(req.name);

	if (!nvme_ctrlr) {
		SPDK_ERRLOG("nvme ctrlr name '%s' does not exist\n", req.name);
		free_rpc_get_controller_health_info(&req);
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Device not found");
		return;
	}

	get_controller_health_info(request, nvme_ctrlr->ctrlr);
	free_rpc_get_controller_health_info(&req);

	return;
}
SPDK_RPC_REGISTER("bdev_nvme_get_controller_health_info",
		  rpc_bdev_nvme_get_controller_health_info, SPDK_RPC_RUNTIME)

struct rpc_bdev_nvme_start_discovery {
	char *name;
	char *trtype;
	char *adrfam;
	char *traddr;
	char *trsvcid;
	char *hostnqn;
	bool wait_for_attach;
	struct spdk_nvme_ctrlr_opts opts;
	struct nvme_ctrlr_opts bdev_opts;
};

static void
free_rpc_bdev_nvme_start_discovery(struct rpc_bdev_nvme_start_discovery *req)
{
	free(req->name);
	free(req->trtype);
	free(req->adrfam);
	free(req->traddr);
	free(req->trsvcid);
	free(req->hostnqn);
}

static const struct spdk_json_object_decoder rpc_bdev_nvme_start_discovery_decoders[] = {
	{"name", offsetof(struct rpc_bdev_nvme_start_discovery, name), spdk_json_decode_string},
	{"trtype", offsetof(struct rpc_bdev_nvme_start_discovery, trtype), spdk_json_decode_string},
	{"traddr", offsetof(struct rpc_bdev_nvme_start_discovery, traddr), spdk_json_decode_string},
	{"adrfam", offsetof(struct rpc_bdev_nvme_start_discovery, adrfam), spdk_json_decode_string, true},
	{"trsvcid", offsetof(struct rpc_bdev_nvme_start_discovery, trsvcid), spdk_json_decode_string, true},
	{"hostnqn", offsetof(struct rpc_bdev_nvme_start_discovery, hostnqn), spdk_json_decode_string, true},
	{"wait_for_attach", offsetof(struct rpc_bdev_nvme_start_discovery, wait_for_attach), spdk_json_decode_bool, true},
	{"ctrlr_loss_timeout_sec", offsetof(struct rpc_bdev_nvme_start_discovery, bdev_opts.ctrlr_loss_timeout_sec), spdk_json_decode_int32, true},
	{"reconnect_delay_sec", offsetof(struct rpc_bdev_nvme_start_discovery, bdev_opts.reconnect_delay_sec), spdk_json_decode_uint32, true},
	{"fast_io_fail_timeout_sec", offsetof(struct rpc_bdev_nvme_start_discovery, bdev_opts.fast_io_fail_timeout_sec), spdk_json_decode_uint32, true},
};

struct rpc_bdev_nvme_start_discovery_ctx {
	struct rpc_bdev_nvme_start_discovery req;
	struct spdk_jsonrpc_request *request;
};

static void
rpc_bdev_nvme_start_discovery_done(void *ctx)
{
	struct spdk_jsonrpc_request *request = ctx;

	spdk_jsonrpc_send_bool_response(request, true);
}

static void
rpc_bdev_nvme_start_discovery(struct spdk_jsonrpc_request *request,
			      const struct spdk_json_val *params)
{
	struct rpc_bdev_nvme_start_discovery_ctx *ctx;
	struct spdk_nvme_transport_id trid = {};
	size_t len, maxlen;
	int rc;
	spdk_bdev_nvme_start_discovery_fn cb_fn;
	void *cb_ctx;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		spdk_jsonrpc_send_error_response(request, -ENOMEM, spdk_strerror(ENOMEM));
		return;
	}

	spdk_nvme_ctrlr_get_default_ctrlr_opts(&ctx->req.opts, sizeof(ctx->req.opts));

	if (spdk_json_decode_object(params, rpc_bdev_nvme_start_discovery_decoders,
				    SPDK_COUNTOF(rpc_bdev_nvme_start_discovery_decoders),
				    &ctx->req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	/* Parse trstring */
	rc = spdk_nvme_transport_id_populate_trstring(&trid, ctx->req.trtype);
	if (rc < 0) {
		SPDK_ERRLOG("Failed to parse trtype: %s\n", ctx->req.trtype);
		spdk_jsonrpc_send_error_response_fmt(request, -EINVAL, "Failed to parse trtype: %s",
						     ctx->req.trtype);
		goto cleanup;
	}

	/* Parse trtype */
	rc = spdk_nvme_transport_id_parse_trtype(&trid.trtype, ctx->req.trtype);
	assert(rc == 0);

	/* Parse traddr */
	maxlen = sizeof(trid.traddr);
	len = strnlen(ctx->req.traddr, maxlen);
	if (len == maxlen) {
		spdk_jsonrpc_send_error_response_fmt(request, -EINVAL, "traddr too long: %s",
						     ctx->req.traddr);
		goto cleanup;
	}
	memcpy(trid.traddr, ctx->req.traddr, len + 1);

	/* Parse adrfam */
	if (ctx->req.adrfam) {
		rc = spdk_nvme_transport_id_parse_adrfam(&trid.adrfam, ctx->req.adrfam);
		if (rc < 0) {
			SPDK_ERRLOG("Failed to parse adrfam: %s\n", ctx->req.adrfam);
			spdk_jsonrpc_send_error_response_fmt(request, -EINVAL, "Failed to parse adrfam: %s",
							     ctx->req.adrfam);
			goto cleanup;
		}
	}

	/* Parse trsvcid */
	if (ctx->req.trsvcid) {
		maxlen = sizeof(trid.trsvcid);
		len = strnlen(ctx->req.trsvcid, maxlen);
		if (len == maxlen) {
			spdk_jsonrpc_send_error_response_fmt(request, -EINVAL, "trsvcid too long: %s",
							     ctx->req.trsvcid);
			goto cleanup;
		}
		memcpy(trid.trsvcid, ctx->req.trsvcid, len + 1);
	}

	if (ctx->req.hostnqn) {
		snprintf(ctx->req.opts.hostnqn, sizeof(ctx->req.opts.hostnqn), "%s",
			 ctx->req.hostnqn);
	}

	ctx->request = request;
	cb_fn = ctx->req.wait_for_attach ? rpc_bdev_nvme_start_discovery_done : NULL;
	cb_ctx = ctx->req.wait_for_attach ? request : NULL;
	rc = bdev_nvme_start_discovery(&trid, ctx->req.name, &ctx->req.opts, &ctx->req.bdev_opts,
				       cb_fn, cb_ctx);
	if (rc) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
	} else if (!ctx->req.wait_for_attach) {
		rpc_bdev_nvme_start_discovery_done(request);
	}

cleanup:
	free_rpc_bdev_nvme_start_discovery(&ctx->req);
	free(ctx);
}
SPDK_RPC_REGISTER("bdev_nvme_start_discovery", rpc_bdev_nvme_start_discovery,
		  SPDK_RPC_RUNTIME)

struct rpc_bdev_nvme_stop_discovery {
	char *name;
};

static const struct spdk_json_object_decoder rpc_bdev_nvme_stop_discovery_decoders[] = {
	{"name", offsetof(struct rpc_bdev_nvme_stop_discovery, name), spdk_json_decode_string},
};

struct rpc_bdev_nvme_stop_discovery_ctx {
	struct rpc_bdev_nvme_stop_discovery req;
	struct spdk_jsonrpc_request *request;
};

static void
rpc_bdev_nvme_stop_discovery_done(void *cb_ctx)
{
	struct rpc_bdev_nvme_stop_discovery_ctx *ctx = cb_ctx;

	spdk_jsonrpc_send_bool_response(ctx->request, true);
	free(ctx->req.name);
	free(ctx);
}

static void
rpc_bdev_nvme_stop_discovery(struct spdk_jsonrpc_request *request,
			     const struct spdk_json_val *params)
{
	struct rpc_bdev_nvme_stop_discovery_ctx *ctx;
	int rc;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		spdk_jsonrpc_send_error_response(request, -ENOMEM, spdk_strerror(ENOMEM));
		return;
	}

	if (spdk_json_decode_object(params, rpc_bdev_nvme_stop_discovery_decoders,
				    SPDK_COUNTOF(rpc_bdev_nvme_stop_discovery_decoders),
				    &ctx->req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	ctx->request = request;
	rc = bdev_nvme_stop_discovery(ctx->req.name, rpc_bdev_nvme_stop_discovery_done, ctx);
	if (rc) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	return;

cleanup:
	free(ctx->req.name);
	free(ctx);
}
SPDK_RPC_REGISTER("bdev_nvme_stop_discovery", rpc_bdev_nvme_stop_discovery,
		  SPDK_RPC_RUNTIME)

enum error_injection_cmd_type {
	NVME_ADMIN_CMD = 1,
	NVME_IO_CMD,
};

struct rpc_add_error_injection {
	char *name;
	enum error_injection_cmd_type cmd_type;
	uint8_t opc;
	bool do_not_submit;
	uint64_t timeout_in_us;
	uint32_t err_count;
	uint8_t sct;
	uint8_t sc;
};

static void
free_rpc_add_error_injection(struct rpc_add_error_injection *req)
{
	free(req->name);
}

static int
rpc_error_injection_decode_cmd_type(const struct spdk_json_val *val, void *out)
{
	int *cmd_type = out;

	if (spdk_json_strequal(val, "admin")) {
		*cmd_type = NVME_ADMIN_CMD;
	} else if (spdk_json_strequal(val, "io")) {
		*cmd_type = NVME_IO_CMD;
	} else {
		SPDK_ERRLOG("Invalid parameter value: cmd_type\n");
		return -EINVAL;
	}

	return 0;
}

static const struct spdk_json_object_decoder rpc_add_error_injection_decoders[] = {
	{ "name", offsetof(struct rpc_add_error_injection, name), spdk_json_decode_string },
	{ "cmd_type", offsetof(struct rpc_add_error_injection, cmd_type), rpc_error_injection_decode_cmd_type },
	{ "opc", offsetof(struct rpc_add_error_injection, opc), spdk_json_decode_uint8 },
	{ "do_not_submit", offsetof(struct rpc_add_error_injection, do_not_submit), spdk_json_decode_bool, true },
	{ "timeout_in_us", offsetof(struct rpc_add_error_injection, timeout_in_us), spdk_json_decode_uint64, true },
	{ "err_count", offsetof(struct rpc_add_error_injection, err_count), spdk_json_decode_uint32, true },
	{ "sct", offsetof(struct rpc_add_error_injection, sct), spdk_json_decode_uint8, true},
	{ "sc", offsetof(struct rpc_add_error_injection, sc), spdk_json_decode_uint8, true},
};

struct rpc_add_error_injection_ctx {
	struct spdk_jsonrpc_request *request;
	struct rpc_add_error_injection rpc;
};

static void
rpc_add_error_injection_done(struct spdk_io_channel_iter *i, int status)
{
	struct rpc_add_error_injection_ctx *ctx = spdk_io_channel_iter_get_ctx(i);

	if (status) {
		spdk_jsonrpc_send_error_response(ctx->request, status,
						 "Failed to add the error injection.");
	} else {
		spdk_jsonrpc_send_bool_response(ctx->request, true);
	}

	free_rpc_add_error_injection(&ctx->rpc);
	free(ctx);
}

static void
rpc_add_error_injection_per_channel(struct spdk_io_channel_iter *i)
{
	struct spdk_io_channel *ch = spdk_io_channel_iter_get_channel(i);
	struct rpc_add_error_injection_ctx *ctx = spdk_io_channel_iter_get_ctx(i);
	struct nvme_ctrlr_channel *ctrlr_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_nvme_qpair *qpair = ctrlr_ch->qpair->qpair;
	struct spdk_nvme_ctrlr *ctrlr = ctrlr_ch->qpair->ctrlr->ctrlr;
	int rc = 0;

	if (qpair != NULL) {
		rc = spdk_nvme_qpair_add_cmd_error_injection(ctrlr, qpair, ctx->rpc.opc,
				ctx->rpc.do_not_submit, ctx->rpc.timeout_in_us, ctx->rpc.err_count,
				ctx->rpc.sct, ctx->rpc.sc);
	}

	spdk_for_each_channel_continue(i, rc);
}

static void
rpc_bdev_nvme_add_error_injection(
	struct spdk_jsonrpc_request *request,
	const struct spdk_json_val *params)
{
	struct rpc_add_error_injection_ctx *ctx;
	struct nvme_ctrlr *nvme_ctrlr;
	int rc;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		spdk_jsonrpc_send_error_response(request, -ENOMEM, spdk_strerror(ENOMEM));
		return;
	}
	ctx->rpc.err_count = 1;
	ctx->request = request;

	if (spdk_json_decode_object(params,
				    rpc_add_error_injection_decoders,
				    SPDK_COUNTOF(rpc_add_error_injection_decoders),
				    &ctx->rpc)) {
		spdk_jsonrpc_send_error_response(request, -EINVAL,
						 "Failed to parse the request");
		goto cleanup;
	}

	nvme_ctrlr = nvme_ctrlr_get_by_name(ctx->rpc.name);
	if (nvme_ctrlr == NULL) {
		SPDK_ERRLOG("No controller with specified name was found.\n");
		spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
		goto cleanup;
	}

	if (ctx->rpc.cmd_type == NVME_IO_CMD) {
		spdk_for_each_channel(nvme_ctrlr,
				      rpc_add_error_injection_per_channel,
				      ctx,
				      rpc_add_error_injection_done);

		return;
	} else {
		rc = spdk_nvme_qpair_add_cmd_error_injection(nvme_ctrlr->ctrlr, NULL, ctx->rpc.opc,
				ctx->rpc.do_not_submit, ctx->rpc.timeout_in_us, ctx->rpc.err_count,
				ctx->rpc.sct, ctx->rpc.sc);
		if (rc) {
			spdk_jsonrpc_send_error_response(request, -rc,
							 "Failed to add the error injection");
		} else {
			spdk_jsonrpc_send_bool_response(ctx->request, true);
		}
	}

cleanup:
	free_rpc_add_error_injection(&ctx->rpc);
	free(ctx);
}
SPDK_RPC_REGISTER("bdev_nvme_add_error_injection", rpc_bdev_nvme_add_error_injection,
		  SPDK_RPC_RUNTIME)

struct rpc_remove_error_injection {
	char *name;
	enum error_injection_cmd_type cmd_type;
	uint8_t opc;
};

static void
free_rpc_remove_error_injection(struct rpc_remove_error_injection *req)
{
	free(req->name);
}

static const struct spdk_json_object_decoder rpc_remove_error_injection_decoders[] = {
	{ "name", offsetof(struct rpc_remove_error_injection, name), spdk_json_decode_string },
	{ "cmd_type", offsetof(struct rpc_remove_error_injection, cmd_type), rpc_error_injection_decode_cmd_type },
	{ "opc", offsetof(struct rpc_remove_error_injection, opc), spdk_json_decode_uint8 },
};

struct rpc_remove_error_injection_ctx {
	struct spdk_jsonrpc_request *request;
	struct rpc_remove_error_injection rpc;
};

static void
rpc_remove_error_injection_done(struct spdk_io_channel_iter *i, int status)
{
	struct rpc_remove_error_injection_ctx *ctx = spdk_io_channel_iter_get_ctx(i);

	if (status) {
		spdk_jsonrpc_send_error_response(ctx->request, status,
						 "Failed to remove the error injection.");
	} else {
		spdk_jsonrpc_send_bool_response(ctx->request, true);
	}

	free_rpc_remove_error_injection(&ctx->rpc);
	free(ctx);
}

static void
rpc_remove_error_injection_per_channel(struct spdk_io_channel_iter *i)
{
	struct spdk_io_channel *ch = spdk_io_channel_iter_get_channel(i);
	struct rpc_remove_error_injection_ctx *ctx = spdk_io_channel_iter_get_ctx(i);
	struct nvme_ctrlr_channel *ctrlr_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_nvme_qpair *qpair = ctrlr_ch->qpair->qpair;
	struct spdk_nvme_ctrlr *ctrlr = ctrlr_ch->qpair->ctrlr->ctrlr;

	if (qpair != NULL) {
		spdk_nvme_qpair_remove_cmd_error_injection(ctrlr, qpair, ctx->rpc.opc);
	}

	spdk_for_each_channel_continue(i, 0);
}

static void
rpc_bdev_nvme_remove_error_injection(struct spdk_jsonrpc_request *request,
				     const struct spdk_json_val *params)
{
	struct rpc_remove_error_injection_ctx *ctx;
	struct nvme_ctrlr *nvme_ctrlr;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		spdk_jsonrpc_send_error_response(request, -ENOMEM, spdk_strerror(ENOMEM));
		return;
	}
	ctx->request = request;

	if (spdk_json_decode_object(params,
				    rpc_remove_error_injection_decoders,
				    SPDK_COUNTOF(rpc_remove_error_injection_decoders),
				    &ctx->rpc)) {
		spdk_jsonrpc_send_error_response(request, -EINVAL,
						 "Failed to parse the request");
		goto cleanup;
	}

	nvme_ctrlr = nvme_ctrlr_get_by_name(ctx->rpc.name);
	if (nvme_ctrlr == NULL) {
		SPDK_ERRLOG("No controller with specified name was found.\n");
		spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
		goto cleanup;
	}

	if (ctx->rpc.cmd_type == NVME_IO_CMD) {
		spdk_for_each_channel(nvme_ctrlr,
				      rpc_remove_error_injection_per_channel,
				      ctx,
				      rpc_remove_error_injection_done);
		return;
	} else {
		spdk_nvme_qpair_remove_cmd_error_injection(nvme_ctrlr->ctrlr, NULL, ctx->rpc.opc);
		spdk_jsonrpc_send_bool_response(ctx->request, true);
	}

cleanup:
	free_rpc_remove_error_injection(&ctx->rpc);
	free(ctx);
}
SPDK_RPC_REGISTER("bdev_nvme_remove_error_injection", rpc_bdev_nvme_remove_error_injection,
		  SPDK_RPC_RUNTIME)
