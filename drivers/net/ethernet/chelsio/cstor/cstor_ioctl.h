/*
 * Copyright (c) 2021-2025 Chelsio Communications. All rights reserved.
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
#ifndef __CSTOR_USER_H__
#define __CSTOR_USER_H__

#define CSTOR_MODULE_VERSION	"1.10.0"
#define CSTOR_MAX_ADAPTERS 64

#define CSTOR_DRIVER_NAME "cstor"

struct t4_status_page {
	__be16 rsvd1;	/* flit 0 - hw owns */
	__be16 qid;
	__be16 cidx;
	__be16 pidx;
	u8 qp_err;	/* flit 1- sw owns */
	u8 cq_armed;
	u8 pad[6];
};

#define CSTOR_ISCSI_ERR_MASK		0x0FF6C000

enum cstor_ioctl_cmd {
	CSTOR_IOCTL_CMD_QUERY_DEVICE = 1,
	CSTOR_IOCTL_CMD_ALLOC_PD,
	CSTOR_IOCTL_CMD_DEALLOC_PD,
	CSTOR_IOCTL_CMD_REG_MR,
	CSTOR_IOCTL_CMD_DEREG_MR,
	CSTOR_IOCTL_CMD_DMA_MAP_MR,
	CSTOR_IOCTL_CMD_DMA_UNMAP_MR,
	CSTOR_IOCTL_CMD_CREATE_CQ,
	CSTOR_IOCTL_CMD_DESTROY_CQ,
	CSTOR_IOCTL_CMD_CREATE_SRQ,
	CSTOR_IOCTL_CMD_DESTROY_SRQ,
	CSTOR_IOCTL_CMD_CREATE_RXQ,
	CSTOR_IOCTL_CMD_DESTROY_RXQ,
	CSTOR_IOCTL_CMD_CREATE_QP,
	CSTOR_IOCTL_CMD_DESTROY_QP,
	CSTOR_IOCTL_CMD_CREATE_LISTEN,
	CSTOR_IOCTL_CMD_DESTROY_LISTEN,
	CSTOR_IOCTL_CMD_SOCK_ACCEPT,
	CSTOR_IOCTL_CMD_SOCK_REJECT,
	CSTOR_IOCTL_CMD_FREE_ATID,
	CSTOR_IOCTL_CMD_SOCK_ATTACH_QP,
	CSTOR_IOCTL_CMD_RESOLVE_ROUTE,
	CSTOR_IOCTL_CMD_CONNECT,
	CSTOR_IOCTL_CMD_SOCK_DISCONNECT,
	CSTOR_IOCTL_CMD_SOCK_RELEASE,
	CSTOR_IOCTL_CMD_GET_UEVENT,
	CSTOR_IOCTL_CMD_CREATE_EVENT_CHANNEL,
	CSTOR_IOCTL_CMD_DESTROY_EVENT_CHANNEL,
	CSTOR_IOCTL_CMD_FIND_DEVICE,
	CSTOR_IOCTL_CMD_SET_ISCSI_REGION_STATUS,
	CSTOR_IOCTL_CMD_ENABLE_ISCSI_DIGEST,
	CSTOR_IOCTL_CMD_INVALIDATE_ISCSI_TAG,
	CSTOR_IOCTL_CMD_SEND_ISCSI_PDU,
};

#define CSTOR_IOCTL_MAGIC       (0x5D)

/*
 * Make sure that all structs defined in this file remain laid out so
 * that they pack the same way on 32-bit and 64-bit architectures (to
 * avoid incompatibility between 32-bit userspace and 64-bit kernels).
 * In particular do not use pointer types -- pass pointers in __u64
 * instead.
 */

struct _nvme_device_attr {
	__u64 page_size_cap;
	__u32 max_ddp_sge;
	__u32 max_ddp_tag;
	__u32 stag_start_addr32;
	__u32 cq_start;
	__u32 max_cq;
	__u32 max_cqe;
	__u32 max_srq;
	__u32 max_srq_wr;
};

