/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
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
#include "spdk/bdev.h"
#include "spdk/conf.h"
#include "spdk/env.h"
#include "spdk/io_channel.h"
#include "spdk/json.h"
#include "spdk/string.h"
#include "spdk/likely.h"
#include "spdk/util.h"
#include "spdk/ftl.h"
#include "spdk_internal/log.h"

#include "bdev_ftl.h"

#define FTL_COMPLETION_RING_SIZE 4096

struct ftl_bdev_ctrlr {
	struct spdk_nvme_ctrlr		*ctrlr;

	struct spdk_nvme_transport_id	trid;

	size_t				ref_cnt;

	LIST_ENTRY(ftl_bdev_ctrlr)	list_entry;
};

struct ftl_bdev {
	struct spdk_bdev		bdev;

	struct ftl_bdev_ctrlr		*ctrlr;

	struct spdk_ftl_dev		*dev;

	ftl_bdev_init_fn		init_cb;

	void				*init_arg;

	LIST_ENTRY(ftl_bdev)		list_entry;
};

struct ftl_io_channel {
	struct spdk_ftl_dev		*dev;

	struct spdk_poller		*poller;

#define FTL_MAX_COMPLETIONS 64
	struct ftl_bdev_io		*io[FTL_MAX_COMPLETIONS];

	/* Completion ring */
	struct spdk_ring		*ring;

	struct spdk_io_channel		*ioch;
};

struct ftl_bdev_io {
	struct ftl_bdev			*bdev;

	struct spdk_ring		*ring;

	int				status;

	struct spdk_thread		*orig_thread;
};

typedef void (*bdev_ftl_finish_fn)(void);

static LIST_HEAD(, ftl_bdev)		g_ftl_bdevs = LIST_HEAD_INITIALIZER(g_ftl_bdevs);
static LIST_HEAD(, ftl_bdev_ctrlr)	g_ftl_bdev_ctrlrs =
	LIST_HEAD_INITIALIZER(g_ftl_bdev_ctrlrs);
static bdev_ftl_finish_fn		g_finish_cb;
static size_t				g_num_conf_bdevs;
static size_t				g_num_init_bdevs;
static pthread_mutex_t			g_ftl_bdev_lock;

static int bdev_ftl_initialize(void);
static void bdev_ftl_finish(void);

static int
bdev_ftl_get_ctx_size(void)
{
	return sizeof(struct ftl_bdev_io);
}

static struct spdk_bdev_module g_ftl_if = {
	.name		= "ftl",
	.async_init	= true,
	.async_fini	= true,
	.module_init	= bdev_ftl_initialize,
	.module_fini	= bdev_ftl_finish,
	.get_ctx_size	= bdev_ftl_get_ctx_size,
};

SPDK_BDEV_MODULE_REGISTER(ftl, &g_ftl_if)

static struct ftl_bdev_ctrlr *
bdev_ftl_ctrlr_find(const struct spdk_nvme_transport_id *trid)
{
	struct ftl_bdev_ctrlr *ftl_ctrlr = NULL;

	LIST_FOREACH(ftl_ctrlr, &g_ftl_bdev_ctrlrs, list_entry) {
		if (!spdk_nvme_transport_id_compare(&ftl_ctrlr->trid, trid)) {
			break;
		}
	}

	return ftl_ctrlr;
}

static struct ftl_bdev_ctrlr *
bdev_ftl_add_ctrlr(struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_transport_id *trid)
{
	struct ftl_bdev_ctrlr *ftl_ctrlr = NULL;

	pthread_mutex_lock(&g_ftl_bdev_lock);

	ftl_ctrlr = bdev_ftl_ctrlr_find(trid);
	if (ftl_ctrlr) {
		ftl_ctrlr->ref_cnt++;
	} else {
		ftl_ctrlr = calloc(1, sizeof(*ftl_ctrlr));
		if (!ftl_ctrlr) {
			goto out;
		}

		ftl_ctrlr->ctrlr = ctrlr;
		ftl_ctrlr->trid = *trid;
		ftl_ctrlr->ref_cnt = 1;

		LIST_INSERT_HEAD(&g_ftl_bdev_ctrlrs, ftl_ctrlr, list_entry);
	}
out:
	pthread_mutex_unlock(&g_ftl_bdev_lock);
	return ftl_ctrlr;
}

