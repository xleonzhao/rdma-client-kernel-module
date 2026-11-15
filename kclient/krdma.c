// credit: https://github.com/snake0/krdma

#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/in.h>
#include <linux/inet.h>
#include <linux/jiffies.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/types.h>

#include "krdma.h"

int krdma_set_addr(struct sockaddr_in *addr, const char *host, const int port) {
	if (!addr || !host)
		return -EINVAL;
	if (port <= 0 || port > 65535)
		return -EINVAL;

	memset(addr, 0, sizeof(struct sockaddr_in));
	addr->sin_family = AF_INET;
	if (!in4_pton(host, -1, (u8 *)&addr->sin_addr.s_addr, -1, NULL))
		return -EINVAL;
	addr->sin_port = htons(port);
	return 0;
}

uint64_t string_to_uint64(const char *str)
{
    uint64_t value = 0;
    size_t len = strlen(str);
    size_t i;

    // Ensure we do not read beyond 8 characters
    len = len > 8 ? 8 : len;

    for (i = 0; i < len; i++) {
        value |= (uint64_t)(unsigned char)str[i] << (8 * (7 - i));
    }

    return value;
}

static bool krdma_state_failed(enum krdma_state state)
{
	return state == KRDMA_ERROR ||
	       state == KRDMA_CONNECT_REJECTED ||
	       state == KRDMA_DISCONNECTED;
}

/*
 * Record a state change and wake up the waiter. A failure state is sticky:
 * once the connection has failed, a late success event must not hide it.
 * Safe to call from interrupt context.
 */
static void krdma_set_state(struct krdma_cb *cb, enum krdma_state state)
{
	unsigned long flags;

	spin_lock_irqsave(&cb->lock, flags);
	if (!krdma_state_failed(cb->state))
		cb->state = state;
	spin_unlock_irqrestore(&cb->lock, flags);

	complete(&cb->cm_done);
}

/*
 * Sleep until the control block reaches the state @want.
 *
 * Returns 0 on success, -ETIMEDOUT if nothing happened within
 * KRDMA_WAIT_TIMEOUT_MS, or a negative errno if the connection failed.
 * Wake-ups for unrelated events are ignored.
 */
int krdma_wait_state(struct krdma_cb *cb, enum krdma_state want)
{
	unsigned long timeout = msecs_to_jiffies(KRDMA_WAIT_TIMEOUT_MS);
	enum krdma_state state;
	unsigned long flags;

	for (;;) {
		if (!wait_for_completion_timeout(&cb->cm_done, timeout)) {
			rdma_error("timed out waiting for state %d\n", want);
			return -ETIMEDOUT;
		}

		spin_lock_irqsave(&cb->lock, flags);
		state = cb->state;
		spin_unlock_irqrestore(&cb->lock, flags);

		if (state == want)
			return 0;
		if (krdma_state_failed(state)) {
			rdma_error("waiting for state %d, but got %d\n",
					want, state);
			return state == KRDMA_CONNECT_REJECTED ?
					-ECONNREFUSED : -ECONNRESET;
		}
		/* stray wake-up from an earlier event, keep waiting */
	}
}

/* Runs in interrupt context: no sleeping, no access to the mapped buffers. */
static void krdma_cq_event_handler(struct ib_cq *cq, void *ctx)
{
	struct krdma_cb *cb = ctx;
	struct ib_wc wc;
	int ret;

	// reiterate our interests to continue receiving notifications
	ib_req_notify_cq(cq, IB_CQ_NEXT_COMP);

	while ((ret = ib_poll_cq(cq, 1, &wc)) == 1) {
		if (wc.status) {
			if (wc.status == IB_WC_WR_FLUSH_ERR) {
				debug("cq flushed\n");
			} else {
				rdma_error("cq completion failed with "
				       "wr_id %llx status %d opcode %d vender_err %x\n",
					wc.wr_id, wc.status, wc.opcode, wc.vendor_err);
			}
			/* wc.opcode is not reliable for a failed completion */
			krdma_set_state(cb, KRDMA_ERROR);
			continue;
		}

		switch (wc.opcode) {
		case IB_WC_SEND:
			debug("send completion\n");
			break;

		case IB_WC_RDMA_WRITE:
			debug("rdma write completion\n");
			krdma_set_state(cb, KRDMA_WRITE_COMPLETE);
			break;

		case IB_WC_RDMA_READ:
			debug("rdma read completion\n");
			krdma_set_state(cb, KRDMA_READ_COMPLETE);
			break;

		case IB_WC_RECV:
			debug("recv completion, %u bytes received\n", wc.byte_len);
			if (wc.byte_len != sizeof(struct krdma_server_info)) {
				rdma_error("unexpected message length %u\n",
						wc.byte_len);
				krdma_set_state(cb, KRDMA_ERROR);
				break;
			}
			krdma_set_state(cb, KRDMA_RECV_DONE);
			break;

		default:
			rdma_error("%s:%d Unexpected opcode %d, Shutting down\n",
			       __func__, __LINE__, wc.opcode);
			krdma_set_state(cb, KRDMA_ERROR);
			break;
		}
	}
	if (ret) {
		rdma_error("poll error %d\n", ret);
		krdma_set_state(cb, KRDMA_ERROR);
	}
}