#define DEFAULT_DDR_ZONES	4
#define MAX_DDR_ZONES		8
#define MAX_EDRAM_ZONES		2

struct _iscsi_device_attr {
	__u32 fl_page_size_cap;
	__u32 ddp_page_size_cap;
	__u32 region_size;
	__u32 iscsi_tagmask;
	__u32 ppod_llimit;
	__u32 ppod_start;
	__u32 edram_start;
	__u32 edram_size;
	__u32 edram_ppod_zone_percentage[MAX_EDRAM_ZONES];
	__u32 edram_ppod_per_bit[MAX_EDRAM_ZONES];
	__u32 ddr_ppod_zone_percentage[MAX_DDR_ZONES];
	__u32 ddr_ppod_per_bit[MAX_DDR_ZONES];
	__u8 num_edram_zones;
	__u8 num_ddr_zones;
};

struct _cstor_device_attr {
#define _CSTOR_DEV_NAME_LEN 32
	__u8 name[_CSTOR_DEV_NAME_LEN];
	__u64 fw_ver;
	__u64 mac_addr[4];
	__u32 vendor_id;
	__u32 vendor_part_id;
	__u32 hw_ver;
	__u32 chip_ver;
	__u32 qp_start;
	__u32 max_qp;
	__u32 max_qp_wr;
	__u32 max_pd;
	__u32 max_pdu_size;
	__u32 max_lso_buf_size;
	__u32 stid_base;
	__u32 max_listen_sock;
	__u32 tid_base;
	__u32 max_sock;
	__u32 max_atids;
	__u32 num_ports;
	__u32 wc_enabled;
	__u32 max_mr;
	__u64 max_mr_size;
	__u8 plat_dev;
	struct _nvme_device_attr nvme;
	struct _iscsi_device_attr iscsi;
};

struct cstor_query_device_resp {
	struct _cstor_device_attr attr;
};

struct cstor_query_device_cmd {
	struct cstor_query_device_resp resp;
};

#define CSTOR_IOCTL_QUERY_DEVICE    \
_IOR(CSTOR_IOCTL_MAGIC, CSTOR_IOCTL_CMD_QUERY_DEVICE, struct cstor_query_device_cmd)

struct cstor_alloc_pd_resp {
	__u32 pdid;
};

struct cstor_alloc_pd_cmd {
	struct cstor_alloc_pd_resp resp;
};

#define CSTOR_IOCTL_ALLOC_PD    \
_IOR(CSTOR_IOCTL_MAGIC, CSTOR_IOCTL_CMD_ALLOC_PD, struct cstor_alloc_pd_cmd)

struct cstor_dealloc_pd_cmd {
	__u32 pdid;
};

#define CSTOR_IOCTL_DEALLOC_PD    \
_IOW(CSTOR_IOCTL_MAGIC, CSTOR_IOCTL_CMD_DEALLOC_PD, struct cstor_dealloc_pd_cmd)

enum _cstor_access_flags {
	_CSTOR_ACCESS_LOCAL_WRITE	 = (1U << 0),
	_CSTOR_ACCESS_REMOTE_WRITE	 = (1U << 1),
	_CSTOR_ACCESS_REMOTE_READ	 = (1U << 2),
};

struct cstor_reg_mr_resp {
	__u32 pbl_addr;
	__u32 page_size;
	__u32 lkey;
	__u32 pbl_start;
};

struct cstor_reg_mr_cmd {
	__u64 pbl_ptr;
	__u64 start;
	__u64 length;
	__u64 acc;
	__u32 tid;
	__u32 srq;
	__u32 pdid;
	struct cstor_reg_mr_resp resp;
};

#define CSTOR_IOCTL_REG_MR    \
_IOWR(CSTOR_IOCTL_MAGIC, CSTOR_IOCTL_CMD_REG_MR, struct cstor_reg_mr_cmd)

struct cstor_dereg_mr_cmd {
	__u32 lkey;
};