static void
bdev_ftl_remove_ctrlr(struct ftl_bdev_ctrlr *ctrlr)
{
	pthread_mutex_lock(&g_ftl_bdev_lock);

	if (--ctrlr->ref_cnt == 0) {
		if (spdk_nvme_detach(ctrlr->ctrlr)) {
			SPDK_ERRLOG("Failed to detach the controller\n");
			goto out;
		}

		LIST_REMOVE(ctrlr, list_entry);
		free(ctrlr);
	}
out:
	pthread_mutex_unlock(&g_ftl_bdev_lock);
}

static void
bdev_ftl_free_cb(void *ctx, int status)
{
	struct ftl_bdev *ftl_bdev = ctx;
	bool finish_done;

	pthread_mutex_lock(&g_ftl_bdev_lock);
	LIST_REMOVE(ftl_bdev, list_entry);
	finish_done = LIST_EMPTY(&g_ftl_bdevs);
	pthread_mutex_unlock(&g_ftl_bdev_lock);

	spdk_io_device_unregister(ftl_bdev, NULL);

	bdev_ftl_remove_ctrlr(ftl_bdev->ctrlr);

	spdk_bdev_destruct_done(&ftl_bdev->bdev, status);
	free(ftl_bdev->bdev.name);
	free(ftl_bdev);

	if (finish_done && g_finish_cb) {
		g_finish_cb();
	}
}

static int
bdev_ftl_destruct(void *ctx)
{
	struct ftl_bdev *ftl_bdev = ctx;
	spdk_ftl_dev_free(ftl_bdev->dev, bdev_ftl_free_cb, ftl_bdev);

	/* return 1 to indicate that the destruction is asynchronous */
	return 1;
}

static void
bdev_ftl_complete_io(struct ftl_bdev_io *io, int rc)
{
	enum spdk_bdev_io_status status;

	switch (rc) {
	case 0:
		status = SPDK_BDEV_IO_STATUS_SUCCESS;
		break;
	case -ENOMEM:
		status = SPDK_BDEV_IO_STATUS_NOMEM;
		break;
	default:
		status = SPDK_BDEV_IO_STATUS_FAILED;
		break;
	}

	spdk_bdev_io_complete(spdk_bdev_io_from_ctx(io), status);
}

static void
bdev_ftl_cb(void *arg, int status)
{
	struct ftl_bdev_io *io = arg;
	size_t cnt __attribute__((unused));

	io->status = status;

	cnt = spdk_ring_enqueue(io->ring, (void **)&io, 1);
	assert(cnt == 1);
}

static int
bdev_ftl_fill_bio(struct ftl_bdev *ftl_bdev, struct spdk_io_channel *ch,
		  struct ftl_bdev_io *io)
{
	struct ftl_io_channel *ioch = spdk_io_channel_get_ctx(ch);

	memset(io, 0, sizeof(*io));

	io->orig_thread = spdk_io_channel_get_thread(ch);
	io->status = SPDK_BDEV_IO_STATUS_SUCCESS;
	io->ring = ioch->ring;
	io->bdev = ftl_bdev;
	return 0;
}

static int
bdev_ftl_readv(struct ftl_bdev *ftl_bdev, struct spdk_io_channel *ch,
	       struct ftl_bdev_io *io)
{
	struct spdk_bdev_io *bio;
	struct ftl_io_channel *ioch = spdk_io_channel_get_ctx(ch);
	int rc;

	bio = spdk_bdev_io_from_ctx(io);

	rc = bdev_ftl_fill_bio(ftl_bdev, ch, io);
	if (rc) {
		return rc;
	}

	return spdk_ftl_read(ftl_bdev->dev,
			     ioch->ioch,
			     bio->u.bdev.offset_blocks,
			     bio->u.bdev.num_blocks,
			     bio->u.bdev.iovs, bio->u.bdev.iovcnt, bdev_ftl_cb, io);
}

static int
bdev_ftl_writev(struct ftl_bdev *ftl_bdev, struct spdk_io_channel *ch,
		struct ftl_bdev_io *io)
{
	struct spdk_bdev_io *bio;
	struct ftl_io_channel *ioch;
	int rc;

	bio = spdk_bdev_io_from_ctx(io);
	ioch = spdk_io_channel_get_ctx(ch);

