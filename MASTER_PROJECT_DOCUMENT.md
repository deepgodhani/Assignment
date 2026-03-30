# Master Project Document
## Linux Kernel Character Device Driver — Thread-Safe Blocking Circular Queue

---

## 1. Project Overview

### What the Project Does
A **loadable Linux kernel module (LKM)** that exposes a **character device** (`/dev/deep`) to user-space processes. The device internally manages a **thread-safe, blocking circular queue (ring buffer)** of variable-capacity. User-space programs communicate with the driver entirely through three custom `ioctl` commands: configure the queue, push data, and pop data.

### Problem It Solves
- Demonstrates **inter-process communication (IPC) via a shared kernel-space queue**: multiple user-space processes can act as producers and consumers without sharing any user-space memory.
- Provides **back-pressure**: producers block when the queue is full; consumers block when the queue is empty — no busy-waiting, no data loss.
- Encapsulates a production-grade kernel programming pattern: dynamic memory management, synchronisation primitives, and safe user ↔ kernel data transfer.

### Target Users
- Embedded / systems engineers building device drivers or custom IPC channels.
- Kernel module developers learning Linux driver infrastructure.
- Interviewers evaluating deep OS / systems programming knowledge.

---

## 2. Key Features

- **Dynamic queue sizing** — capacity is set at runtime via a single `ioctl`, not hardcoded.
- **Blocking producer/consumer** — uses `wait_queue_head_t` so threads sleep (no CPU spin) when the queue is full or empty.
- **Mutex-protected critical sections** — all queue-state mutations are serialised with `struct mutex`.
- **Variable-length messages** — each entry carries an arbitrary byte payload (`char *data` + `int length`), not a fixed-size slot.
- **Interruptible waits** — `wait_event_interruptible` / `mutex_lock_interruptible` allow signals (e.g. `SIGINT`) to wake sleeping processes cleanly.
- **Proper resource cleanup** — `module_exit` drains the queue and frees all kernel heap allocations.
- **Three companion user-space tools** — `configurator`, `filler`, `reader` — that exercise the full ioctl API.
- **GPL-licensed kernel module** — correct licensing for use of GPL-only kernel symbols.

---

## 3. Tech Stack

| Layer | Technology | Why |
|---|---|---|
| Language (kernel) | **C (GNU C / kernel dialect)** | Only C is supported for in-kernel code |
| Kernel API | **Linux Kernel (LKM API)** | `cdev`, `device_create`, `class_create`, `ioctl` dispatch |
| Synchronisation | **`struct mutex`** | Sleeping lock — correct for potentially long critical sections |
| Blocking I/O | **`wait_queue_head_t`** | Efficient, interruptible sleep-wait without CPU spinning |
| Memory management | **`kmalloc` / `kzalloc` / `kfree`** | Kernel slab allocator for dynamic kernel-heap allocations |
| User↔Kernel transfer | **`copy_from_user` / `copy_to_user`** | Safe access — validates user pointers and handles page faults |
| ioctl encoding | **`_IOW` / `_IOWR` macros** | Standard kernel ioctl command encoding (encodes direction + size) |
| Build system | **`Kbuild` (kernel Makefile)** | Standard out-of-tree module build system |
| User-space (test tools) | **C (POSIX)** | `open`, `ioctl`, `malloc`, `free` — no external libraries needed |

---

## 4. Architecture / System Design

### High-Level Design

```
User Space                         Kernel Space
─────────────────────────────────────────────────────────────
 configurator ─┐
 filler        ├──  /dev/deep  ──▶  chardev driver (chdev.c)
 reader        ┘     (ioctl)         │
                                     ├─ struct cq (circular queue)
                                     │   ├─ items[]  (heap-allocated slots)
                                     │   ├─ mutex    (serialises head/tail/size)
                                     │   ├─ read_q   (consumers sleep here)
                                     │   └─ write_q  (producers sleep here)
                                     └─ file_operations.unlocked_ioctl
```