#define CSTOR_IOCTL_DEREG_MR    \
_IOW(CSTOR_IOCTL_MAGIC, CSTOR_IOCTL_CMD_DEREG_MR, struct cstor_dereg_mr_cmd)

struct cstor_create_cq_resp {
	__u64 key;
	__u64 gts_key;
	__u64 memsize;
	__u32 cqid;
	__u32 size;
	__u32 qid_mask;
	__u32 reserved; /* explicit padding (optional for i386) */
};

struct cstor_create_cq_cmd {
#define INVALID_EVENT_FD 0xFFFFFFFF
	__u32 event_fd;
	__u32 num_cqe;
	__u16 cqe_size;
	struct cstor_create_cq_resp resp;
};

#define CSTOR_IOCTL_CREATE_CQ    \
_IOWR(CSTOR_IOCTL_MAGIC, CSTOR_IOCTL_CMD_CREATE_CQ, struct cstor_create_cq_cmd)

struct cstor_destroy_cq_cmd {
	__u32 cqid;
};

#define CSTOR_IOCTL_DESTROY_CQ    \
_IOW(CSTOR_IOCTL_MAGIC, CSTOR_IOCTL_CMD_DESTROY_CQ, struct cstor_destroy_cq_cmd)

struct cstor_create_srq_resp {
	__u64 srq_key;
	__u64 srq_db_gts_key;
	__u64 srq_memsize;
	__u32 srqid;
	__u32 srq_size;
	__u32 srq_max_wr;
	__u32 rqt_abs_idx;
	__u32 qid_mask;
	__u32 flags;
	__u32 max_wr;
};

struct cstor_create_srq_cmd {
	__u32 max_wr;
	__u32 pdid;
	struct cstor_create_srq_resp resp;
};

#define CSTOR_IOCTL_CREATE_SRQ    \
_IOWR(CSTOR_IOCTL_MAGIC, CSTOR_IOCTL_CMD_CREATE_SRQ, struct cstor_create_srq_cmd)

struct cstor_destroy_srq_cmd {
	__u32 srqid;
};

#define CSTOR_IOCTL_DESTROY_SRQ    \
_IOW(CSTOR_IOCTL_MAGIC, CSTOR_IOCTL_CMD_DESTROY_SRQ, struct cstor_destroy_srq_cmd)

struct cstor_create_rxq_resp {
	__u64 db_key;
	__u64 fl_key;
	__u64 iq_key;
	__u64 fl_memsize;
	__u64 iq_memsize;
	__u32 fl_id;
	__u32 iq_id;
	__u32 abs_id;
	__u32 fl_size;
	__u32 iq_size;
	__u32 fl_align;
	__u32 iqe_len;
	__u32 flags;
	__u32 qid_mask;
	__u64 fl_bar2_key;
	__u64 iq_bar2_key;
};

struct cstor_create_rxq_cmd {
	__u8 port_id;
	__u32 max_wr;
	__u32 fl_page_size;
	struct cstor_create_rxq_resp resp;
};

#define CSTOR_IOCTL_CREATE_RXQ    \
_IOWR(CSTOR_IOCTL_MAGIC, CSTOR_IOCTL_CMD_CREATE_RXQ, struct cstor_create_rxq_cmd)

struct cstor_destroy_rxq_cmd {
	__u32 rxqid;
};

#define CSTOR_IOCTL_DESTROY_RXQ    \
_IOW(CSTOR_IOCTL_MAGIC, CSTOR_IOCTL_CMD_DESTROY_RXQ, struct cstor_destroy_rxq_cmd)

struct cstor_create_qp_resp {
	__u64 ma_sync_key;
	__u64 sq_key;
	__u64 rq_key;
	__u64 sq_db_gts_key;
	__u64 rq_db_gts_key;
	__u64 sq_memsize;
	__u64 rq_memsize;
	__u32 sqid;
	__u32 rqid;
	__u32 sq_size;
	__u32 rq_size;
	__u32 rq_max_wr;
	__u32 qid_mask;
	__u32 flags;
	__u32 max_send_wr;
	__u32 max_recv_wr;
	__u32 stag_idx;
	__u32 pbl_offset;
	__u32 pbl_max_ddp_sge;
};