	rc = bdev_ftl_fill_bio(ftl_bdev, ch, io);
	if (rc) {
		return rc;
	}

	return spdk_ftl_write(ftl_bdev->dev,
			      ioch->ioch,
			      bio->u.bdev.offset_blocks,
			      bio->u.bdev.num_blocks,
			      bio->u.bdev.iovs,
			      bio->u.bdev.iovcnt, bdev_ftl_cb, io);
}

static void
bdev_ftl_get_buf_cb(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io,
		    bool success)
{
	if (!success) {
		bdev_ftl_complete_io((struct ftl_bdev_io *)bdev_io->driver_ctx,
				     SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	int rc = bdev_ftl_readv((struct ftl_bdev *)bdev_io->bdev->ctxt,
				ch, (struct ftl_bdev_io *)bdev_io->driver_ctx);

	if (spdk_unlikely(rc != 0)) {
		bdev_ftl_complete_io((struct ftl_bdev_io *)bdev_io->driver_ctx, rc);
	}
}

static int
bdev_ftl_flush(struct ftl_bdev *ftl_bdev, struct spdk_io_channel *ch, struct ftl_bdev_io *io)
{
	int rc;

	rc = bdev_ftl_fill_bio(ftl_bdev, ch, io);
	if (rc) {
		return rc;
	}

	return spdk_ftl_flush(ftl_bdev->dev, bdev_ftl_cb, io);
}

static int
_bdev_ftl_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct ftl_bdev *ftl_bdev = (struct ftl_bdev *)bdev_io->bdev->ctxt;

	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		spdk_bdev_io_get_buf(bdev_io, bdev_ftl_get_buf_cb,
				     bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen);
		return 0;

	case SPDK_BDEV_IO_TYPE_WRITE:
		return bdev_ftl_writev(ftl_bdev, ch, (struct ftl_bdev_io *)bdev_io->driver_ctx);

	case SPDK_BDEV_IO_TYPE_FLUSH:
		return bdev_ftl_flush(ftl_bdev, ch, (struct ftl_bdev_io *)bdev_io->driver_ctx);

	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
	case SPDK_BDEV_IO_TYPE_RESET:
	case SPDK_BDEV_IO_TYPE_UNMAP:
	default:
		return -ENOTSUP;
		break;
	}
}

static void
bdev_ftl_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	int rc = _bdev_ftl_submit_request(ch, bdev_io);

	if (spdk_unlikely(rc != 0)) {
		bdev_ftl_complete_io((struct ftl_bdev_io *)bdev_io->driver_ctx, rc);
	}
}

static bool
bdev_ftl_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type)
{
	switch (io_type) {
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_WRITE:
	case SPDK_BDEV_IO_TYPE_FLUSH:
		return true;
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
	case SPDK_BDEV_IO_TYPE_RESET:
	case SPDK_BDEV_IO_TYPE_UNMAP:
	default:
		return false;
	}
}

static struct spdk_io_channel *
bdev_ftl_get_io_channel(void *ctx)
{
	struct ftl_bdev *ftl_bdev = ctx;

	return spdk_get_io_channel(ftl_bdev);
}

static void
bdev_ftl_write_config_json(struct spdk_bdev *bdev, struct spdk_json_write_ctx *w)
{
	struct ftl_bdev *ftl_bdev = bdev->ctxt;
	struct spdk_ftl_attrs attrs;
	const char *trtype_str;
	char uuid[SPDK_UUID_STRING_LEN];

	spdk_ftl_dev_get_attrs(ftl_bdev->dev, &attrs);

	spdk_json_write_object_begin(w);

	spdk_json_write_named_string(w, "method", "construct_ftl_bdev");

	spdk_json_write_named_object_begin(w, "params");
	spdk_json_write_named_string(w, "name", ftl_bdev->bdev.name);

	trtype_str = spdk_nvme_transport_id_trtype_str(ftl_bdev->ctrlr->trid.trtype);
	if (trtype_str) {
		spdk_json_write_named_string(w, "trtype", trtype_str);
	}
	spdk_json_write_named_string(w, "traddr", ftl_bdev->ctrlr->trid.traddr);

	spdk_json_write_named_string_fmt(w, "punits", "%d-%d", attrs.range.begin, attrs.range.end);

	spdk_uuid_fmt_lower(uuid, sizeof(uuid), &attrs.uuid);
	spdk_json_write_named_string(w, "uuid", uuid);

	spdk_json_write_object_end(w);

	spdk_json_write_object_end(w);
}