int krdma_cma_event_handler(struct rdma_cm_id *cm_id,
		struct rdma_cm_event *event)
{
	struct krdma_cb *cb = cm_id->context;

	switch (event->event) {
	case RDMA_CM_EVENT_ADDR_RESOLVED:
		debug("%s: RDMA_CM_EVENT_ADDR_RESOLVED, cm_id %p\n",
				__func__, cm_id);
		krdma_set_state(cb, KRDMA_ADDR_RESOLVED);
		break;

	case RDMA_CM_EVENT_ROUTE_RESOLVED:
		debug("%s: RDMA_CM_EVENT_ROUTE_RESOLVED, cm_id %p\n",
				__func__, cm_id);
		krdma_set_state(cb, KRDMA_ROUTE_RESOLVED);
		break;

	case RDMA_CM_EVENT_ESTABLISHED:
		debug("%s: RDMA_CM_EVENT_ESTABLISHED, cm_id %p\n",
				__func__, cm_id);
		krdma_set_state(cb, KRDMA_CONNECTED);
		break;

	case RDMA_CM_EVENT_DISCONNECTED:
		debug("%s: RDMA_CM_EVENT_DISCONNECTED, cm_id %p\n",
				__func__, cm_id);
		krdma_set_state(cb, KRDMA_DISCONNECTED);
		break;

	case RDMA_CM_EVENT_CONNECT_ERROR:
	case RDMA_CM_EVENT_UNREACHABLE:
	case RDMA_CM_EVENT_REJECTED:
		rdma_error("RDMA_CM_EVENT %d, cm_id %p, error %d\n",
				event->event, cm_id, event->status);
		krdma_set_state(cb, KRDMA_CONNECT_REJECTED);
		break;

	case RDMA_CM_EVENT_TIMEWAIT_EXIT:
		/* normal tail end of a disconnect, nothing to do */
		break;

	case RDMA_CM_EVENT_ADDR_ERROR:
	case RDMA_CM_EVENT_ROUTE_ERROR:
	case RDMA_CM_EVENT_DEVICE_REMOVAL:
	default:
		rdma_error("%s: RDMA_CM_EVENT %d, cm_id %p, error %d\n",
				__func__, event->event, cm_id, event->status);
		krdma_set_state(cb, KRDMA_ERROR);
		break;
	}
	return 0;
}

/*
 * Allocate a buffer and map it for the RDMA device. ib_dma_map_single()
 * does the right thing for both hardware devices (real DMA mapping) and
 * software devices such as siw and rxe (kernel virtual addresses).
 */
static void *krdma_alloc_mapped(struct krdma_cb *cb, size_t size,
		enum dma_data_direction dir, u64 *dma_addr)
{
	struct ib_device *dev = cb->pd->device;
	void *buf;

	buf = kzalloc(size, GFP_KERNEL);
	if (!buf)
		return NULL;

	*dma_addr = ib_dma_map_single(dev, buf, size, dir);
	if (ib_dma_mapping_error(dev, *dma_addr)) {
		rdma_error("Failed to map buffer, length=%zu\n", size);
		kfree(buf);
		return NULL;
	}
	return buf;
}

static void krdma_free_mapped(struct krdma_cb *cb, void *buf, u64 dma_addr,
		size_t size, enum dma_data_direction dir)
{
	if (!buf)
		return;
	ib_dma_unmap_single(cb->pd->device, dma_addr, size, dir);
	kfree(buf);
}

