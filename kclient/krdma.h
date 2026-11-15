// credit: https://github.com/snake0/krdma/blob/nopost/krdma.h

#ifndef __KRDMA_H
#define __KRDMA_H
/*
 * Copyright (C) 2019, Trusted Cloud Group, Shanghai Jiao Tong University.
 *
 * Authors:
 *   Yubin Chen <binsschen@sjtu.edu.cn>
 *   Jin Zhang <jzhang3002@sjtu.edu.cn>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 */
#include <linux/completion.h>
#include <linux/in.h>
#include <linux/kernel.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <rdma/ib_verbs.h>
#include <rdma/rdma_cm.h>

/* Error Macro*/
#define rdma_error(msg, args...) do {\
	printk(KERN_ERR "%s : %d : ERROR : "msg, __FILE__, __LINE__, ## args);\
} while (0)

#define KCLIENT_DEBUG

#ifdef KCLIENT_DEBUG
/* Debug Macro */
#define debug(msg, args...) do {\
	printk(KERN_INFO "DEBUG: "msg, ## args);\
} while (0)

#else

#define debug(msg, args...) do { } while (0)

#endif /* KCLIENT_DEBUG */

#define RDMA_RESOLVE_TIMEOUT 2000
/* how long to wait for any single CM event or work completion */
#define KRDMA_WAIT_TIMEOUT_MS 10000

/*
 * SEND, RDMA WRITE and RDMA READ are posted one after another, but a send
 * queue slot is only released once its completion has been polled, so leave
 * some headroom.
 */
#define RDMA_SEND_QUEUE_DEPTH 4
#define RDMA_RECV_QUEUE_DEPTH 32
#define RDMA_CQ_QUEUE_DEPTH (RDMA_SEND_QUEUE_DEPTH + RDMA_RECV_QUEUE_DEPTH)

#define RDMA_RDWR_BUF_LEN (PAGE_SIZE * 1)

/*
 * Wire format of the metadata exchanged with the user space server
 * (struct rdma_buffer_attr in src/rdma_common.h). Both sides use host
 * byte order.
 */
struct krdma_server_info {
	uint64_t addr;		/* server buffer address, used as remote_addr */
	uint32_t size;
	uint32_t rkey;
};

// simulating sending server client's secret
struct krdma_client_info {
	uint64_t token1;
	uint32_t size;
	uint32_t token2;
};

enum krdma_state {
	KRDMA_INIT = 0,
	KRDMA_ADDR_RESOLVED,
	KRDMA_ROUTE_RESOLVED,
	KRDMA_CONNECTED,
	KRDMA_RECV_DONE,
	KRDMA_WRITE_COMPLETE,
	KRDMA_READ_COMPLETE,
	/* failure states, sticky once set */
	KRDMA_ERROR,
	KRDMA_CONNECT_REJECTED,
	KRDMA_DISCONNECTED,
};

/* control block that supports both RDMA send/recv and read/write */
struct krdma_cb {
	/* protects state; taken from both process and interrupt context */
	spinlock_t lock;
	enum krdma_state state;
	/* signalled on every state change, see krdma_wait_state() */
	struct completion cm_done;

	/* Communication Manager id */
	struct rdma_cm_id *cm_id;

	/* Completion Queue */
	struct ib_cq *send_cq;
	struct ib_cq *recv_cq;
	/* Protection Domain */
	struct ib_pd *pd;
	/* Queue Pair */
	struct ib_qp *qp;

	/*
	 * All buffers below are kmalloc'ed and mapped with ib_dma_map_single().
	 * The CPU must only touch them between ib_dma_sync_single_for_cpu()
	 * and ib_dma_sync_single_for_device().
	 */

	// for RDMA SEND/RECV operations
	struct krdma_client_info *send_buf;	/* single send buf */
	u64 send_dma_addr;
	struct krdma_server_info *recv_buf;	/* single recv buf */
	u64 recv_dma_addr;

	/* copy of what the server sent us, valid after the metadata exchange */
	struct krdma_server_info server;

	// for RDMA READ/WRITE operations
	uint32_t rdma_buf_size;
	char *rdma_write_buf;			/* local rdma write ops buffer */
	u64  rdma_wbuf_dma_addr;
	char *rdma_read_buf;			/* local rdma read ops buffer */
	u64  rdma_rbuf_dma_addr;
};

struct krdma_cb *krdma_alloc_cb(void);
int krdma_init_cb(struct krdma_cb *cb);
void krdma_free_cb(struct krdma_cb *cb);
int krdma_cma_event_handler(struct rdma_cm_id *cm_id,
		struct rdma_cm_event *event);
int krdma_wait_state(struct krdma_cb *cb, enum krdma_state want);
int krdma_set_addr(struct sockaddr_in *addr, const char *host, const int port);
int krdma_resolve_remote(struct krdma_cb *cb, const char *host, const int port);
int krdma_setup_buf(struct krdma_cb *cb);
void krdma_free_buf(struct krdma_cb *cb);
uint64_t string_to_uint64(const char *str);

#endif /* __KRDMA_H */
