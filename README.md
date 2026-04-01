# Linux Kernel Character Device Queue Driver

A Linux loadable kernel module that exposes a thread-safe, blocking circular queue to user space via `ioctl`.

---

## Demo / Architecture

```
  User Space                        Kernel Space
  ──────────────────────────────────────────────────────
  configurator  ──┐
  filler        ──┼── ioctl(/dev/deep) ──► chardev (chdev.c)
  reader        ──┘                               │
                                          ┌───────▼────────┐
                                          │  Circular Queue │
                                          │  (mutex + wait  │
                                          │   queues)       │
                                          └────────────────-┘

  ioctl commands:
    SET_SIZE_OF_QUEUE  →  allocate queue with N slots
    PUSH_DATA          →  enqueue a variable-length byte blob
    POP_DATA           →  dequeue a byte blob (blocks if empty)
```

---

## Why I Built This

Standard kernel FIFOs (e.g., `kfifo`) are fixed-width. This driver lets user-space processes exchange **variable-length binary messages** through a named device node (`/dev/deep`) using three simple `ioctl` commands, demonstrating how to build a proper producer-consumer pipeline entirely in kernel space — complete with blocking, wake-ups, and safe memory management.

---

## Key Technical Highlights

- **Blocking producer/consumer** — writers sleep on `write_q` when the queue is full; readers sleep on `read_q` when it is empty, using `wait_event_interruptible`.
- **Mutex-protected critical section** — every enqueue/dequeue is guarded by a `struct mutex`, preventing data races between concurrent user-space callers.
- **Variable-length messages** — each slot stores an independently `kmalloc`-ed buffer whose size is passed by the caller, not fixed at compile time.
- **Dynamic capacity** — `SET_SIZE_OF_QUEUE` ioctl configures the ring buffer at runtime; the driver rejects a second configuration attempt with `EBUSY`.
- **Clean teardown** — `chardev_exit` walks the live portion of the ring and `kfree`s every pending item before unregistering the device, preventing memory leaks on `rmmod`.

---

## Tech Stack

| Layer | Technology |
|---|---|
| Language | C (GNU C / kernel dialect) |
| Environment | Linux Kernel Module (LKM) |
| Kernel APIs | `cdev`, `device_create`, `mutex`, `wait_queue`, `copy_to/from_user` |
| Build system | Kbuild (`Makefile` targeting `obj-m`) |
| User-space tools | POSIX C (`open`, `ioctl`, `malloc`) |
| IPC mechanism | `ioctl` on a character device node |

---

## How to Run Locally

### Prerequisites

- Linux system (x86-64 or ARM)
- Kernel headers for the **running** kernel:
  ```bash
  sudo apt install linux-headers-$(uname -r) build-essential   # Debian/Ubuntu
  sudo dnf install kernel-devel gcc                             # Fedora/RHEL
  ```

### 1 — Build the kernel module

```bash
make          # produces chardev.ko
```

### 2 — Load the module

```bash
sudo insmod chardev.ko
# Verify it loaded:
dmesg | tail -5
# Confirm device node:
ls -l /dev/deep
```

### 3 — Build the user-space tools

```bash
gcc -o configurator configurator.c
gcc -o filler       filler.c
gcc -o reader       reader.c
```

### 4 — Run the demo

```bash
# Step 1: configure queue with 100 slots
sudo ./configurator

# Step 2 (terminal A): push data
sudo ./filler

# Step 3 (terminal B): pop data
sudo ./reader
```

### 5 — Unload the module

```bash
sudo rmmod chardev
dmesg | tail -5    # should show "Module unloaded."
```

---

## Architecture Overview

The driver registers a single character device (`/dev/deep`, major number allocated dynamically) and exposes three `ioctl` commands:

| Command | Direction | Description |
|---|---|---|
| `SET_SIZE_OF_QUEUE` | user → kernel | Allocates the ring buffer (`struct cq`) with *N* `queue_item` slots |
| `PUSH_DATA` | user → kernel | Copies a `user_data` struct (length + pointer) into the next free slot; blocks if full |
| `POP_DATA` | kernel → user | Copies the oldest item back to user space and frees kernel memory; blocks if empty |

All concurrency is handled with a single `struct mutex` plus two `wait_queue_head_t` objects (`read_q`, `write_q`).  The circular ring is indexed with `head` (write pointer) and `tail` (read pointer) modulo `capacity`.

---

## Known Limitations / What I'd Improve

- **Single global queue** — the queue is process-agnostic; multiple processes share the same ring buffer. A per-`open` queue (using `file->private_data`) would isolate producers from consumers.
- **No `read`/`write` file ops** — the driver only supports `ioctl`. Implementing `read`/`write` would make it usable with standard shell tools (`cat`, `echo`).
- **No max message size cap** — a malicious caller can request an arbitrarily large `kmalloc`, potentially exhausting kernel memory.
- **`configurator` hardcodes size 100** — the queue size should be a command-line argument.
- **No `poll`/`select` support** — adding `poll_wait` would allow event-driven user-space applications instead of blocking indefinitely.
- **Single device instance** — extending to multiple minor numbers would allow isolated queues for different use cases.