/* Buffers for send/recv and for locally issued RDMA read write */
int krdma_setup_buf(struct krdma_cb *cb) {
	if (!cb || !cb->pd)
		return -EINVAL;

	cb->rdma_buf_size = RDMA_RDWR_BUF_LEN;

	cb->send_buf = krdma_alloc_mapped(cb, sizeof(*cb->send_buf),
				DMA_TO_DEVICE, &cb->send_dma_addr);
	if (!cb->send_buf)
		goto exit;

	cb->recv_buf = krdma_alloc_mapped(cb, sizeof(*cb->recv_buf),
				DMA_FROM_DEVICE, &cb->recv_dma_addr);
	if (!cb->recv_buf)
		goto exit;

	cb->rdma_write_buf = krdma_alloc_mapped(cb, cb->rdma_buf_size,
				DMA_TO_DEVICE, &cb->rdma_wbuf_dma_addr);
	if (!cb->rdma_write_buf)
		goto exit;

	cb->rdma_read_buf = krdma_alloc_mapped(cb, cb->rdma_buf_size,
				DMA_FROM_DEVICE, &cb->rdma_rbuf_dma_addr);
	if (!cb->rdma_read_buf)
		goto exit;

	debug("rdma write buffer mapped ok, dma_addr %llx, size %u\n",
			cb->rdma_wbuf_dma_addr, cb->rdma_buf_size);
	debug("rdma read buffer mapped ok, dma_addr %llx, size %u\n",
			cb->rdma_rbuf_dma_addr, cb->rdma_buf_size);

	return 0;

exit:
	krdma_free_buf(cb);
	return -ENOMEM;
}

/* Safe to call more than once and on a partially set up control block. */
void krdma_free_buf(struct krdma_cb *cb) {
	if (!cb || !cb->pd)
		return;

	krdma_free_mapped(cb, cb->send_buf, cb->send_dma_addr,
			sizeof(*cb->send_buf), DMA_TO_DEVICE);
	cb->send_buf = NULL;

	krdma_free_mapped(cb, cb->recv_buf, cb->recv_dma_addr,
			sizeof(*cb->recv_buf), DMA_FROM_DEVICE);
	cb->recv_buf = NULL;

	krdma_free_mapped(cb, cb->rdma_write_buf, cb->rdma_wbuf_dma_addr,
			cb->rdma_buf_size, DMA_TO_DEVICE);
	cb->rdma_write_buf = NULL;

	krdma_free_mapped(cb, cb->rdma_read_buf, cb->rdma_rbuf_dma_addr,
			cb->rdma_buf_size, DMA_FROM_DEVICE);
	cb->rdma_read_buf = NULL;
}

struct krdma_cb * krdma_alloc_cb(void)
{
	struct krdma_cb *cb;

	cb = kzalloc(sizeof(*cb), GFP_KERNEL);
	if (!cb) {
		rdma_error("failed to allocate memory for cb\n");
		return NULL;
	}

	spin_lock_init(&cb->lock);
	init_completion(&cb->cm_done);

	return cb;
}

/*
 * Allocate pd, cq and qp. On failure whatever was allocated so far is left
 * in the control block (failed members are NULL) for krdma_free_cb().
 */
int krdma_init_cb(struct krdma_cb *cb) {
	int ret;
	struct ib_cq_init_attr cq_attr;
	struct ib_qp_init_attr qp_init_attr;

	if (!cb || !cb->cm_id)
		return -EINVAL;

	/* Create Protection Domain. */
	cb->pd = ib_alloc_pd(cb->cm_id->device, 0);
	if (IS_ERR(cb->pd)) {
		ret = PTR_ERR(cb->pd);
		cb->pd = NULL;
		rdma_error("ib_alloc_pd failed, ret %d\n", ret);
		return ret;
	}
	debug("ib_alloc_pd succeed, cm_id %p\n", cb->cm_id);

	/* Create send Completion Queue. */
	memset(&cq_attr, 0, sizeof(cq_attr));
	cq_attr.cqe = RDMA_CQ_QUEUE_DEPTH;
	cq_attr.comp_vector = 0;
	cb->send_cq = ib_create_cq(cb->cm_id->device, krdma_cq_event_handler, NULL, cb, &cq_attr);
	if (IS_ERR(cb->send_cq)) {
		ret = PTR_ERR(cb->send_cq);
		cb->send_cq = NULL;
		rdma_error("ib_create_cq failed, ret %d\n", ret);
		return ret;
	}

	/* Create recv Completion Queue. */
	memset(&cq_attr, 0, sizeof(cq_attr));
	cq_attr.cqe = RDMA_CQ_QUEUE_DEPTH;
	cq_attr.comp_vector = 0;
	cb->recv_cq = ib_create_cq(cb->cm_id->device, krdma_cq_event_handler, NULL, cb, &cq_attr);
	if (IS_ERR(cb->recv_cq)) {
		ret = PTR_ERR(cb->recv_cq);
		cb->recv_cq = NULL;
		rdma_error("ib_create_cq failed, ret %d\n", ret);
		return ret;
	}

	ret = ib_req_notify_cq(cb->send_cq, IB_CQ_NEXT_COMP);
	if (ret) {
		rdma_error("ib_req_notify_cq() for send cq failed\n");
		return ret;
	}

	ret = ib_req_notify_cq(cb->recv_cq, IB_CQ_NEXT_COMP);
	if (ret) {
		rdma_error("ib_req_notify_cq() for recv cq failed\n");
		return ret;
	}

	debug("ib_create_cq succeed, cm_id %p\n", cb->cm_id);

	/* Create Queue Pair. */
	memset(&qp_init_attr, 0, sizeof(qp_init_attr));
	qp_init_attr.cap.max_send_wr = RDMA_SEND_QUEUE_DEPTH;
	qp_init_attr.cap.max_recv_wr = RDMA_RECV_QUEUE_DEPTH;
	qp_init_attr.cap.max_recv_sge = 1;
	qp_init_attr.cap.max_send_sge = 1;
	/* Mlx doesn't support inline sends for kernel QPs (yet) */
	qp_init_attr.cap.max_inline_data = 0;
	qp_init_attr.qp_type = IB_QPT_RC;
	qp_init_attr.send_cq = cb->send_cq;
	qp_init_attr.recv_cq = cb->recv_cq;
	qp_init_attr.sq_sig_type = IB_SIGNAL_REQ_WR;
	ret = rdma_create_qp(cb->cm_id, cb->pd, &qp_init_attr);
	if (ret) {
		rdma_error("rdma_create_qp failed, ret %d\n", ret);
		return ret;
	}
	cb->qp = cb->cm_id->qp;
	debug("ib_create_qp succeed, cm_id %p\n", cb->cm_id);

	return 0;
}