### Data Flow

**Initialisation (`SET_SIZE_OF_QUEUE`):**
1. User calls `ioctl(fd, SET_SIZE_OF_QUEUE, &n)`.
2. Kernel copies `n` from user space, allocates `struct cq` + `queue_item[n]` array on the kernel heap, initialises mutex and wait-queues.

**Push (`PUSH_DATA`):**
1. User fills `struct user_data { int length; char *data; }` and calls `ioctl(fd, PUSH_DATA, &ud)`.
2. Kernel acquires mutex; if `size == capacity`, releases mutex and sleeps on `write_q`.
3. On wake, re-acquires mutex, `kmalloc`s a new slot, `copy_from_user` copies payload, advances `head` (modulo capacity), increments `size`.
4. Releases mutex, wakes any sleepers on `read_q`.

**Pop (`POP_DATA`):**
1. User allocates a buffer, sets `user_data.length`, calls `ioctl(fd, POP_DATA, &ud)`.
2. Kernel acquires mutex; if `size == 0`, sleeps on `read_q`.
3. Validates buffer length ≥ slot length, `copy_to_user` copies payload + actual length back, `kfree`s slot, advances `tail`, decrements `size`.
4. Releases mutex, wakes any sleepers on `write_q`.

### Patterns Used
- **Producer–Consumer** (classic bounded-buffer problem)
- **Character Device Driver** pattern (Linux `cdev` + `file_operations`)
- **RAII-style cleanup** in module exit (drain, free items, free queue, destroy device/class)

---

## 5. My Contributions

- **Designed and implemented the entire kernel module** (`chdev.c`) from scratch, including device registration, ioctl dispatch, and circular-queue logic.
- **Chose `struct mutex` over spinlocks** — the critical section can sleep (via `wait_event_interruptible`), making spinlocks incorrect; mutex is the right primitive.
- **Designed the ioctl API surface** — three distinct commands, proper `_IOW`/`_IOWR` encoding, and a shared `user_data` struct for variable-length message passing.
- **Implemented blocking semantics** using `wait_queue_head_t` and interruptible variants so signals are handled gracefully without leaving the mutex locked.
- **Wrote three user-space test programs** (`configurator.c`, `filler.c`, `reader.c`) to demonstrate and validate the full driver lifecycle.
- **Wrote the Kbuild Makefile** for out-of-tree module compilation.

---

## 6. Challenges & Solutions

### Challenge 1: Sleeping inside a mutex-held section is illegal
- **Problem:** Cannot call `wait_event_interruptible` while holding a mutex — this would deadlock.
- **Solution:** Release the mutex *before* sleeping (`mutex_unlock(&gq->lock)`), then re-acquire it in the wake-up path with `mutex_lock_interruptible`. This follows the standard Linux wait-loop idiom.
- **Trade-off:** The queue state must be re-checked after every wake-up (loop condition), because another thread may have consumed the slot between the wake and the re-acquire.

### Challenge 2: Safe user ↔ kernel memory transfer
- **Problem:** Directly dereferencing a user-space pointer in kernel mode causes kernel oops or security vulnerabilities.
- **Solution:** All user-space data is transferred through `copy_from_user` / `copy_to_user`. Both functions validate the pointer and handle faults safely.
- **Trade-off:** Two separate copies (struct metadata + payload data) are needed per operation, but this is unavoidable with variable-length messages.

### Challenge 3: Signal interruption during blocking wait
- **Problem:** A blocked `ioctl` call must return promptly when a signal (e.g. `SIGINT`) arrives, not hang forever.
- **Solution:** Used `wait_event_interruptible` and `mutex_lock_interruptible`, which return `-ERESTARTSYS` on signal delivery — the kernel then either restarts or reports `EINTR` to user space.

### Challenge 4: Memory leak on module unload with non-empty queue
- **Problem:** If the module is unloaded while items remain in the queue, their `data` allocations are leaked.
- **Solution:** The `chardev_exit` function iterates over all live items (`size` entries starting from `tail`), `kfree`s each payload, then frees the `items` array and the `cq` struct itself.

