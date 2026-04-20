# Multi-Container Runtime (Project Jackfruit)

**Team Members:**
- Pranav Hareesh Achar (PES1UG24AM196)
- Rahul Sharan Sharma (PES1UG24AM218)

**Environment:** 
This project was developed and tested on **Native Debian**. 
*Note: Development on Debian ensures compatibility with the required Ubuntu 22.04/24.04 environments while utilizing native Linux kernel features for namespaces and modules.*

---

## 1. Build, Load, and Run Instructions

This guide provides the exact sequence to reproduce all 8 project requirements.

### Prerequisites
- Debian/Ubuntu Linux
- `build-essential` and `linux-headers-$(uname -r)`
- Root (`sudo`) privileges

### Step 1: Preparation (One-time Setup)
Clone the repository and prepare the environment:
```bash
# 1. Build all binaries and the kernel module
cd boilerplate
make clean && make

# 2. Setup the Base Root Filesystem (Alpine Linux)
mkdir -p rootfs-base
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base

# 3. CRITICAL: Inject workload binaries into the base rootfs so containers can run them
cp cpu_hog io_pulse memory_hog rootfs-base/bin/

# 4. Create writable copies for individual containers
cp -a rootfs-base rootfs-alpha
cp -a rootfs-base rootfs-beta
```

### Step 2: Load Kernel Monitor
```bash
sudo insmod monitor.ko
# Verify the device exists
ls -l /dev/container_monitor
```

### Step 3: Start the Supervisor (Terminal A)
The supervisor must stay running. Open a terminal and run:
```bash
sudo ./engine supervisor ./rootfs-base
```

### Step 4: Execute Showcases (Terminal B)
Open a **new terminal** and run these specific commands to verify the requirements:

**A. Multi-Container & Metadata (Req #1, #2, #4)**
```bash
# Start background containers
sudo ./engine start alpha ./rootfs-alpha "/bin/cpu_hog 30"
sudo ./engine start beta ./rootfs-beta "/bin/cpu_hog 30"

# Verify tracking metadata
sudo ./engine ps
```

**B. Logging (Req #3)**
```bash
# View real-time logs captured through the bounded buffer
sudo ./engine logs alpha
```

**C. Memory Limits (Req #5, #6)**
```bash
# Start a container that exceeds 64MB hard limit
sudo ./engine start mem-demo ./rootfs-beta "/bin/memory_hog 80" --soft-mib 32 --hard-mib 64

# Wait 2 seconds, then check metadata to see "killed" status
sudo ./engine ps

# Check kernel logs for the enforcement event
sudo dmesg | tail -n 20
```

**D. Scheduling Experiment (Req #7)**
```bash
# Run a high-priority and low-priority task concurrently
sudo ./engine start high-pri ./rootfs-alpha "/bin/cpu_hog 10" --nice -20
sudo ./engine start low-pri ./rootfs-beta "/bin/cpu_hog 10" --nice 19

# Wait for completion and compare the 'accumulator' values in logs
sudo ./engine logs high-pri | tail -n 2
sudo ./engine logs low-pri | tail -n 2
```

**E. Teardown (Req #8)**
```bash
# Stop any remaining containers
sudo ./engine stop alpha
sudo ./engine ps

# Unload the kernel module
sudo rmmod monitor
```

---

## 2. Design Decisions and Tradeoffs

- **Namespace Isolation:** Used `CLONE_NEWPID`, `CLONE_NEWUTS`, and `CLONE_NEWNS` for robust isolation. `chroot` was chosen for filesystem isolation as it is sufficient for the project scope, though `pivot_root` would offer better security against escapes.
- **IPC Architecture:** 
    - **Control Plane:** Unix Domain Sockets were chosen for the CLI-to-Supervisor communication due to their stream-oriented nature and efficient local performance.
    - **Data Plane (Logging):** Pipes were used to capture container `stdout`/`stderr` as they integrate natively with `dup2` and `fork`/`clone`.
- **Logging Pipeline:** Implemented a **Bounded Buffer** (16 chunks of 4KB) with a dedicated consumer thread. This ensures that container processes are not blocked by slow disk I/O and prevents memory exhaustion if logging volume is high.
- **Kernel Monitor:** Implemented as an LKM with a 1-second timer. This allows for asynchronous memory enforcement that is difficult to bypass from user-space, though it introduces a slight monitoring delay (up to 1s).

---

## 3. Engineering Analysis

- **Isolation Mechanisms:** Namespaces virtualize kernel resources. The PID namespace makes the containerized process see itself as PID 1, while the Mount namespace + `chroot` prevents access to the host's files. The host kernel is still shared, meaning kernel exploits can theoretically affect all containers.
- **Process Lifecycle:** The supervisor process acts as the "init" for containers, reaping children via `waitpid` in a `SIGCHLD` handler. This prevents the accumulation of zombie processes and allows the supervisor to track the exact exit status and reason (normal vs killed).
- **Synchronization:** The logging pipeline uses `pthread_mutex` for atomicity and `pthread_cond_t` for signaling. This avoids "busy-waiting" and ensures that the consumer thread only wakes up when data is available, and producers only proceed when there is space in the buffer.
- **Memory Management:** RSS (Resident Set Size) is used for monitoring as it reflects actual physical memory usage. Soft limits provide a way to warn users before hard limits (enforced via `SIGKILL`) terminate the process to protect the host system's stability.

---

## 4. Demo Screenshots

| # | What to Demonstrate | What the Screenshot Must Show | Screenshot |
|---|---|---|---|
| 1 | Multi-container supervision | Two or more containers running under one supervisor process | ![Multi-container](screenshots/demo1.png) |
| 2 | Metadata tracking | Output of the `ps` command showing tracked container metadata | ![Metadata](screenshots/demo2.png) |
| 3 | Bounded-buffer logging | Log file contents captured through the logging pipeline, and evidence of the pipeline operating (e.g., producer/consumer activity) | ![Logging](screenshots/demo3.png) |
| 4 | CLI and IPC | A CLI command being issued and the supervisor responding, demonstrating the second IPC mechanism | ![CLI_IPC](screenshots/demo4.png) |
| 5 | Soft-limit warning | `dmesg` or log output showing a soft-limit warning event for a container | ![Soft-limit](screenshots/demo5.png) |
| 6 | Hard-limit enforcement | `dmesg` or log output showing a container being killed after exceeding its hard limit, and the supervisor metadata reflecting the kill | ![Hard-limit](screenshots/demo6.png) |
| 7 | Scheduling experiment | Terminal output or measurements from at least one scheduling experiment, with observable differences between configurations | ![Scheduling](screenshots/demo7.png) |
| 8 | Clean teardown | Evidence that containers are reaped, threads exit, and no zombies remain after shutdown (e.g., `ps aux` output, supervisor exit messages) | ![Teardown](screenshots/demo8.png) |

---

## 5. Scheduler Experiment Results

Experiments performed using `cpu_hog` (CPU-bound) and `io_pulse` (I/O-bound) workloads.

| Container | Workload | Nice Value | Observation |
|---|---|---|---|
| Alpha | `cpu_hog` | 0 | Higher CPU share and faster completion. |
| Beta | `cpu_hog` | 10 | Lower priority resulted in significant slowdown when competing with Alpha. |

*Analysis:* The Linux CFS (Completely Fair Scheduler) correctly allocated fewer vruntime slices to the container with the higher nice value, demonstrating effective priority enforcement.