static const struct spdk_bdev_fn_table ftl_fn_table = {
	.destruct		= bdev_ftl_destruct,
	.submit_request		= bdev_ftl_submit_request,
	.io_type_supported	= bdev_ftl_io_type_supported,
	.get_io_channel		= bdev_ftl_get_io_channel,
	.write_config_json	= bdev_ftl_write_config_json,
};

int
bdev_ftl_parse_punits(struct spdk_ftl_punit_range *range, const char *range_string)
{
	regex_t range_regex;
	regmatch_t range_match;
	unsigned long begin = 0, end = 0;
	char *str_ptr;
	int rc = -1;

	if (regcomp(&range_regex, "\\b[[:digit:]]+-[[:digit:]]+\\b", REG_EXTENDED)) {
		SPDK_ERRLOG("Regex init error\n");
		return -1;
	}

	if (regexec(&range_regex, range_string, 1, &range_match, 0)) {
		SPDK_WARNLOG("Invalid range\n");
		goto out;
	}

	errno = 0;
	begin = strtoul(range_string + range_match.rm_so, &str_ptr, 10);
	if ((begin == ULONG_MAX && errno == ERANGE) || (begin == 0 && errno == EINVAL)) {
		SPDK_WARNLOG("Invalid range '%s'\n", range_string);
		goto out;
	}

	errno = 0;
	/* +1 to skip the '-' delimiter */
	end = strtoul(str_ptr + 1, NULL, 10);
	if ((end == ULONG_MAX && errno == ERANGE) || (end == 0 && errno == EINVAL)) {
		SPDK_WARNLOG("Invalid range '%s'\n", range_string);
		goto out;
	}

	if (begin > UINT_MAX || end > UINT_MAX) {
		SPDK_WARNLOG("Invalid range '%s'\n", range_string);
		goto out;
	}

	if (begin > end) {
		SPDK_WARNLOG("Invalid range '%s'\n", range_string);
		goto out;
	}

	range->begin = (unsigned int)begin;
	range->end = (unsigned int)end;

	rc = 0;
out:
	regfree(&range_regex);
	return rc;
}

static int
bdev_ftl_read_bdev_config(struct spdk_conf_section *sp,
			  struct ftl_bdev_init_opts *opts,
			  size_t *num_bdevs)
{
	const char *val;
	size_t i;
	int rc = 0;

	*num_bdevs = 0;

	for (i = 0; i < FTL_MAX_BDEVS; i++, opts++) {
		val = spdk_conf_section_get_nmval(sp, "TransportID", i, 0);
		if (!val) {
			break;
		}

		rc = spdk_nvme_transport_id_parse(&opts->trid, val);
		if (rc < 0) {
			SPDK_ERRLOG("Unable to parse TransportID: %s\n", val);
			rc = -1;
			break;
		}

		if (opts->trid.trtype != SPDK_NVME_TRANSPORT_PCIE) {
			SPDK_ERRLOG("Unsupported transport type\n");
			continue;
		}

		opts->mode = SPDK_FTL_MODE_CREATE;

		val = spdk_conf_section_get_nmval(sp, "TransportID", i, 1);
		if (!val) {
			SPDK_ERRLOG("No name provided for TransportID\n");
			rc = -1;
			break;
		}
		opts->name = val;

		val = spdk_conf_section_get_nmval(sp, "TransportID", i, 2);
		if (!val) {
			SPDK_ERRLOG("No punit range provided for TransportID\n");
			rc = -1;
			break;
		}

		if (bdev_ftl_parse_punits(&opts->range, val)) {
			SPDK_ERRLOG("Invalid punit range\n");
			rc = -1;
			break;
		}

		val = spdk_conf_section_get_nmval(sp, "TransportID", i, 3);
		if (!val) {
			continue;
		}

		rc = spdk_uuid_parse(&opts->uuid, val);
		if (rc < 0) {
			SPDK_ERRLOG("Failed to parse uuid: %s\n", val);
			rc = -1;
			break;
		}

		opts->mode = 0;
	}

	if (!rc) {
		*num_bdevs = i;
	}

	return rc;
}

