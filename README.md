# rdma-client-kernel-module

An RDMA client that runs **inside the Linux kernel**, talking to an ordinary user-space RDMA server.

## Summary

This repository is a fork of [animeshtrivedi/rdma-example](https://github.com/animeshtrivedi/rdma-example), a small, well-commented user-space RDMA client/server pair.

**What this fork adds** is `kclient/`: the same client, rewritten as a loadable kernel module (`rdma_kclient.ko`) on top of the in-kernel verbs and RDMA CM APIs.

- **Kernel-space client, user-space server.** The module connects to the unmodified `rdma_server`, so you can compare the kernel and user-space code paths side by side for exactly the same exchange.
- **Full round trip.** It connects, exchanges buffer metadata with SEND/RECV, does an RDMA WRITE into the server's buffer, RDMA READs it back, and compares the two.
- **Defensive by design.** Every wait has a timeout, a dropped connection is reported as an error, all resources are released on every failure path, and the module can be unloaded at any time.

The kernel code borrows ideas and some code from [krdma](https://github.com/snake0/krdma) and [krping](https://github.com/larrystevenwise/krping). The original user-space example in `src/` is unchanged.

## Install

### Requirements

- Linux with headers for the running kernel (`/lib/modules/$(uname -r)/build`). The module uses the verbs API as it has been since kernel 4.19.
- `gcc`, `make`, `cmake`.
- `rdma-core` development files (`libibverbs`, `librdmacm`) for the user-space server.
- An RDMA-capable network adapter (InfiniBand, RoCE or iWARP) on both ends.

### Build

```bash
git clone https://github.com/xleonzhao/rdma-client-kernel-module.git
cd rdma-client-kernel-module

# user-space server (and the original user-space client) -> ./bin
cmake .
make

# kernel client -> kclient/rdma_kclient.ko
cd kclient
make
```

`make debug` in `kclient/` builds the module with debug info.

### Run

1. Start the server on the machine that owns `<server-ip>`:

   ```bash
   ./bin/rdma_server -a <server-ip> [-p <port>]
   ```

2. Load the kernel client. It runs once, as soon as it is loaded:

   ```bash
   sudo modprobe rdma_cm
   sudo insmod kclient/rdma_kclient.ko server=<server-ip> [port=<port>]
   ```

3. Read the result from the kernel log:

   ```bash
   sudo dmesg | tail -n 40
   ```

   A good run ends with:

   ```text
   rdma_kclient: SUCCESS, source and destination buffers match
   ```

4. Unload the module. To run again, restart the server and load the module again.

   ```bash
   sudo rmmod rdma_kclient
   ```

| Parameter | Required | Default | Meaning |
|---|---|---|---|
| `server` | yes | – | IPv4 address of the RDMA server |
| `port` | no | `20886` | TCP port the server listens on |

`insmod` fails with `-EINVAL` if `server` is missing or not a valid IPv4 address.

The original user-space client still works against the same server:

```bash
./bin/rdma_client -a <server-ip> -s textstring
```

### Troubleshooting

| Symptom (`insmod` output or `dmesg`) | Likely cause |
|---|---|
| `insmod: ... Invalid parameters` | `server=` missing or malformed |
| `rdma_resolve_addr did not complete` | no RDMA device can reach `<server-ip>` (check `rdma link show`) |
| `connection was not established` | server not running, wrong port, or a firewall |
| `timed out waiting for state N` | the server stopped responding; each step waits at most 10 s |

## Technical Details

### Repository layout

```text
src/                  user-space example (unchanged from upstream)
  rdma_server.c         the server used by both clients
  rdma_client.c         the original user-space client
  rdma_common.[ch]      shared helpers and the wire format
kclient/              the kernel client (this fork)
  kclient.c             module entry points and the client's control flow
  krdma.[ch]            control block, CM/CQ event handlers, buffers, teardown
  Makefile              out-of-tree kbuild
CMakeLists.txt        builds src/ into ./bin
```

### Architecture

```text
  client machine                               server machine
 ┌────────────────────────────┐               ┌───────────────────────────┐
 │ kernel                     │               │ user space                │
 │  rdma_kclient.ko           │               │  rdma_server              │
 │   ├ kclient.c  (kthread)   │               │   librdmacm / libibverbs  │
 │   └ krdma.c    (helpers)   │               ├───────────────────────────┤
 │  rdma_cm  ·  ib_core       │               │ kernel: rdma_ucm, ib_core │
 │  device driver (e.g. mlx5) │◄── RDMA ─────►│ device driver             │
 └────────────────────────────┘               └───────────────────────────┘
```

The module has two layers:

- **`kclient.c` – what to do.** Module init/exit, parameter checks, a kernel thread, and the five steps of the example, written as straight-line code.
- **`krdma.c` – how to do it.** Owns the control block `struct krdma_cb`: connection id, protection domain, two completion queues, queue pair, the four buffers and the state machine. It also holds the two asynchronous callbacks.

### Control flow

`insmod` validates the parameters and starts the kthread `krdma_client`, which runs these steps in order. Each step blocks in `krdma_wait_state()` until the matching event arrives.

```text
 kernel client                                          user-space server
 ─────────────                                          ─────────────────
 1. rdma_create_id
    rdma_resolve_addr ──► wait ADDR_RESOLVED            listening
    rdma_resolve_route ─► wait ROUTE_RESOLVED
    alloc PD, send CQ, recv CQ, QP; map 4 buffers

 2. rdma_connect ───────── connect request ───────────► pre-post RECV, accept
    wait CONNECTED  ◄───── established ────────────────

 3. post RECV (for the server's reply)
    post SEND {token1, size=4096, token2} ────────────► allocate + register a
    wait RECV_DONE  ◄───── SEND {addr, length, stag} ── buffer of `size` bytes

 4. RDMA WRITE "hello, world" ────────────────────────► (server CPU not involved)
    wait WRITE_COMPLETE

 5. RDMA READ  ◄──────────────────────────────────────  (server CPU not involved)
    wait READ_COMPLETE
    memcmp(write buffer, read buffer) → SUCCESS / error

 6. rdma_disconnect ──────── disconnect ──────────────► clean up and exit
    free QP, buffers, CQs, PD, id
```

After step 6 the thread does not exit; it sleeps until `rmmod` so that `kthread_stop()` always finds a live thread.

### Events and the state machine

The kernel APIs are asynchronous. Two callbacks feed one state variable:

| Callback | Called from | Reports |
|---|---|---|
| `krdma_cma_event_handler()` | RDMA CM, process context | address/route resolved, established, rejected, disconnected, errors |
| `krdma_cq_event_handler()` | completion queue, interrupt context | completion of SEND, RECV, RDMA WRITE, RDMA READ, and failed or flushed work requests |

Both call `krdma_set_state()`, which updates `cb->state` under a spinlock and signals a completion. The states are:

```text
INIT → ADDR_RESOLVED → ROUTE_RESOLVED → CONNECTED → RECV_DONE → WRITE_COMPLETE → READ_COMPLETE

failure states (from any state, sticky): ERROR, CONNECT_REJECTED, DISCONNECTED
```

`krdma_wait_state(cb, want)` is the only place the thread sleeps. It:

- returns `0` when the state equals `want`;
- returns an error as soon as a failure state is seen, so a dropped connection can never be mistaken for progress;
- ignores wake-ups caused by unrelated events;
- gives up with `-ETIMEDOUT` after `KRDMA_WAIT_TIMEOUT_MS` (10 s).

Failure states are sticky: once the connection has failed, a late success event cannot overwrite it.

### Memory and DMA

| Buffer | Size | Direction | Used for |
|---|---|---|---|
| `send_buf` | 16 bytes | to device | client metadata (SEND) |
| `recv_buf` | 16 bytes | from device | server metadata (RECV) |
| `rdma_write_buf` | 1 page | to device | source of the RDMA WRITE |
| `rdma_read_buf` | 1 page | from device | destination of the RDMA READ |

- All four are `kmalloc`ed and mapped for the RDMA device with `ib_dma_map_single()`.
- The CPU only touches a mapped buffer between `ib_dma_sync_single_for_cpu()` and `ib_dma_sync_single_for_device()`.
- No memory region is registered on the client. Work requests use the protection domain's `local_dma_lkey`; only the server's buffer needs a remote key, and the server provides it.

### Wire format

The two metadata messages are 16 bytes each and share the layout of `struct rdma_buffer_attr` in `src/rdma_common.h`:

| Offset | Size | Client → server | Server → client |
|---|---|---|---|
| 0 | 8 | `token1` (demo value, ignored) | buffer address |
| 8 | 4 | `size`: buffer length requested | buffer length |
| 12 | 4 | `token2` (demo value, ignored) | stag (remote key) |

Fields are sent in host byte order, as in the upstream example, so both ends must have the same endianness.

### Teardown and unload

- `krdma_free_cb()` is the single cleanup path. It accepts a control block in any stage of setup and releases things in dependency order: disconnect → QP → buffers → CQs → PD → connection id → the control block itself.
- Setup functions never leave an error pointer in the control block; a failed member is `NULL`, which is how cleanup knows to skip it.
- `rmmod` calls `kthread_stop()`. If the client is still waiting on the server, unloading blocks until that step completes or times out (at most 10 s).

### User space vs. kernel, side by side

| Task | User space (`src/rdma_client.c`) | Kernel (`kclient/`) |
|---|---|---|
| CM events | event channel + `rdma_get_cm_event()` | callback passed to `rdma_create_id()` |
| Completions | completion channel + `ibv_get_cq_event()` / `ibv_poll_cq()` | CQ callback + `ib_poll_cq()` |
| Local buffers | `ibv_reg_mr()` for each buffer | `ib_dma_map_single()` + `local_dma_lkey` |
| Posting work | `ibv_post_send()` / `ibv_post_recv()` | `ib_post_send()` / `ib_post_recv()` |
| Waiting | blocking calls | completion + state machine with timeout |
| Output | `printf` | `printk` → `dmesg` |

### Limitations

- IPv4 only; one connection, one run per module load.
- The server handles a single client and then exits, so it must be restarted for each run.
- Same-endianness peers only (see *Wire format*).
- A demo, not a library: the payload is the fixed string `"hello, world"`.

## Credits and license

- Original example: Animesh Trivedi, [rdma-example](https://github.com/animeshtrivedi/rdma-example).
- Kernel helper code derived from [krdma](https://github.com/snake0/krdma) (Trusted Cloud Group, Shanghai Jiao Tong University, GPL v2) and [krping](https://github.com/larrystevenwise/krping).
- Kernel client: Leon Zhao. The kernel module is licensed under GPL v2; the rest of the repository is under the Apache License 2.0 (see `LICENSE`).
