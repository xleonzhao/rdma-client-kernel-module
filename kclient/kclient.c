/*
 * An example RDMA client side code. 
 * Author: Animesh Trivedi 
 *         atrivedi@apache.org
 */

/*
* RDMA client as a kernel module
* Author: Leon Zhao
*         xleonzhao@gmail.com 
*/

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/in.h>
#include <linux/inet.h>
#include <linux/string.h>

#include "krdma.h"

#define this_client_token1 "ABCDEFGH"
#define this_client_token2 "4321"

#define IPADDR_LEN 16
static char server[IPADDR_LEN]={0}; // server ip address
module_param_string(server, server, IPADDR_LEN, S_IRUGO);
MODULE_PARM_DESC(server, "IPv4 address of the RDMA server (required)");

static int port = 20886; // server port number
module_param(port, int, S_IRUGO);
MODULE_PARM_DESC(port, "port of the RDMA server (default 20886)");

/* The data we write to the server buffer and expect to read back */
static const char test_string[] = "hello, world";

static struct task_struct *thread = NULL;

/* This function prepares client side connection resources for an RDMA connection */
static int client_init_rdma(struct krdma_cb *cb)
{
	int ret;

	/* Create cm_id */
	cb->cm_id = rdma_create_id(&init_net, krdma_cma_event_handler, cb,
					RDMA_PS_TCP, IB_QPT_RC);
	if (IS_ERR(cb->cm_id)) {
		ret = PTR_ERR(cb->cm_id);
		cb->cm_id = NULL;
		rdma_error("rdma_create_id error %d\n", ret);
		return ret;
	}
	debug("created cm_id %p\n", cb->cm_id);

	ret = krdma_resolve_remote(cb, server, port);
	if (ret)
		return ret;

	/* 
	* Allocate pd, cq, qp and buffers. Whatever gets allocated is freed
	* by the caller through krdma_free_cb(), on success and on failure.
	*/
	ret = krdma_init_cb(cb);
	if (ret)
		return ret;

	return krdma_setup_buf(cb);
}

static int client_connect_to_server(struct krdma_cb *cb)
{
	struct rdma_conn_param conn_param;
	int ret;

	memset(&conn_param, 0, sizeof conn_param);
	conn_param.responder_resources = 3;
	conn_param.initiator_depth = 3;
	conn_param.retry_count = 3;
	/* 7 = keep retrying if the server has no receive buffer posted yet */
	conn_param.rnr_retry_count = 7;

	ret = rdma_connect(cb->cm_id, &conn_param);
	if (ret) {
		rdma_error("rdma_connect error %d\n", ret);
		return ret;
	}

	ret = krdma_wait_state(cb, KRDMA_CONNECTED);
	if (ret) {
		rdma_error("connection was not established, ret %d\n", ret);
		return ret;
	}

	debug("rdma_connect successful\n");
	return 0;
}

/* Exchange buffer metadata with the server. The client sends its, and then receives
 * from the server. The client-side metadata on the server is _not_ used because
 * this program is client driven. But it shown here how to do it for the illustration
 * purposes
 */