/*
 * Tear down everything the control block owns and free it. Works on a
 * control block in any state of setup, including NULL.
 */
void krdma_free_cb(struct krdma_cb *cb)
{
	if (!cb)
		return;

	if (cb->cm_id) {
		/* fails harmlessly if the connection was never established */
		rdma_disconnect(cb->cm_id);
		if (cb->qp) {
			rdma_destroy_qp(cb->cm_id);
			cb->qp = NULL;
		}
	}

	/* needs cb->pd for unmapping, so before the pd goes away */
	krdma_free_buf(cb);

	if (cb->send_cq) {
		ib_destroy_cq(cb->send_cq);
		cb->send_cq = NULL;
	}
	if (cb->recv_cq) {
		ib_destroy_cq(cb->recv_cq);
		cb->recv_cq = NULL;
	}

	if (cb->pd) {
		ib_dealloc_pd(cb->pd);
		cb->pd = NULL;
	}

	if (cb->cm_id) {
		/* waits for any running CM event handler to finish */
		rdma_destroy_id(cb->cm_id);
		cb->cm_id = NULL;
	}

	kfree(cb);
}

/*
 * Call rdma_resolve_route for dev detection
 */
int krdma_resolve_remote(struct krdma_cb *cb, const char *host, const int port) {
	int ret;
	struct sockaddr_in addr;

	/* Resolve address */
	ret = krdma_set_addr(&addr, host, port);
	if (ret < 0) {
		rdma_error("invalid server address or port\n");
		return ret;
	}
	ret = rdma_resolve_addr(cb->cm_id, NULL,
			(struct sockaddr *)&addr, RDMA_RESOLVE_TIMEOUT);
	if (ret) {
		rdma_error("rdma_resolve_addr failed, ret %d\n", ret);
		return ret;
	}
	ret = krdma_wait_state(cb, KRDMA_ADDR_RESOLVED);
	if (ret) {
		rdma_error("rdma_resolve_addr did not complete, ret %d\n", ret);
		return ret;
	}
	debug("rdma_resolve_addr succeed, device[%s] port_num[%u]\n",
		cb->cm_id->device->name, cb->cm_id->port_num);

	/* Resolve route. */
	ret = rdma_resolve_route(cb->cm_id, RDMA_RESOLVE_TIMEOUT);
	if (ret) {
		rdma_error("rdma_resolve_route failed, ret %d\n", ret);
		return ret;
	}
	ret = krdma_wait_state(cb, KRDMA_ROUTE_RESOLVED);
	if (ret) {
		rdma_error("rdma_resolve_route did not complete, ret %d\n", ret);
		return ret;
	}
	debug("rdma_resolve_route succeed, cm_id %p, "
			"remote host[%pI4], remote port[%d]\n",
			cb->cm_id,
			&((struct sockaddr_in *)&cb->cm_id->route.addr.dst_addr)->sin_addr.s_addr,
			ntohs(((struct sockaddr_in *)&cb->cm_id->route.addr.dst_addr)->sin_port));

	return 0;
}
