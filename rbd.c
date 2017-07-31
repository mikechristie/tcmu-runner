/*
 * Copyright 2016, China Mobile, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may
 * not use this file except in compliance with the License. You may obtain
 * a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations
 * under the License.
 */

#define _GNU_SOURCE
#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <endian.h>
#include <errno.h>
#include <pthread.h>

#include <scsi/scsi.h>

#include "tcmu-runner.h"
#include "libtcmu.h"
#include "tcmur_device.h"

#include <rbd/librbd.h>

/*
 * rbd_lock_acquire exclusive lock support was added in librbd 0.1.11
 */
#if LIBRBD_VERSION_CODE >= LIBRBD_VERSION(0, 1, 11)
#define RBD_LOCK_ACQUIRE_SUPPORT
#endif

enum {
	TCMU_RBD_OPENING,
	TCMU_RBD_OPENED,
	TCMU_RBD_CLOSING,
	TCMU_RBD_CLOSED,
};

struct tcmu_rbd_state {
	rados_t cluster;
	rados_ioctx_t io_ctx;
	rbd_image_t image;

	char *image_name;
	char *pool_name;
	char *osd_op_timeout;

	pthread_spinlock_t lock;	/* protect state */
	int state;
};

struct rbd_aio_cb {
	struct tcmu_device *dev;
	struct tcmulib_cmd *tcmulib_cmd;

	int64_t length;
	char *bounce_buffer;
};

static void tcmu_rbd_image_close(struct tcmu_device *dev)
{
	struct tcmu_rbd_state *state = tcmu_get_dev_private(dev);

	pthread_spin_lock(&state->lock);
	if (state->state != TCMU_RBD_OPENED) {
		tcmu_dev_dbg(dev, "skipping close. state %d\n", state->state);
		pthread_spin_unlock(&state->lock);
		return;
	}
	state->state = TCMU_RBD_CLOSING;
	pthread_spin_unlock(&state->lock);

	rbd_close(state->image);
	rados_ioctx_destroy(state->io_ctx);
	rados_shutdown(state->cluster);

	state->cluster = NULL;
	state->io_ctx = NULL;
	state->image = NULL;

	pthread_spin_lock(&state->lock);
	state->state = TCMU_RBD_CLOSED;
	pthread_spin_unlock(&state->lock);
}

static int tcmu_rbd_image_open(struct tcmu_device *dev)
{
	struct tcmu_rbd_state *state = tcmu_get_dev_private(dev);
	int ret;

	pthread_spin_lock(&state->lock);
	if (state->state == TCMU_RBD_OPENED) {
		tcmu_dev_dbg(dev, "skipping open. Already opened\n");
		pthread_spin_unlock(&state->lock);
		return 0;
	}

	if (state->state != TCMU_RBD_CLOSED) {
		tcmu_dev_dbg(dev, "skipping open. state %d\n", state->state);
		pthread_spin_unlock(&state->lock);
		return -EBUSY;
	}
	state->state = TCMU_RBD_OPENING;
	pthread_spin_unlock(&state->lock);

	ret = rados_create(&state->cluster, NULL);
	if (ret < 0) {
		tcmu_dev_dbg(dev, "Could not create cluster. (Err %d)\n", ret);
		goto set_closed;
	}

	/* Fow now, we will only read /etc/ceph/ceph.conf */
	rados_conf_read_file(state->cluster, NULL);
	rados_conf_set(state->cluster, "rbd_cache", "false");
	ret = rados_conf_set(state->cluster, "rados_osd_op_timeout",
			     state->osd_op_timeout);
	if (ret < 0) {
		tcmu_dev_err(dev, "Could not set rados osd op timeout to %s (Err %d. Failover may be delayed.)\n",
			     state->osd_op_timeout, ret);
	}

	ret = rados_connect(state->cluster);
	if (ret < 0) {
		tcmu_dev_err(dev, "Could not connect to cluster. (Err %d)\n",
			     ret);
		goto set_cluster_null;
	}

	ret = rados_ioctx_create(state->cluster, state->pool_name,
				 &state->io_ctx);
	if (ret < 0) {
		tcmu_dev_err(dev, "Could not create ioctx for pool %s. (Err %d)\n",
			     state->pool_name, ret);
		goto rados_shutdown;
	}

	ret = rbd_open(state->io_ctx, state->image_name, &state->image, NULL);
	if (ret < 0) {
		tcmu_dev_err(dev, "Could not open image %s. (Err %d)\n",
			     state->image_name, ret);
		goto rados_destroy;
	}

	pthread_spin_lock(&state->lock);
	state->state = TCMU_RBD_OPENED;
	pthread_spin_unlock(&state->lock);
	return 0;

rados_destroy:
	rados_ioctx_destroy(state->io_ctx);
	state->io_ctx = NULL;
rados_shutdown:
	rados_shutdown(state->cluster);
set_cluster_null:
	state->cluster = NULL;
set_closed:
	pthread_spin_lock(&state->lock);
	state->state = TCMU_RBD_CLOSED;
	pthread_spin_unlock(&state->lock);
	return ret;
}