static int client_xchange_metadata_with_server(struct krdma_cb *cb)
{
	struct ib_device *dev = cb->pd->device;
	struct krdma_client_info *info = cb->send_buf;
	struct ib_sge server_recv_sge[1];
	struct ib_recv_wr server_recv_wr;
	const struct ib_recv_wr *bad_recv_wr = NULL;
	struct ib_sge client_send_sge[1];
	struct ib_send_wr client_send_wr;
	const struct ib_send_wr *bad_send_wr = NULL;
	int ret;

	// get ready for receiving server info
	memset(server_recv_sge, 0, sizeof(server_recv_sge));
	memset(&server_recv_wr, 0, sizeof(server_recv_wr));

	server_recv_sge[0].addr = cb->recv_dma_addr;
	server_recv_sge[0].length = sizeof(struct krdma_server_info);
	server_recv_sge[0].lkey = cb->pd->local_dma_lkey;
	server_recv_wr.sg_list = server_recv_sge;
	server_recv_wr.num_sge = 1;

	ret = ib_post_recv(cb->qp /* which QP */,
		      &server_recv_wr /* receive work request*/,
		      &bad_recv_wr /* error WRs */);
	if (ret) {
		rdma_error("Failed to pre-post the receive buffer, errno: %d \n", ret);
		return ret;
	}
	debug("Receive buffer posting is successful \n");

	// now preparing data to send to server
	memset(client_send_sge, 0, sizeof(client_send_sge));
	memset(&client_send_wr, 0, sizeof(client_send_wr));

	/* the send buffer is mapped: take it back before writing to it ... */
	ib_dma_sync_single_for_cpu(dev, cb->send_dma_addr, sizeof(*info),
			DMA_TO_DEVICE);
	info->token1 = string_to_uint64(this_client_token1);
	info->token2 = (uint32_t)(string_to_uint64(this_client_token2) >> 32);
	// request server to allocate a buffer with info->size
	info->size = cb->rdma_buf_size;
	/* ... and hand it over to the device again before posting */
	ib_dma_sync_single_for_device(dev, cb->send_dma_addr, sizeof(*info),
			DMA_TO_DEVICE);

	client_send_sge[0].addr = cb->send_dma_addr;
	client_send_sge[0].length = sizeof(struct krdma_client_info);
	client_send_sge[0].lkey = cb->pd->local_dma_lkey;
	client_send_wr.sg_list = client_send_sge;
	client_send_wr.num_sge = 1;
	client_send_wr.opcode = IB_WR_SEND;
	client_send_wr.send_flags = IB_SEND_SIGNALED;

	/* Now we post it */
	ret = ib_post_send(cb->qp, 
		    	&client_send_wr,
	       		&bad_send_wr);
	if (ret) {
		rdma_error("Failed to send client metadata, ret: %d \n", ret);
		return ret;
	}
	debug("Send posting is successful \n");

	/* at this point we are expecting 2 work completion. One for our 
	 * send and one for recv that we will get from the server for 
	 * its buffer information. Only the recv moves the state forward. */
	ret = krdma_wait_state(cb, KRDMA_RECV_DONE);
	if (ret) {
		rdma_error("Failed to receive server metadata, ret: %d \n", ret);
		return ret;
	}

	/* the device has written the recv buffer: sync before reading it */
	ib_dma_sync_single_for_cpu(dev, cb->recv_dma_addr,
			sizeof(*cb->recv_buf), DMA_FROM_DEVICE);
	cb->server = *cb->recv_buf;

	debug("Server sent us its buffer location and credentials, showing \n");
	debug("---------------------------------------------------------\n");
	debug("buffer info, addr: 0x%llx , len: %u , rkey : 0x%x \n",
			(unsigned long long)cb->server.addr, cb->server.size,
			cb->server.rkey);
	debug("---------------------------------------------------------\n");

	if (cb->server.size < cb->rdma_buf_size) {
		rdma_error("server buffer too small: %u, need %u\n",
				cb->server.size, cb->rdma_buf_size);
		return -EINVAL;
	}

	return 0;
}

/* This function does :
 * 1) RDMA write from the local write buffer -> remote buffer 
 * 2) RDMA read from remote bufer -> the local read buffer
 */ 
static int client_remote_memory_ops(struct krdma_cb *cb) 
{
	struct ib_device *dev = cb->pd->device;
	struct ib_sge client_rdma_sge[1];
	struct ib_rdma_wr client_rdma_wr;
	const struct ib_send_wr *bad_send_wr = NULL;
	int ret;

	// copy data to the write buffer, which is mapped for the device
	ib_dma_sync_single_for_cpu(dev, cb->rdma_wbuf_dma_addr,
			cb->rdma_buf_size, DMA_TO_DEVICE);
	memcpy(cb->rdma_write_buf, test_string, sizeof(test_string));
	ib_dma_sync_single_for_device(dev, cb->rdma_wbuf_dma_addr,
			cb->rdma_buf_size, DMA_TO_DEVICE);

	memset(client_rdma_sge, 0, sizeof(client_rdma_sge));
	memset(&client_rdma_wr, 0, sizeof(client_rdma_wr));

	/* Step 1: is to copy the local buffer into the remote buffer. */
	/* now we fill up SGE */
	client_rdma_sge[0].addr = cb->rdma_wbuf_dma_addr;
	client_rdma_sge[0].length = (uint32_t) sizeof(test_string);
	client_rdma_sge[0].lkey = cb->pd->local_dma_lkey;
	/* now we link to the rdma work request */
	client_rdma_wr.wr.sg_list = client_rdma_sge;
	client_rdma_wr.wr.num_sge = 1;
	client_rdma_wr.wr.opcode = IB_WR_RDMA_WRITE;
	client_rdma_wr.wr.send_flags = IB_SEND_SIGNALED;
	/* we have to tell server side info for RDMA */
	client_rdma_wr.rkey = cb->server.rkey;
	client_rdma_wr.remote_addr = cb->server.addr;
	/* Now we post it */
	ret = ib_post_send(cb->qp, &client_rdma_wr.wr, &bad_send_wr);
	if (ret) {
		rdma_error("Failed to write client src buffer, ret %d \n", 
				ret);
		return ret;
	}
	/* at this point we are expecting 1 work completion for the write */
	ret = krdma_wait_state(cb, KRDMA_WRITE_COMPLETE);
	if (ret) {
		rdma_error("RDMA WRITE did not complete, ret %d \n", ret);
		return ret;
	}

	debug("Client side WRITE is complete \n");

	/* Now we prepare a READ using same variables but for destination */
	client_rdma_sge[0].addr = cb->rdma_rbuf_dma_addr;
	client_rdma_sge[0].length = cb->rdma_buf_size;
	client_rdma_sge[0].lkey = cb->pd->local_dma_lkey;
	/* now we link to the rdma work request */
	client_rdma_wr.wr.sg_list = client_rdma_sge;
	client_rdma_wr.wr.num_sge = 1;
	client_rdma_wr.wr.opcode = IB_WR_RDMA_READ;
	client_rdma_wr.wr.send_flags = IB_SEND_SIGNALED;
	/* we have to tell server side info for RDMA */
	client_rdma_wr.remote_addr = cb->server.addr;
	client_rdma_wr.rkey = cb->server.rkey;
	/* Now we post it */
	ret = ib_post_send(cb->qp, 
		       &client_rdma_wr.wr, &bad_send_wr);
	if (ret) {
		rdma_error("Failed to read client dst buffer from the master, ret %d\n", 
				ret);
		return ret;
	}
	/* at this point we are expecting 1 work completion for the read */
	ret = krdma_wait_state(cb, KRDMA_READ_COMPLETE);
	if (ret) {
		rdma_error("RDMA READ did not complete, ret %d \n", ret);
		return ret;
	}

	debug("Client side READ is complete \n");

	/* the device has written the read buffer: sync before looking at it */
	ib_dma_sync_single_for_cpu(dev, cb->rdma_rbuf_dma_addr,
			cb->rdma_buf_size, DMA_FROM_DEVICE);

	return 0;
}