static int
bdev_ftl_poll(void *arg)
{
	struct ftl_io_channel *ch = arg;
	size_t cnt, i;

	cnt = spdk_ring_dequeue(ch->ring, (void **)&ch->io, FTL_MAX_COMPLETIONS);

	for (i = 0; i < cnt; ++i) {
		bdev_ftl_complete_io(ch->io[i], ch->io[i]->status);
	}

	return cnt;
}

static int
bdev_ftl_io_channel_create_cb(void *io_device, void *ctx)
{
	struct ftl_io_channel *ch = ctx;
	struct ftl_bdev *ftl_bdev = (struct ftl_bdev *)io_device;

	ch->dev = ftl_bdev->dev;
	ch->ring = spdk_ring_create(SPDK_RING_TYPE_MP_SC, FTL_COMPLETION_RING_SIZE,
				    SPDK_ENV_SOCKET_ID_ANY);

	if (!ch->ring) {
		return -ENOMEM;
	}

	ch->poller = spdk_poller_register(bdev_ftl_poll, ch, 0);
	if (!ch->poller) {
		spdk_ring_free(ch->ring);
		return -ENOMEM;
	}

	ch->ioch = spdk_get_io_channel(ftl_bdev->dev);

	return 0;
}

static void
bdev_ftl_io_channel_destroy_cb(void *io_device, void *ctx_buf)
{
	struct ftl_io_channel *ch = ctx_buf;

	spdk_ring_free(ch->ring);
	spdk_poller_unregister(&ch->poller);
	spdk_put_io_channel(ch->ioch);
}

static void
bdev_ftl_create_cb(struct spdk_ftl_dev *dev, void *ctx, int status)
{
	struct ftl_bdev		*ftl_bdev = ctx;
	struct ftl_bdev_info	info = {};
	struct spdk_ftl_attrs	attrs;
	ftl_bdev_init_fn	init_cb = ftl_bdev->init_cb;
	void			*init_arg = ftl_bdev->init_arg;
	int			rc = -ENODEV;

	if (status) {
		SPDK_ERRLOG("Failed to create FTL device (%d)\n", status);
		rc = status;
		goto error_dev;
	}

	spdk_ftl_dev_get_attrs(dev, &attrs);

	ftl_bdev->dev = dev;
	ftl_bdev->bdev.product_name = "FTL disk";
	ftl_bdev->bdev.write_cache = 0;
	ftl_bdev->bdev.blocklen = attrs.lbk_size;
	ftl_bdev->bdev.blockcnt = attrs.lbk_cnt;
	/* TODO: Investigate why nbd test are failing without this alignment */
	ftl_bdev->bdev.required_alignment = spdk_u32log2(attrs.lbk_size);
	ftl_bdev->bdev.uuid = attrs.uuid;

	SPDK_DEBUGLOG(SPDK_LOG_BDEV_FTL, "Creating bdev %s:\n", ftl_bdev->bdev.name);
	SPDK_DEBUGLOG(SPDK_LOG_BDEV_FTL, "\tblock_len:\t%zu\n", attrs.lbk_size);
	SPDK_DEBUGLOG(SPDK_LOG_BDEV_FTL, "\tblock_cnt:\t%"PRIu64"\n", attrs.lbk_cnt);
	SPDK_DEBUGLOG(SPDK_LOG_BDEV_FTL, "\tpunits:\t\t%u-%u\n", attrs.range.begin,
		      attrs.range.end);

	ftl_bdev->bdev.ctxt = ftl_bdev;
	ftl_bdev->bdev.fn_table = &ftl_fn_table;
	ftl_bdev->bdev.module = &g_ftl_if;

	spdk_io_device_register(ftl_bdev, bdev_ftl_io_channel_create_cb,
				bdev_ftl_io_channel_destroy_cb,
				sizeof(struct ftl_io_channel),
				ftl_bdev->bdev.name);

	if (spdk_bdev_register(&ftl_bdev->bdev)) {
		goto error_unregister;
	}

	info.name = ftl_bdev->bdev.name;
	info.uuid = ftl_bdev->bdev.uuid;

	pthread_mutex_lock(&g_ftl_bdev_lock);
	LIST_INSERT_HEAD(&g_ftl_bdevs, ftl_bdev, list_entry);
	pthread_mutex_unlock(&g_ftl_bdev_lock);

	init_cb(&info, init_arg, 0);
	return;

error_unregister:
	spdk_io_device_unregister(ftl_bdev, NULL);
error_dev:
	bdev_ftl_remove_ctrlr(ftl_bdev->ctrlr);

	free(ftl_bdev->bdev.name);
	free(ftl_bdev);

	init_cb(NULL, init_arg, rc);
}