#ifdef RBD_LOCK_ACQUIRE_SUPPORT

static int has_lock(struct tcmu_device *dev)
{
	struct tcmu_rbd_state *state = tcmu_get_dev_private(dev);
	int ret, is_owner;

	ret = rbd_is_exclusive_lock_owner(state->image, &is_owner);
	if (ret == -ESHUTDOWN) {
 		/* -ESHUTDOWN/-EBLACKLISTED(-108) = client is blacklisted. */
		return ret;
	} else if (ret < 0) {
		/* let initiator figure things out */
		tcmu_dev_err(dev, "Could not check lock ownership. (Err %d).\n", ret);
		return -EIO;
	} else if (is_owner) {
		tcmu_dev_dbg(dev, "Is owner\n");
		return 1;
	}
	tcmu_dev_dbg(dev, "Not owner\n");

	return 0;
}

static int tcmu_rbd_has_lock(struct tcmu_device *dev)
{
	if (has_lock(dev) <= 0)
		return TCMUR_LOCK_FAILED;

	return TCMUR_LOCK_SUCCESS;
}

static int tcmu_rbd_image_reopen(struct tcmu_device *dev)
{
	struct tcmu_rbd_state *state = tcmu_get_dev_private(dev);
	int ret;

	tcmu_rbd_image_close(dev);
	ret = tcmu_rbd_image_open(dev);

	if (!ret) {
		tcmu_dev_warn(dev, "image %s/%s was blacklisted. Successfully reopened.\n",
			      state->pool_name, state->image_name);
	} else {
		tcmu_dev_warn(dev, "image %s/%s was blacklisted. Reopen failed with error %d.\n",
			      state->pool_name, state->image_name, ret);
	}

	return ret;
}

/**
 * tcmu_rbd_lock_break - break rbd exclusive lock if needed
 * @dev: device to break the lock for.
 * @orig_owner: if non null, only break the lock if get owners matches
 *
 * If orig_owner is null and tcmu_rbd_lock_break fails to break the lock
 * for a retryable error (-EAGAIN) the owner of the lock will be returned.
 * The caller must free the string returned.
 *
 * Returns:
 * 0 = lock has been broken.
 * -EAGAIN = retryable error
 * -EIO = hard failure.
 */
static int tcmu_rbd_lock_break(struct tcmu_device *dev, char **orig_owner)
{
	struct tcmu_rbd_state *state = tcmu_get_dev_private(dev);
	rbd_lock_mode_t lock_mode;
	char *owners[1];
	size_t num_owners = 1;
	int ret;

	ret = rbd_lock_get_owners(state->image, &lock_mode, owners,
				  &num_owners);
	if (ret == -ENOENT || (!ret && !num_owners))
		return 0;

	if (ret < 0) {
		tcmu_dev_err(dev, "Could not get lock owners %d\n", ret);
		return -EAGAIN;
	}

	if (lock_mode != RBD_LOCK_MODE_EXCLUSIVE) {
		tcmu_dev_err(dev, "Invalid lock type (%d) found\n", lock_mode);
		ret = -EIO;
		goto free_owners;
	}

	if (*orig_owner && strcmp(*orig_owner, owners[0])) {
		/* someone took the lock while we were retrying */
		ret = -EIO;
		goto free_owners;
	}

	tcmu_dev_dbg(dev, "Attempting to break lock from %s.\n", owners[0]);

	ret = rbd_lock_break(state->image, lock_mode, owners[0]);
	if (ret < 0) {
		tcmu_dev_err(dev, "Could not break lock from %s. (Err %d)\n",
			     owners[0], ret);
		ret = -EAGAIN;
		if (!*orig_owner) {
			*orig_owner = strdup(owners[0]);
			if (!*orig_owner)
				ret = -EIO;
		}
	}

free_owners:
	rbd_lock_get_owners_cleanup(owners, num_owners);
	return ret;
}