/* Runs the whole example once. All resources are released before returning. */
static int client_run(void)
{
	struct krdma_cb *cb;
	int ret;

	cb = krdma_alloc_cb();
	if (!cb) {
		rdma_error("alloc cb fail\n");
		return -ENOMEM;
	}

	ret = client_init_rdma(cb);
	if (ret) { 
		rdma_error("Failed to initialize RDMA resources, ret = %d \n", ret);
		goto exit;
	}

	ret = client_connect_to_server(cb);
	if (ret) { 
		rdma_error("Failed to setup client connection , ret = %d \n", ret);
		goto exit;
	}

	ret = client_xchange_metadata_with_server(cb);
	if (ret) {
		rdma_error("Failed to exchange metadata , ret = %d \n", ret);
		goto exit;
	}

	ret = client_remote_memory_ops(cb);
	if (ret) {
		rdma_error("Failed to finish remote memory ops, ret = %d \n", ret);
		goto exit;
	}

	/* This is our testing step */
	if (memcmp(cb->rdma_write_buf, cb->rdma_read_buf, sizeof(test_string))) {
		rdma_error("src and dst buffers do not match \n");
		ret = -EIO;
	} else {
		printk(KERN_INFO "rdma_kclient: SUCCESS, source and destination buffers match \n");
	}

exit:
	/* disconnects and frees up pd, cq, qp and buffers */
	krdma_free_cb(cb);
	return ret;
}

static int client_main(void * data) {
	int ret;

	ret = client_run();
	if (ret) {
		debug("error occurred, abort, ret = %d\n", ret);
	} else {
		debug("quit normally\n");
	}

	/*
	 * The work is done, but do not return yet: kclient_exit() calls
	 * kthread_stop(), which needs the thread to still exist. Sleep until
	 * the module is unloaded.
	 */
	set_current_state(TASK_INTERRUPTIBLE);
	while (!kthread_should_stop()) {
		schedule();
		set_current_state(TASK_INTERRUPTIBLE);
	}
	__set_current_state(TASK_RUNNING);

	return ret;
}

/////////////////////////////////////////////////////////////

static int __init kclient_init(void) {
	struct sockaddr_in addr;
	int ret;

	/* fail the insmod right away instead of connecting to 0.0.0.0 */
	ret = krdma_set_addr(&addr, server, port);
	if (ret) {
		rdma_error("missing or invalid parameters, usage: "
			"insmod rdma_kclient.ko server=<ipv4 address> [port=<port>]\n");
		return ret;
	}

	thread = kthread_run(client_main, NULL, "krdma_client");
	if (IS_ERR(thread)) {
		rdma_error("start thead failed.\n");
		ret = PTR_ERR(thread);
		thread = NULL;
		return ret;
	}
    return 0;
}

static void __exit kclient_exit(void) {
	/*
	 * The thread parks itself when it is done, so it is always there to
	 * be stopped. If it is still talking to the server this waits until
	 * the current step finishes or times out (KRDMA_WAIT_TIMEOUT_MS).
	 */
	if (thread)
		kthread_stop(thread);
}

module_init(kclient_init);
module_exit(kclient_exit);
MODULE_AUTHOR("Leon Zhao <xleonzhao@gmail.com>");
MODULE_DESCRIPTION("RDMA client kernel module");
MODULE_LICENSE("GPL v2");
