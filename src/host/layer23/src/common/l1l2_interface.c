/* Layer 1 socket interface of layer2/3 stack */

/* (C) 2010 by Holger Hans Peter Freyther
 * (C) 2010,2018 by Harald Welte <laforge@gnumonks.org>
 *
 * All Rights Reserved
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <osmocom/bb/common/osmocom_data.h>
#include <osmocom/bb/common/l1ctl.h>
#include <osmocom/bb/common/logging.h>
#include <osmocom/bb/common/l1l2_interface.h>

#include <osmocom/core/utils.h>
#include <osmocom/core/socket.h>

#include <sys/socket.h>
#include <sys/un.h>

#include <arpa/inet.h>

#define _GNU_SOURCE
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>

#define GSM_L2_LENGTH 256
#define GSM_L2_HEADROOM 32

static int layer2_read(struct osmo_fd *fd)
{
	struct msgb *msg;
	uint16_t len;
	int rc;

	msg = msgb_alloc_headroom(GSM_L2_LENGTH+GSM_L2_HEADROOM, GSM_L2_HEADROOM, "Layer2");
	if (!msg) {
		LOGP(DL1C, LOGL_ERROR, "Failed to allocate msg.\n");
		return -ENOMEM;
	}

	rc = read(fd->fd, &len, sizeof(len));
	if (rc < sizeof(len)) {
		fprintf(stderr, "Layer2 socket failed\n");
		msgb_free(msg);
		if (rc >= 0)
			rc = -EIO;
		layer2_close((struct osmocom_ms *) fd->data);
		exit(102);
		return rc;
	}

	len = ntohs(len);
	if (len > GSM_L2_LENGTH) {
		LOGP(DL1C, LOGL_ERROR, "Length is too big: %u\n", len);
		msgb_free(msg);
		return -EINVAL;
	}


	msg->l1h = msgb_put(msg, len);
	rc = read(fd->fd, msg->l1h, msgb_l1len(msg));
	if (rc != msgb_l1len(msg)) {
		LOGP(DL1C, LOGL_ERROR, "Can not read data: len=%d rc=%d "
		     "errno=%d\n", len, rc, errno);
		msgb_free(msg);
		return rc;
	}

	l1ctl_recv((struct osmocom_ms *) fd->data, msg);

	return 0;
}

static int layer2_write(struct osmo_fd *fd, struct msgb *msg)
{
	int rc;

	if (fd->fd <= 0)
		return -EINVAL;

	rc = write(fd->fd, msg->data, msg->len);
	if (rc != msg->len) {
		LOGP(DL1C, LOGL_ERROR, "Failed to write data: rc: %d\n", rc);
		return rc;
	}

	return 0;
}

int layer2_open(struct osmocom_ms *ms, const char *socket_path)
{
	int rc;

	rc = osmo_sock_unix_init_ofd(&ms->l2_wq.bfd, SOCK_STREAM, 0, socket_path, OSMO_SOCK_F_CONNECT);
	if (rc < 0) {
		LOGP(DL1C, LOGL_ERROR, "Failed to create unix domain socket %s: %s\n",
		     socket_path, strerror(-rc));
		return rc;
	}

	osmo_wqueue_init(&ms->l2_wq, 100);
	ms->l2_wq.bfd.data = ms;
	ms->l2_wq.read_cb = layer2_read;
	ms->l2_wq.write_cb = layer2_write;

	return 0;
}

int layer2_close(struct osmocom_ms *ms)
{
	if (ms->l2_wq.bfd.fd <= 0)
		return -EINVAL;

	close(ms->l2_wq.bfd.fd);
	ms->l2_wq.bfd.fd = -1;
	osmo_fd_unregister(&ms->l2_wq.bfd);
	osmo_wqueue_clear(&ms->l2_wq);

	return 0;
}

int osmo_send_l1(struct osmocom_ms *ms, struct msgb *msg)
{
	DEBUGP(DL1C, "Sending: '%s'\n", osmo_hexdump(msg->data, msg->len));

	if (msg->l1h != msg->data)
		LOGP(DL1C, LOGL_ERROR, "Message L1 header != Message Data\n");

	/* prepend 16bit length before sending */
	msgb_push_u16(msg, msg->len);

	if (osmo_wqueue_enqueue(&ms->l2_wq, msg) != 0) {
		LOGP(DL1C, LOGL_ERROR, "Failed to enqueue msg.\n");
		msgb_free(msg);
		return -1;
	}

	return 0;
}