static int tcmu_rbd_lock(struct tcmu_device *dev)
{
	struct tcmu_rbd_state *state = tcmu_get_dev_private(dev);
	int ret = 0, attempts = 0;
	char *orig_owner = NULL;

	/*
	 * TODO: Add retry/timeout settings to handle windows/ESX.
	 * Or, set to transitioning and grab the lock in the background.
	 */
	while (attempts++ < 5) {
		ret = has_lock(dev);
		if (ret == 1) {
			ret = 0;
			break;
		} else if (ret == -ESHUTDOWN) {
			ret = tcmu_rbd_image_reopen(dev);
			continue;
		} else if (ret < 0) {
			sleep(1);
			continue;
		}

		ret = tcmu_rbd_lock_break(dev, &orig_owner);
		if (ret == -EIO)
			break;
		else if (ret == -EAGAIN) {
			sleep(1);
			continue;
		}

		ret = rbd_lock_acquire(state->image, RBD_LOCK_MODE_EXCLUSIVE);
		if (!ret) {
			tcmu_dev_warn(dev, "Acquired exclusive lock.\n");
			break;
		}

		tcmu_dev_err(dev, "Unknown error %d while trying to acquire lock.\n",
			     ret);
	}

	if (orig_owner)
		free(orig_owner);

	if (ret)
		return TCMUR_LOCK_FAILED;
	else
		return TCMUR_LOCK_SUCCESS;
}

static int tcmu_rbd_unlock(struct tcmu_device *dev)
{
	struct tcmu_rbd_state *state = tcmu_get_dev_private(dev);

	if (rbd_lock_release(state->image))
		return TCMUR_LOCK_FAILED;
	return TCMUR_LOCK_SUCCESS;
}

#endif

static void tcmu_rbd_state_free(struct tcmu_rbd_state *state)
{
	pthread_spin_destroy(&state->lock);

	if (state->osd_op_timeout)
		free(state->osd_op_timeout);
	if (state->image_name)
		free(state->image_name);
	if (state->pool_name)
		free(state->pool_name);
	free(state);
}