static int
bdev_ftl_create(struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_transport_id *trid,
		const char *name, struct spdk_ftl_punit_range *range, unsigned int mode,
		const struct spdk_uuid *uuid, ftl_bdev_init_fn cb, void *cb_arg)
{
	struct ftl_bdev *ftl_bdev = NULL;
	struct ftl_bdev_ctrlr *ftl_ctrlr;
	struct spdk_ftl_dev_init_opts opts = {};
	int rc;

	ftl_ctrlr = bdev_ftl_add_ctrlr(ctrlr, trid);
	if (!ftl_ctrlr) {
		spdk_nvme_detach(ctrlr);
		return -ENOMEM;
	}

	ftl_bdev = calloc(1, sizeof(*ftl_bdev));
	if (!ftl_bdev) {
		SPDK_ERRLOG("Could not allocate ftl_bdev\n");
		rc = -ENOMEM;
		goto error_ctrlr;
	}

	ftl_bdev->bdev.name = strdup(name);
	if (!ftl_bdev->bdev.name) {
		rc = -ENOMEM;
		goto error_ctrlr;
	}

	ftl_bdev->ctrlr = ftl_ctrlr;
	ftl_bdev->init_cb = cb;
	ftl_bdev->init_arg = cb_arg;

	opts.conf = NULL;
	opts.ctrlr = ctrlr;
	opts.trid = *trid;
	opts.range = *range;
	opts.mode = mode;
	opts.uuid = *uuid;
	opts.name = ftl_bdev->bdev.name;
	/* TODO: set threads based on config */
	opts.core_thread = opts.read_thread = spdk_get_thread();

	rc = spdk_ftl_dev_init(&opts, bdev_ftl_create_cb, ftl_bdev);
	if (rc) {
		SPDK_ERRLOG("Could not create FTL device\n");
		goto error_name;
	}

	return 0;

error_name:
	free(ftl_bdev->bdev.name);
error_ctrlr:
	bdev_ftl_remove_ctrlr(ftl_ctrlr);
	free(ftl_bdev);
	return rc;
}

static void
bdev_ftl_bdev_init_done(void)
{
	pthread_mutex_lock(&g_ftl_bdev_lock);

	if (++g_num_init_bdevs != g_num_conf_bdevs) {
		pthread_mutex_unlock(&g_ftl_bdev_lock);
		return;
	}

	pthread_mutex_unlock(&g_ftl_bdev_lock);

	spdk_bdev_module_init_done(&g_ftl_if);
}

static void
bdev_ftl_init_cb(const struct ftl_bdev_info *info, void *ctx, int status)
{
	if (status) {
		SPDK_ERRLOG("Failed to initialize FTL bdev\n");
	}

	bdev_ftl_bdev_init_done();
}

static void
bdev_ftl_initialize_cb(void *ctx, int status)
{
	struct spdk_conf_section *sp;
	struct ftl_bdev_init_opts *opts = NULL;
	size_t i;

	if (status) {
		SPDK_ERRLOG("Failed to initialize FTL module\n");
		goto out;
	}

	sp = spdk_conf_find_section(NULL, "Ftl");
	if (!sp) {
		goto out;
	}

	opts = calloc(FTL_MAX_BDEVS, sizeof(*opts));
	if (!opts) {
		SPDK_ERRLOG("Failed to allocate bdev init opts\n");
		goto out;
	}

	if (bdev_ftl_read_bdev_config(sp, opts, &g_num_conf_bdevs)) {
		goto out;
	}

	for (i = 0; i < g_num_conf_bdevs; ++i) {
		if (bdev_ftl_init_bdev(&opts[i], bdev_ftl_init_cb, NULL)) {
			SPDK_ERRLOG("Failed to create bdev '%s'\n", opts[i].name);
			bdev_ftl_bdev_init_done();
		}
	}
out:
	if (g_num_conf_bdevs == 0) {
		spdk_bdev_module_init_done(&g_ftl_if);
	}

	free(opts);
}