---

## 7. Impact & Results

- **Zero busy-wait CPU usage**: blocking `ioctl` consumers/producers consume 0% CPU while waiting, compared to a polling design that would peg a CPU core.
- **Correct multi-process behaviour**: multiple concurrent producers and consumers operate safely due to mutex-protected state transitions and wait-queue wakeup.
- **Clean resource lifecycle**: no kernel memory leaks on module unload, validated by `kmemleak` compatibility patterns.
- **Portable build**: out-of-tree Kbuild structure compiles against any installed kernel headers without patching the kernel source tree.
- **Practical IPC primitive**: demonstrates a complete, production-pattern kernel IPC channel usable as a foundation for real embedded product drivers.

---

## 8. Code Highlights

### Circular Buffer Index Arithmetic
```c
gq->head = (gq->head + 1) % gq->capacity;  // wrap-around without branching
gq->tail = (gq->tail + 1) % gq->capacity;
```
- O(1) enqueue and dequeue with modulo wrapping — no shifting, no dynamic resize.

### Wait-Loop Idiom (Producer)
```c
while (gq->size == gq->capacity) {
    mutex_unlock(&gq->lock);
    if (wait_event_interruptible(gq->write_q, gq->size < gq->capacity))
        return -ERESTARTSYS;    // signal received — abort cleanly
    if (mutex_lock_interruptible(&gq->lock))
        return -ERESTARTSYS;
}
```
- Textbook Linux kernel producer–consumer: release lock → sleep → re-acquire → re-check condition.
- The `while` (not `if`) guards against spurious wakeups.

### ioctl Command Encoding
```c
#define SET_SIZE_OF_QUEUE  _IOW('a', 'a', int *)
#define PUSH_DATA          _IOW('a', 'b', struct user_data)
#define POP_DATA           _IOWR('a', 'c', struct user_data)
```
- `_IOW` = write to kernel; `_IOWR` = write + read back. Encodes magic, ordinal, and transfer size into a 32-bit command number, preventing collisions with other drivers.

### Module Cleanup (Drain on Exit)
```c
for (int i = 0; i < gq->size; i++) {
    kfree(gq->items[(gq->tail + i) % gq->capacity].data);
}
kfree(gq->items);
kfree(gq);
```
- Iterates from `tail` using modulo arithmetic — same circular-buffer logic applied to cleanup.

---

## 9. Scalability & Improvements

### Scalability Paths
- **Per-file-descriptor queues**: allocate a separate `cq` per `open()` call (store in `file->private_data`) to support multiple independent queues from different processes.
- **Multiple minor devices**: use `alloc_chrdev_region` with `count > 1` and a minor-to-queue mapping for N independent named queues.
- **`mmap` interface**: for very high-throughput scenarios, expose the ring buffer via `mmap` to eliminate the `copy_from/to_user` overhead.

### Future Enhancements
- [ ] **`read`/`write` file operations** in addition to `ioctl`, enabling standard `cat`, `echo`, and pipe usage.
- [ ] **`poll`/`select` support** (`file_operations.poll`) so user-space event loops can monitor the device with `epoll`.
- [ ] **Configurable per-entry max size** to prevent unbounded kernel heap allocations.
- [ ] **`sysfs` or `debugfs` statistics** (current size, capacity, total pushed/popped) for observability.
- [ ] **`O_NONBLOCK` support**: return `-EAGAIN` instead of sleeping when the flag is set.
- [ ] **RCU or lock-free ring buffer** for extreme single-producer/single-consumer throughput.
- [ ] **Unit tests with KUnit** (Linux kernel testing framework) for in-kernel verification.

---

## 10. Resume-Ready Bullet Points