struct cstor_create_qp_cmd {
	__u32 pdid;
	__u32 scqid;
	__u32 rcqid;
#define CSTOR_INVALID_SRQID 0xFFFFFFFF
	__u32 srqid;
#define CSTOR_INVALID_RXQID 0xFFFFFFFF
	__u32 rxqid;
	__u32 max_send_wr;
	__u32 max_recv_wr;
	__u32 max_ddp_sge;
	__u32 max_ddp_tag;
	__u8 protocol;
	struct cstor_create_qp_resp resp;
};

#define CSTOR_IOCTL_CREATE_QP    \
_IOWR(CSTOR_IOCTL_MAGIC, CSTOR_IOCTL_CMD_CREATE_QP, struct cstor_create_qp_cmd)

struct cstor_destroy_qp_cmd {
	__u32 qid;
};

#define CSTOR_IOCTL_DESTROY_QP    \
_IOW(CSTOR_IOCTL_MAGIC, CSTOR_IOCTL_CMD_DESTROY_QP, struct cstor_destroy_qp_cmd)

struct cstor_create_server_resp {
	__u32 stid;
	__u8 port_id;
};

enum _cstor_transport_protocol {
	_CSTOR_NVME_TCP_PROTOCOL = 1,
	_CSTOR_ISCSI_PROTOCOL = 2,
};

struct cstor_create_listen_resp {
	__u32 stid;
	__u8 port_id;
};

#define _CSTOR_LCSK_INADDR_ANY_PORT_ID 0xFF

struct cstor_create_listen_cmd {
	__u8 ipv4;
	__u8 protocol;
	__u8 inaddr_any;
	__be16 tcp_port;
	__be32 ip_addr[4];
	__u32 backlog;
	__u32 efd;
	__u32 first_pdu_recv_timeout;
	struct cstor_create_listen_resp resp;
};

#define CSTOR_IOCTL_CREATE_LISTEN    \
_IOWR(CSTOR_IOCTL_MAGIC, CSTOR_IOCTL_CMD_CREATE_LISTEN, struct cstor_create_listen_cmd)

struct cstor_destroy_listen_cmd {
	__u32 stid;
};

#define CSTOR_IOCTL_DESTROY_LISTEN    \
_IOW(CSTOR_IOCTL_MAGIC, CSTOR_IOCTL_CMD_DESTROY_LISTEN, struct cstor_destroy_listen_cmd)

union _cstor_qp_attr {
	struct {
		__u8 rx_pda;
		__u8 hdgst;
		__u8 ddgst;
		__u8 cmd_pdu_hdr_recv_zcopy;
	};
	struct {
		__u32 ddp_page_size;
	};
};

struct cstor_accept_cr_cmd {
	union _cstor_qp_attr attr;
	__u32 tid;
	__u32 qid;
	__u8 protocol;
};

#define CSTOR_IOCTL_SOCK_ACCEPT    \
_IOW(CSTOR_IOCTL_MAGIC, CSTOR_IOCTL_CMD_SOCK_ACCEPT, struct cstor_accept_cr_cmd)

struct cstor_reject_cr_cmd {
	__u32 tid;
};

#define CSTOR_IOCTL_SOCK_REJECT    \
_IOW(CSTOR_IOCTL_MAGIC, CSTOR_IOCTL_CMD_SOCK_REJECT, struct cstor_reject_cr_cmd)

struct cstor_free_atid_cmd {
	__u32 atid;
};

#define CSTOR_IOCTL_FREE_ATID    \
_IOW(CSTOR_IOCTL_MAGIC, CSTOR_IOCTL_CMD_FREE_ATID, struct cstor_free_atid_cmd)

struct cstor_resolve_route_resp {
	__u8 port_id;
};