static int tcmu_rbd_open(struct tcmu_device *dev)
{
	struct tcmur_device *rdev = tcmu_get_daemon_dev_private(dev);
	rbd_image_info_t image_info;
	char *pool, *name, *next_opt;
	char *config, *dev_cfg_dup;
	struct tcmu_rbd_state *state;
	uint64_t rbd_size;
	int ret;

	state = calloc(1, sizeof(*state));
	if (!state)
		return -ENOMEM;
	state->state = TCMU_RBD_CLOSED;
	tcmu_set_dev_private(dev, state);

	ret = pthread_spin_init(&state->lock, 0);
	if (ret != 0) {
		free(state);
		return ret;
	}

	dev_cfg_dup = strdup(tcmu_get_dev_cfgstring(dev));
	config = dev_cfg_dup;
	if (!config) {
		ret = -ENOMEM;
		goto free_state;
	}

	tcmu_dev_dbg(dev, "tcmu_rbd_open config %s\n", config);
	config = strchr(config, '/');
	if (!config) {
		tcmu_dev_err(dev, "no configuration found in cfgstring\n");
		ret = -EINVAL;
		goto free_config;
	}
	config += 1; /* get past '/' */

	pool = strtok(config, "/");
	if (!pool) {
		tcmu_dev_err(dev, "Could not get pool name\n");
		ret = -EINVAL;
		goto free_config;
	}
	state->pool_name = strdup(pool);
	if (!state->pool_name) {
		ret = -ENOMEM;
		tcmu_dev_err(dev, "Could not copy pool name\n");
		goto free_config;
	}

	name = strtok(NULL, "/");
	if (!name) {
		tcmu_dev_err(dev, "Could not get image name\n");
		ret = -EINVAL;
		goto free_config;
	}

	state->image_name = strdup(name);
	if (!state->image_name) {
		ret = -ENOMEM;
		tcmu_dev_err(dev, "Could not copy image name\n");
		goto free_config;
	}

	/* The next options are optional */
	next_opt = strtok(NULL, "/");
	if (next_opt) {
		if (!strncmp(next_opt, "osd_op_timeout=", 15)) {
			state->osd_op_timeout = strdup(next_opt + 15);
			if (!state->osd_op_timeout ||
			    !strlen(state->osd_op_timeout)) {
				ret = -ENOMEM;
				tcmu_dev_err(dev, "Could not copy osd op timeout.\n");
				goto free_config;
			}
		}
	}

	if (rdev->flags & TMCUR_DEV_FLAG_FO_USE_AA && !state->osd_op_timeout) {
		tcmu_dev_warn(dev, "rados osd op timeout not set, but active/active enabled.\n")
	}

	ret = tcmu_rbd_image_open(dev);
	if (ret < 0) {
		goto free_config;
	}

	ret = rbd_get_size(state->image, &rbd_size);
	if (ret < 0) {
		tcmu_dev_err(dev, "error getting rbd_size %s\n", name);
		goto stop_image;
	}

	if (rbd_size !=
	    tcmu_get_dev_num_lbas(dev) * tcmu_get_dev_block_size(dev)) {
		tcmu_dev_err(dev, "device size and backing size disagree: device (num LBAs %lld, block size %ld) backing %lld\n",
			     tcmu_get_dev_num_lbas(dev),
			     tcmu_get_dev_block_size(dev), rbd_size);
		ret = -EIO;
		goto stop_image;
	}

	ret = rbd_stat(state->image, &image_info, sizeof(image_info));
	if (ret < 0) {
		tcmu_dev_err(dev, "Could not stat image.\n");
		goto stop_image;
	}
	tcmu_set_dev_max_xfer_len(dev, image_info.obj_size /
				  tcmu_get_dev_block_size(dev));

	tcmu_dev_dbg(dev, "config %s, size %lld\n", tcmu_get_dev_cfgstring(dev),
		     rbd_size);
	free(dev_cfg_dup);
	return 0;

stop_image:
	tcmu_rbd_image_close(dev);
free_config:
	free(dev_cfg_dup);
free_state:
	tcmu_rbd_state_free(state);
	return ret;
}

static void tcmu_rbd_close(struct tcmu_device *dev)
{
	struct tcmu_rbd_state *state = tcmu_get_dev_private(dev);

	tcmu_rbd_image_close(dev);
	tcmu_rbd_state_free(state);
}

/*
 * Tmp hack. We really want to disconnect ceph tcp connections with TCP
 * USER timeout set really low or SO_LINGER=0 (if that setting does
 * really kill the retry queue and immediately send a RST) then do
 * new function that works like rados_op_cancel and then close and reopen
 * the image.
 *
 * We don't have that so we kill the box. The initiator multipath layer
 * will handle like a normal node down.
 */
static void tcmu_reboot(void)
{
	char *reboot = "b";
	int fd, ret;

	fd = open("/proc/sysrq-trigger", O_WRONLY);
	if (fd < 0) {
		tcmu_err("Could not open sysrq to reboot. Error %d.\n", errno);
		return;
	}

	ret = write(fd, reboot, 1);
	if (ret <= 0) {
		tcmu_err("Could not write to sysrq to reboot. Error %d\n",
			 errno);
		goto close;
	}
close:
	close(fd);
}

static int tcmu_rbd_handle_timedout_cmd(struct tcmu_device *dev,
					struct tcmulib_cmd *cmd)
{
	tcmu_dev_err(dev, "timing out cmd.\n");
	tcmu_reboot();

	/*
	 * TODO: When we can do a clean fast client restart fail cmd below.
	 * We are going to kill the iscsi connection, so return a retryable
	 * error and let the initiator multipath layer retry.
	 */
	return SAM_STAT_BUSY;
}

/*
 * NOTE: RBD async APIs almost always return 0 (success), except
 * when allocation (via new) fails - which is not caught. So,
 * the only errno we've to bother about as of now are memory
 * allocation errors.
 */

static void rbd_finish_aio_read(rbd_completion_t completion,
				struct rbd_aio_cb *aio_cb)
{
	struct tcmu_device *dev = aio_cb->dev;
	struct tcmulib_cmd *tcmulib_cmd = aio_cb->tcmulib_cmd;
	struct iovec *iovec = tcmulib_cmd->iovec;
	size_t iov_cnt = tcmulib_cmd->iov_cnt;
	int64_t ret;
	int tcmu_r;