static int
bdev_ftl_initialize(void)
{
	struct ftl_module_init_opts ftl_opts = {};
	pthread_mutexattr_t attr;
	int rc = 0;

	if (pthread_mutexattr_init(&attr)) {
		SPDK_ERRLOG("Mutex initialization failed\n");
		return -1;
	}

	if (pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE)) {
		SPDK_ERRLOG("Mutex initialization failed\n");
		rc = -1;
		goto error;
	}

	if (pthread_mutex_init(&g_ftl_bdev_lock, &attr)) {
		SPDK_ERRLOG("Mutex initialization failed\n");
		rc = -1;
		goto error;
	}

	/* TODO: retrieve this from config */
	ftl_opts.anm_thread = spdk_get_thread();
	rc = spdk_ftl_module_init(&ftl_opts, bdev_ftl_initialize_cb, NULL);

	if (rc) {
		bdev_ftl_initialize_cb(NULL, rc);

	}
error:
	pthread_mutexattr_destroy(&attr);
	return rc;
}

int
bdev_ftl_init_bdev(struct ftl_bdev_init_opts *opts, ftl_bdev_init_fn cb, void *cb_arg)
{
	struct ftl_bdev_ctrlr *ftl_ctrlr;
	struct spdk_nvme_ctrlr *ctrlr;

	assert(opts != NULL);
	assert(cb != NULL);

	pthread_mutex_lock(&g_ftl_bdev_lock);

	/* Check already attached controllers first */
	LIST_FOREACH(ftl_ctrlr, &g_ftl_bdev_ctrlrs, list_entry) {
		if (!spdk_nvme_transport_id_compare(&ftl_ctrlr->trid, &opts->trid)) {
			pthread_mutex_unlock(&g_ftl_bdev_lock);
			return bdev_ftl_create(ftl_ctrlr->ctrlr, &ftl_ctrlr->trid, opts->name,
					       &opts->range, opts->mode, &opts->uuid, cb, cb_arg);
		}
	}

	pthread_mutex_unlock(&g_ftl_bdev_lock);

	ctrlr = spdk_nvme_connect(&opts->trid, NULL, 0);
	if (!ctrlr) {
		return -ENODEV;
	}

	if (!spdk_nvme_ctrlr_is_ocssd_supported(ctrlr)) {
		spdk_nvme_detach(ctrlr);
		return -EPERM;
	}

	return bdev_ftl_create(ctrlr, &opts->trid, opts->name, &opts->range,
			       opts->mode, &opts->uuid, cb, cb_arg);
}

void
bdev_ftl_delete_bdev(const char *name, spdk_bdev_unregister_cb cb_fn, void *cb_arg)
{
	struct ftl_bdev *ftl_bdev, *tmp;

	pthread_mutex_lock(&g_ftl_bdev_lock);

	LIST_FOREACH_SAFE(ftl_bdev, &g_ftl_bdevs, list_entry, tmp) {
		if (strcmp(ftl_bdev->bdev.name, name) == 0) {
			pthread_mutex_unlock(&g_ftl_bdev_lock);
			spdk_bdev_unregister(&ftl_bdev->bdev, cb_fn, cb_arg);
			return;
		}
	}

	pthread_mutex_unlock(&g_ftl_bdev_lock);
	cb_fn(cb_arg, -ENODEV);
}

static void
bdev_ftl_ftl_module_fini_cb(void *ctx, int status)
{
	if (status) {
		SPDK_ERRLOG("Failed to deinitialize FTL module\n");
		assert(0);
	}

	spdk_bdev_module_finish_done();
}

static void
bdev_ftl_finish_cb(void)
{
	if (spdk_ftl_module_fini(bdev_ftl_ftl_module_fini_cb, NULL)) {
		SPDK_ERRLOG("Failed to deinitialize FTL module\n");
		assert(0);
	}
}

static void
bdev_ftl_finish(void)
{
	pthread_mutex_lock(&g_ftl_bdev_lock);

	if (LIST_EMPTY(&g_ftl_bdevs)) {
		pthread_mutex_unlock(&g_ftl_bdev_lock);
		bdev_ftl_finish_cb();
		return;
	}

	g_finish_cb = bdev_ftl_finish_cb;
	pthread_mutex_unlock(&g_ftl_bdev_lock);
}

SPDK_LOG_REGISTER_COMPONENT("bdev_ftl", SPDK_LOG_BDEV_FTL)