- **Designed and implemented a Linux loadable kernel module** in C exposing a thread-safe, blocking circular-queue IPC channel via a custom `ioctl` API, eliminating busy-wait CPU overhead with `wait_queue_head_t` sleep-wait semantics.
- **Engineered a producer–consumer synchronisation model** using `struct mutex` and interruptible wait queues, correctly handling mutex-release-before-sleep patterns and POSIX signal interruption (`ERESTARTSYS`) in kernel context.
- **Applied safe user↔kernel memory transfer** via `copy_from_user` / `copy_to_user` with full error-path cleanup, preventing kernel oops and ensuring no memory leaks on module unload.
- **Built an out-of-tree kernel module** with a `Kbuild` Makefile and three companion POSIX user-space test tools (`configurator`, `filler`, `reader`), demonstrating the complete driver lifecycle from `insmod` to `rmmod`.
- **Implemented dynamic runtime queue configuration** via `_IOW`/`_IOWR`-encoded ioctl commands, enabling variable-capacity ring buffers allocated on the kernel heap without recompiling the module.

---

## 11. Interview Talking Points

### "Walk me through what this project does."
> "It's a Linux kernel character device driver that implements a bounded, blocking queue. User-space processes open `/dev/deep` and use three ioctl commands: one to set the queue capacity, one to push a variable-length message, and one to pop. The driver lives in kernel space and handles concurrency with a mutex and wait queues — producers sleep when the queue is full, consumers sleep when it's empty. No polling, no busy-wait."

### "Why ioctl instead of read/write?"
> "I chose ioctl because I needed to pass a struct (length + pointer) in a single syscall, which the standard `read`/`write` interface doesn't support cleanly for variable-length messages. A proper next step would be to *also* implement `read`/`write` for POSIX composability and add `poll` so user-space event loops can watch the fd."

### "Why mutex and not spinlock?"
> "The critical section calls `wait_event_interruptible`, which can sleep. Spinlocks cannot be held across a sleep — that would cause a deadlock or BUG() in the kernel. Mutexes are sleeping locks, so they're the correct choice here. Spinlocks would only be appropriate if I were in an interrupt context or needed to hold the lock for just a few instructions."

### "How do you handle a signal while blocked in ioctl?"
> "I use `wait_event_interruptible` and `mutex_lock_interruptible`. Both return `-ERESTARTSYS` if a signal arrives while the thread is sleeping. The ioctl then propagates that return value up to the syscall layer, which either restarts the syscall transparently or returns `-EINTR` to user space, depending on the signal handler's `SA_RESTART` flag."

### "What happens when you unload the module with data still in the queue?"
> "The `module_exit` function iterates over all live items — `gq->size` entries starting at `gq->tail` using the same modulo arithmetic as normal dequeue — and `kfree`s each payload. Then it frees the items array and the queue struct itself, destroys the device and class, and unregisters the chrdev region. No leaks."

### "How would you extend this to support multiple independent queues?"
> "Two approaches: (1) allocate the `cq` in the `open()` file operation and store it in `file->private_data` — each open fd gets its own queue. (2) Register multiple minor device numbers and maintain an array of queue pointers, one per minor. The first is simpler for per-process isolation; the second is better for named, system-wide shared queues."

---

## Bonus: Suggested Improvements to Stand Out at Top Companies

| Improvement | Why It Matters |
|---|---|
| Add `poll` / `epoll` support | Enables integration with async event loops (libuv, io_uring) |
| Add `O_NONBLOCK` mode | Standard POSIX contract — expected by any seasoned systems engineer |
| Write KUnit tests | Shows TDD discipline for kernel code; rare and impressive |
| Add `debugfs` stats node | Observability is a production requirement, not an afterthought |
| Expose via `read`/`write` + `ioctl` | Shows awareness of POSIX composability (piping, shell tools) |
| Benchmark with `perf` / `ftrace` | Demonstrates performance engineering mindset |
| Add per-fd queue support | Shows scalable design thinking beyond single global state |
| Document with kernel-doc comments | Matches Linux upstream contribution standards |