struct cstor_resolve_route_cmd {
	struct sockaddr_storage raddr;
	struct cstor_resolve_route_resp resp;
};

#define CSTOR_IOCTL_RESOLVE_ROUTE    \
_IOWR(CSTOR_IOCTL_MAGIC, CSTOR_IOCTL_CMD_RESOLVE_ROUTE, struct cstor_resolve_route_cmd)

#define CSTOR_INVALID_ATID 0xffffffff
#define CSTOR_INVALID_TID 0xffffffff

struct cstor_connect_resp {
	__u32 atid;
};

struct cstor_connect_cmd {
	struct sockaddr_storage raddr;
	__u32 efd;
	__u8 protocol;
	struct cstor_connect_resp resp;
};

#define CSTOR_IOCTL_CONNECT    \
_IOWR(CSTOR_IOCTL_MAGIC, CSTOR_IOCTL_CMD_CONNECT, struct cstor_connect_cmd)

struct cstor_sock_attach_qp_cmd {
	union _cstor_qp_attr attr;
	__u32 tid;
	__u32 qid;
	__u8 protocol;
};

#define CSTOR_IOCTL_SOCK_ATTACH_QP	\
_IOW(CSTOR_IOCTL_MAGIC, CSTOR_IOCTL_CMD_SOCK_ATTACH_QP, struct cstor_sock_attach_qp_cmd)

struct cstor_sock_disconnect_cmd {
	__u32 tid;
	__u8 abort;
};

#define CSTOR_IOCTL_SOCK_DISCONNECT    \
_IOW(CSTOR_IOCTL_MAGIC, CSTOR_IOCTL_CMD_SOCK_DISCONNECT, struct cstor_sock_disconnect_cmd)

struct cstor_sock_release_cmd {
	__u32 tid;
	__u32 atid;
};

#define CSTOR_IOCTL_SOCK_RELEASE    \
_IOW(CSTOR_IOCTL_MAGIC, CSTOR_IOCTL_CMD_SOCK_RELEASE, struct cstor_sock_release_cmd)

enum cstor_uevent_type {
	CSTOR_UEVENT_CONNECT_REQ = 1,
	CSTOR_UEVENT_CONNECT_RPL,
	CSTOR_UEVENT_RECV_ISCSI_PDU,
	CSTOR_UEVENT_DISCONNECTED,
	CSTOR_UEVENT_DEVICE_FATAL,
	CSTOR_UEVENT_MAX,
};

struct _cstor_connect_req {
	__u32 stid;
	__u32 tid;
	__u16 num_tags;
	__u8 port_id;
	__u8 ipv4;
	__u8 protocol;
	__u16 vlan_id;
	__be16 lport;
	__be16 rport;
	__be32 laddr[4];
	__be32 raddr[4];
	__be32 rcv_nxt;
};

enum _cstor_connect_status {
	_CSTOR_CONNECT_SUCCESS,
	_CSTOR_CONNECT_FAILURE,
};

struct _cstor_connect_rpl {
	__u32 tid;
	__u32 atid;
	__u16 num_tags;
	__u8 port_id;
	__u8 ipv4;
	__u8 status;
	__u16 vlan_id;
	__be16 lport;
	__be16 rport;
	__be32 laddr[4];
	__be32 raddr[4];
	__be32 rcv_nxt;

};

struct _cstor_iscsi_pdu_info {
	__u32 tid;
	__u32 pdu_len;
	__u32 hlen;
	__u32 status;
};

struct cstor_uevent {
	u16 event;
	union {
		struct _cstor_connect_req req;
		struct _cstor_connect_rpl rpl;
		struct _cstor_iscsi_pdu_info pdu_info;
		__u32 tid;
		__u8 port_id;
	} u;
};

struct cstor_get_uevent_resp {
	struct cstor_uevent uevt;
};

struct cstor_get_uevent_cmd {
	__u64 buf;
	__u32 buf_len;
	__u32 efd;
	struct cstor_get_uevent_resp resp;
};