	ret = rbd_aio_get_return_value(completion);
	rbd_aio_release(completion);

	if (ret == -ETIMEDOUT) {
		tcmu_r = tcmu_rbd_handle_timedout_cmd(dev, tcmulib_cmd);
	} else if (ret == -ESHUTDOWN) {
		tcmu_r = tcmu_set_sense_data(tcmulib_cmd->sense_buf,
					     NOT_READY, ASC_PORT_IN_STANDBY,
					     NULL);
	} else if (ret < 0) {
		tcmu_r = tcmu_set_sense_data(tcmulib_cmd->sense_buf,
					     MEDIUM_ERROR, ASC_READ_ERROR, NULL);
	} else {
		tcmu_r = SAM_STAT_GOOD;
		tcmu_memcpy_into_iovec(iovec, iov_cnt,
				       aio_cb->bounce_buffer, aio_cb->length);
	}

	tcmulib_cmd->done(dev, tcmulib_cmd, tcmu_r);

	free(aio_cb->bounce_buffer);
	free(aio_cb);
}

static int tcmu_rbd_read(struct tcmu_device *dev, struct tcmulib_cmd *cmd,
			     struct iovec *iov, size_t iov_cnt, size_t length,
			     off_t offset)
{
	struct tcmu_rbd_state *state = tcmu_get_dev_private(dev);
	struct rbd_aio_cb *aio_cb;
	rbd_completion_t completion;
	ssize_t ret;

	aio_cb = calloc(1, sizeof(*aio_cb));
	if (!aio_cb) {
		tcmu_dev_err(dev, "Could not allocate aio_cb.\n");
		goto out;
	}

	aio_cb->dev = dev;
	aio_cb->length = length;
	aio_cb->tcmulib_cmd = cmd;

	aio_cb->bounce_buffer = malloc(length);
	if (!aio_cb->bounce_buffer) {
		tcmu_dev_err(dev, "Could not allocate bounce buffer.\n");
		goto out_free_aio_cb;
	}

	ret = rbd_aio_create_completion
		(aio_cb, (rbd_callback_t) rbd_finish_aio_read, &completion);
	if (ret < 0) {
		goto out_free_bounce_buffer;
	}

	ret = rbd_aio_read(state->image, offset, length, aio_cb->bounce_buffer,
			   completion);
	if (ret < 0) {
		goto out_remove_tracked_aio;
	}

	return 0;

out_remove_tracked_aio:
	rbd_aio_release(completion);
out_free_bounce_buffer:
	free(aio_cb->bounce_buffer);
out_free_aio_cb:
	free(aio_cb);
out:
	return SAM_STAT_TASK_SET_FULL;
}

static void rbd_finish_aio_generic(rbd_completion_t completion,
				   struct rbd_aio_cb *aio_cb)
{
	struct tcmu_device *dev = aio_cb->dev;
	struct tcmulib_cmd *tcmulib_cmd = aio_cb->tcmulib_cmd;
	int64_t ret;
	int tcmu_r;

	ret = rbd_aio_get_return_value(completion);
	rbd_aio_release(completion);

	if (ret == -ETIMEDOUT) {
		tcmu_r = tcmu_rbd_handle_timedout_cmd(dev, tcmulib_cmd);
	} else if (ret == -ESHUTDOWN) {
		tcmu_r = tcmu_set_sense_data(tcmulib_cmd->sense_buf,
					     NOT_READY, ASC_PORT_IN_STANDBY,
					     NULL);
		} else if (ret < 0) {
		tcmu_r = tcmu_set_sense_data(tcmulib_cmd->sense_buf,
					     MEDIUM_ERROR, ASC_WRITE_ERROR,
					     NULL);
	} else {
		tcmu_r = SAM_STAT_GOOD;
	}

	tcmulib_cmd->done(dev, tcmulib_cmd, tcmu_r);

	if (aio_cb->bounce_buffer) {
		free(aio_cb->bounce_buffer);
	}
	free(aio_cb);
}

