/*
 * Copyright (c) 2014, 2015 EMC Corporation.  All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifndef NTRDMA_PORT_H
#define NTRDMA_PORT_H

#include "ntrdma_os.h"

struct ntrdma_ntb_dev;
struct ntrdma_quiesce;

#define NTRDMA_VERSION_NONE		0

/* NTRDMA state messages sent and received from the peer */
enum ntrdma_port_state {
	NTRDMA_QUIESCE = 0,	/* quiesce is requested or peer lost */
	NTRDMA_RESET_REQ,	/* port reset is requested */
	NTRDMA_RESET_ACK,	/* port reset is acknowledged */
	NTRDMA_READY_SPAD,	/* spad values are ready */
	NTRDMA_READY_INIT,	/* initialization values are ready */
	NTRDMA_READY_CONF,	/* configuration values are ready */
	NTRDMA_READY_PORT,	/* port is ready to enable device */
};

/* NTRDMA peer info header to be used by all future versions
 *
 * Each driver will allocate memory for a peer_info structure, to be
 * initialized by the peer.  The dma address of the peer_info will be
 * communicated by way of ntb scratchpad registers.
 *
 * After the address is communicated, each peer initializes the other's
 * peer_info.  The peer_info is used to verify the version, the pingval in
 * location in memory instead of scratchpads, and additional information
 * specific to the version.
 */
struct ntrdma_peer_info_hdr {
	/* Version tag unique to the chosen version, for verification */
	ntrdma_u32_t			version_tag;
	/* Pingval location in memory to be used instead of spad */
	ntrdma_u32_t			pingval;
};

int ntrdma_ntb_port_init(struct ntrdma_ntb_dev *dev);
void ntrdma_ntb_port_deinit(struct ntrdma_ntb_dev *dev);

void ntrdma_port_link_event(struct ntrdma_ntb_dev *dev);
void ntrdma_port_recv_event(struct ntrdma_ntb_dev *dev);

void ntrdma_port_send(struct ntrdma_ntb_dev *dev, int send_event);
void ntrdma_port_quiesce(struct ntrdma_ntb_dev *dev);

ntrdma_u32_t ntrdma_port_info_read_pingval(struct ntrdma_ntb_dev *dev);
void ntrdma_port_info_write_pingval(struct ntrdma_ntb_dev *dev, ntrdma_u32_t val);

/* Create a version code for negotiating with the peer.
 *
 * supported - the highest protocol version supported
 * compatible - the lowest version supported, for compatibility
 */
static inline ntrdma_u32_t ntrdma_version_code(ntrdma_u16_t supported,
					       ntrdma_u16_t compatible)
{
	return ((ntrdma_u32_t)supported) | (((ntrdma_u32_t)compatible) << 16);
}

/* Get the highest protocol version supported from the version code. */
static inline ntrdma_u16_t ntrdma_version_supported(ntrdma_u32_t version)
{
	return (ntrdma_u16_t)(version);
}

/* Get the lowest protocol version supported from the version code. */
static inline ntrdma_u16_t ntrdma_version_compatible(ntrdma_u32_t version)
{
	return (ntrdma_u16_t)(version >> 16);
}

/* Get the highest version supported by both drivers */
static inline ntrdma_u16_t ntrdma_version_choose(ntrdma_u32_t va,
						 ntrdma_u32_t vb)
{
	ntrdma_u16_t as, ac, bs, bc;

	as = ntrdma_version_supported(va);
	ac = ntrdma_version_compatible(va);
	bs = ntrdma_version_supported(vb);
	bc = ntrdma_version_compatible(vb);

	/* B is compatible with A version */
	if (bc <= as && as <= bs)
		return as;

	/* A is compatible with B version */
	if (ac <= bs && bs <= as)
		return bs;

	/* the versions are not compatible */
	return NTRDMA_VERSION_NONE;
}

/* Create a pingval code for exchanging states with the peer.
 *
 * state - the numeric initialization state of the driver
 * seqno - the pingval sequence number
 */
static inline ntrdma_u32_t ntrdma_pingval(ntrdma_u16_t state,
					  ntrdma_u16_t seqno)
{
	return (((ntrdma_u32_t)state) << 16) | ((ntrdma_u32_t)seqno);
}

/* Get the highest protocol version supported from the version code. */
static inline ntrdma_u16_t ntrdma_pingval_state(ntrdma_u32_t pingval)
{
	return (ntrdma_u16_t)(pingval >> 16);
}

#endif