#define CSTOR_IOCTL_GET_UEVENT    \
_IOWR(CSTOR_IOCTL_MAGIC, CSTOR_IOCTL_CMD_GET_UEVENT, struct cstor_get_uevent_cmd)

enum _cstor_event_channel_flags {
	_CSTOR_EVENT_CHANNEL_FLAG_CM_EVENT	= (1U << 0),
	_CSTOR_EVENT_CHANNEL_FLAG_ASYNC_EVENT	= (1U << 1),
};

struct cstor_create_event_channel_cmd {
	__u32 efd;
	__u32 flag;
};

#define CSTOR_IOCTL_CREATE_EVENT_CHANNEL	\
_IOW(CSTOR_IOCTL_MAGIC, CSTOR_IOCTL_CMD_CREATE_EVENT_CHANNEL, struct cstor_create_event_channel_cmd)

struct cstor_destroy_event_channel_cmd {
	__u32 efd;
};

#define CSTOR_IOCTL_DESTROY_EVENT_CHANNEL	\
_IOW(CSTOR_IOCTL_MAGIC, CSTOR_IOCTL_CMD_DESTROY_EVENT_CHANNEL,	\
struct cstor_destroy_event_channel_cmd)

struct cstor_find_device_resp {
	__u8 port_id;
};

struct cstor_find_device_cmd {
	__u8 ipv4;
	__be16 tcp_port;
	__be32 ip_addr[4];
	struct cstor_find_device_resp resp;
};

#define CSTOR_IOCTL_FIND_DEVICE    \
_IOWR(CSTOR_IOCTL_MAGIC, CSTOR_IOCTL_CMD_FIND_DEVICE, struct cstor_find_device_cmd)

enum cstor_iscsi_region_status {
	CSTOR_ISCSI_REGION_INUSE,
	CSTOR_ISCSI_REGION_FREE,
};

struct cstor_set_iscsi_region_status_cmd {
	__u8 status;
};

#define CSTOR_IOCTL_SET_ISCSI_REGION_STATUS	\
_IOW(CSTOR_IOCTL_MAGIC, CSTOR_IOCTL_CMD_SET_ISCSI_REGION_STATUS,	\
     struct cstor_set_iscsi_region_status_cmd)

struct _cstor_enable_iscsi_digest_attr {
	__u8 hdgst;
	__u8 ddgst;
};

struct cstor_enable_iscsi_digest_cmd {
	__u32 tid;
	union {
		struct _cstor_enable_iscsi_digest_attr dgst;
	};
};

#define CSTOR_IOCTL_ENABLE_ISCSI_DIGEST	\
_IOW(CSTOR_IOCTL_MAGIC, CSTOR_IOCTL_CMD_ENABLE_ISCSI_DIGEST, struct cstor_enable_iscsi_digest_cmd)

struct cstor_iscsi_tag_info {
	u32 pm_addr;
	u32 dlen;
};

struct cstor_invalidate_iscsi_tag_cmd {
#define _MAX_INVALIDATE_ISCSI_TAG 32
	struct cstor_iscsi_tag_info tinfo[_MAX_INVALIDATE_ISCSI_TAG];
	u32 count;
};

#define CSTOR_IOCTL_INVALIDATE_ISCSI_TAG	\
_IOW(CSTOR_IOCTL_MAGIC, CSTOR_IOCTL_CMD_INVALIDATE_ISCSI_TAG,	\
     struct cstor_invalidate_iscsi_tag_cmd)

struct cstor_send_iscsi_pdu_cmd {
	__u64 buf;
	__u32 buf_len;
	__u32 tid;
	__u8 hdgst;
	__u8 ddgst;
};

#define CSTOR_IOCTL_SEND_ISCSI_PDU	\
_IOW(CSTOR_IOCTL_MAGIC, CSTOR_IOCTL_CMD_SEND_ISCSI_PDU, struct cstor_send_iscsi_pdu_cmd)
#endif