static int tcmu_rbd_write(struct tcmu_device *dev, struct tcmulib_cmd *cmd,
			  struct iovec *iov, size_t iov_cnt, size_t length,
			  off_t offset)
{
	struct tcmu_rbd_state *state = tcmu_get_dev_private(dev);
	struct rbd_aio_cb *aio_cb;
	rbd_completion_t completion;
	ssize_t ret;

	aio_cb = calloc(1, sizeof(*aio_cb));
	if (!aio_cb) {
		tcmu_dev_err(dev, "Could not allocate aio_cb.\n");
		goto out;
	}

	aio_cb->dev = dev;
	aio_cb->length = length;
	aio_cb->tcmulib_cmd = cmd;

	aio_cb->bounce_buffer = malloc(length);
	if (!aio_cb->bounce_buffer) {
		tcmu_dev_err(dev, "Failed to allocate bounce buffer.\n");
		goto out_free_aio_cb;
	}

	tcmu_memcpy_from_iovec(aio_cb->bounce_buffer, length, iov, iov_cnt);

	ret = rbd_aio_create_completion
		(aio_cb, (rbd_callback_t) rbd_finish_aio_generic, &completion);
	if (ret < 0) {
		goto out_free_bounce_buffer;
	}

	ret = rbd_aio_write(state->image, offset,
			    length, aio_cb->bounce_buffer, completion);
	if (ret < 0) {
		goto out_remove_tracked_aio;
	}

	return 0;

out_remove_tracked_aio:
	rbd_aio_release(completion);
out_free_bounce_buffer:
	free(aio_cb->bounce_buffer);
out_free_aio_cb:
	free(aio_cb);
out:
	return SAM_STAT_TASK_SET_FULL;
}

#ifdef LIBRBD_SUPPORTS_AIO_FLUSH

static int tcmu_rbd_flush(struct tcmu_device *dev, struct tcmulib_cmd *cmd)
{
	struct tcmu_rbd_state *state = tcmu_get_dev_private(dev);
	struct rbd_aio_cb *aio_cb;
	rbd_completion_t completion;
	ssize_t ret = -ENOMEM;

	aio_cb = calloc(1, sizeof(*aio_cb));
	if (!aio_cb) {
		tcmu_dev_err(dev, "Could not allocate aio_cb.\n");
		goto out;
	}

	aio_cb->dev = dev;
	aio_cb->tcmulib_cmd = cmd;
	aio_cb->bounce_buffer = NULL;

	ret = rbd_aio_create_completion
		(aio_cb, (rbd_callback_t) rbd_finish_aio_generic, &completion);
	if (ret < 0) {
		goto out_free_aio_cb;
	}

	ret = rbd_aio_flush(state->image, completion);
	if (ret < 0) {
		goto out_remove_tracked_aio;
	}

	return 0;

out_remove_tracked_aio:
	rbd_aio_release(completion);
out_free_aio_cb:
	free(aio_cb);
out:
	return SAM_STAT_TASK_SET_FULL;
}

#endif

/*
 * For backstore creation
 *
 * Specify poolname/devicename, e.g,
 *
 * $ targetcli create /backstores/user:rbd/test 2G rbd/test/osd_op_timeout=30
 *
 * poolname must be the name of an existing rados pool.
 *
 * devicename is the name of the rbd image.
 */
static const char tcmu_rbd_cfg_desc[] =
	"RBD config string is of the form:\n"
	"poolname/devicename/optional osd_op_timeout=N secs\n"
	"where:\n"
	"poolname:	Existing RADOS pool\n"
	"devicename:	Name of the RBD image\n";

struct tcmur_handler tcmu_rbd_handler = {
	.name	       = "Ceph RBD handler",
	.subtype       = "rbd",
	.cfg_desc      = tcmu_rbd_cfg_desc,
	.open	       = tcmu_rbd_open,
	.close	       = tcmu_rbd_close,
	.read	       = tcmu_rbd_read,
	.write	       = tcmu_rbd_write,
#ifdef LIBRBD_SUPPORTS_AIO_FLUSH
	.flush	       = tcmu_rbd_flush,
#endif

#ifdef RBD_LOCK_ACQUIRE_SUPPORT
	.lock          = tcmu_rbd_lock,
	.unlock        = tcmu_rbd_unlock,
	.has_lock      = tcmu_rbd_has_lock,
#endif
};

int handler_init(void)
{
	return tcmur_register_handler(&tcmu_rbd_handler);
}
