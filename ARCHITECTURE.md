# edu-pci-driver: Architecture and Learning Guide

A small Linux driver for a fake (emulated) PCI device. You run it in a virtual machine, so you need no hardware.

This file has two jobs:
1. **Part A and B** explain what the project IS (the big picture and the diagrams).
2. **Part C** explains what we DID, step by step, and WHY.
3. **Part D to F** are a glossary, a cheat sheet, and the mistakes we made (the best teachers).

## The KISS rules we followed

1. Build the smallest thing that works. Then add ONE thing.
2. Do not add the next thing until the last thing is tested.
3. If a test passes, ask: "could this test ever fail?" Then try to break the code on purpose to find out.
4. One file until it hurts. The whole driver is still one file (`driver/edu_driver.c`).

---

# PART A: The big picture in 60 seconds

## What is a driver?

A device (network card, GPU, disk) is just a box with **registers** (like numbered mailboxes). The CPU cannot "use" it by magic. A **driver** is the code that:

1. **Finds** the device.
2. **Talks** to it by writing and reading its registers.
3. **Listens** for it (the device says "I'm done" with an **interrupt**).
4. **Moves big data** with it without the CPU copying every byte (**DMA**).
5. **Offers** a simple door to normal programs (here: `/dev/edu0`).

## The restaurant picture

```
 You (user program)  -->  Waiter (driver)  -->  Kitchen (device)
   "10 factorial?"        writes order slip       cooks it
                          goes to sleep           rings a bell (interrupt)
                          wakes up, brings food   <-- result ready
```

- The **waiter does not stand and stare** at the kitchen. They sleep until the bell rings. That is what an interrupt is for.
- Only **one order at a time** goes to the kitchen (the kitchen has one stove). That is the mutex (lock).

## What our fake device (QEMU "edu") can do

| Feature | What it is |
|---|---|
| ID register | Says "I am an edu device, version 1.0" |
| Liveness register | Write a number, read back its bitwise opposite. Proves the registers work. |
| Factorial engine | Write n, it computes n!, then raises an interrupt |
| DMA engine | Copies bytes between RAM and its own 4 KB buffer, then raises an interrupt |

It is a teaching device. The "work" is silly (factorials), but the **driver pattern is the same as a real network card or GPU**: submit work, sleep, interrupt, get result.

---

# PART B: Architecture

## B1. Where it runs

```
Your Windows laptop
 +-- WSL2 (Ubuntu)           <- your workshop: gcc, make, qemu, scripts, git
      +-- QEMU               <- pretend computer that has an "edu" device plugged in
           +-- Guest Linux   <- a small Linux we built; our driver is loaded in HERE
                +-- edu_driver.ko   (the driver we wrote)
                +-- test programs   (test_edu, test_dma, test_stress)
```

**Why a virtual machine?** If the driver crashes, only the pretend computer dies. You reboot it in 2 seconds. Your real laptop is never at risk.

## B2. Layers (who talks to whom)

```
+--------------------------------------------------------------+
| USER SPACE                                                   |
|   test_edu / test_dma / test_stress                          |
|     open("/dev/edu0")                                        |
|     ioctl(fd, EDU_IOC_FACTORIAL, ...)   (sleeps until done)  |
|     ioctl(fd, EDU_IOC_DMA_LOOPBACK, ...)                     |
+----------------------------|---------------------------------+
                             |  system call boundary
+----------------------------v---------------------------------+
| KERNEL SPACE:  edu_driver.ko                                 |
|                                                              |
|   edu_ioctl()  -- the front door (checks the request)        |
|        |                                                     |
|   mutex  -- only ONE job talks to the device at a time       |
|        |                                                     |
|   write registers / start DMA  (MMIO: ioread32, iowrite32)   |
|        |                                                     |
|   wait_for_completion()  <--- the process sleeps here        |
|        ^                                                     |
|   edu_irq()  -- interrupt handler: read, ack, store, WAKE    |
|                                                              |
|   PCI core: finds the device, calls probe() / remove()       |
+----------------------------|-----------------^---------------+
                             | registers       | interrupt (INTx)
+----------------------------v-----------------|---------------+
| QEMU "edu" DEVICE                                            |
|   0x00 ID    0x04 liveness   0x08 factorial                  |
|   0x20 status  0x24 irq-status  0x64 irq-ack                 |
|   0x80 dma-src  0x88 dma-dst  0x90 dma-count  0x98 dma-cmd   |
|   internal 4 KB buffer at device address 0x40000             |
+--------------------------------------------------------------+
```

## B3. Life of the driver (load to unload)

```
boot
  |
Kernel scans the PCI bus, sees vendor 0x1234 / device 0x11e8  ("edu")
  |
insmod edu_driver.ko   -> kernel calls probe()
  |
probe():
   1. enable the device                  (pcim_enable_device)
   2. map its registers into memory      (pcim_iomap_regions, BAR0)
   3. read the ID register, run the liveness check   <- "is it really there?"
   4. set the DMA address limit (28 bits), allow bus mastering
   5. allocate two 4 KB DMA buffers
   6. hook up the interrupt              (INTx, shared)
   7. create /dev/edu0                   (misc device)
  |
...programs use /dev/edu0...
  |
rmmod edu_driver  -> remove(): delete /dev/edu0 first (nobody can start a new job)
                     the rest is cleaned up automatically (the "pcim_/devm_" helpers)
```

## B4. One factorial request

```
 test program              driver                          edu device
     |                        |                                 |
     | ioctl(FACTORIAL, 10)   |                                 |
     |----------------------->|                                 |
     |                        | lock the mutex                  |
     |                        | write "interrupt me when done"  |
     |                        | write 10 to factorial register  |
     |                        |-------------------------------->|
     |  (sleeping)            | wait_for_completion ...         | computes 10!
     |                        |                                 |
     |                        |<------- interrupt --------------|
     |                        | edu_irq(): read status,         |
     |                        |   read result, ack, complete()  |
     |                        | wakes up, unlock mutex          |
     | result = 3628800       |                                 |
     |<-----------------------|                                 |
```

Rule for the interrupt handler: **read, acknowledge, store, wake. Nothing else.** It runs in a special context where it must not sleep.

## B5. One DMA loopback request

```
user buffer --copy--> tx buffer (kernel RAM)
                          |
              (1) DMA: RAM -> device buffer      device copies bytes itself
                          |    wait for interrupt
              (2) DMA: device buffer -> rx buffer (kernel RAM, cleared first)
                          |    wait for interrupt
rx buffer --copy--> user buffer      then the test checks: out == in
```

The CPU never copies the data to or from the device. The **device** moves it. That is the point of DMA.

## B6. Files

```
edu-pci-driver/
  ARCHITECTURE.md        this file
  README.md              short how-to-run
  driver/
    edu_driver.c         the whole driver
    edu_uapi.h           the ioctl "contract" shared with user programs
    Makefile             builds edu_driver.ko against our kernel
  tests/
    test_edu.c           factorial 0..12 against a reference
    test_dma.c           DMA loopback, 7 sizes + 2 bad lengths
    test_stress.c        8 processes x 500 requests at the same time
  scripts/
    check_env.sh         are the tools installed?
    build_kernel.sh      download + build Linux 6.6.87 with KASAN and lockdep
    build_initramfs.sh   pack busybox + driver + tests into a tiny filesystem
    run_qemu.sh          boot the pretend computer
    test.sh              build everything, boot, run all tests, pass/fail
    ci_local.sh          test a fresh clone, like a CI machine would see it
    mutation_*.sh        "break it on purpose" checks (see Part C, M6/M7/M8)
  .github/workflows/ci.yml   GitHub runs test.sh on every push
```

---

# PART C: What we did, step by step, and why

Each step: **Goal**, **What we did**, **Why**, **What you learn**, **Gotcha** (a real problem we hit).

## Step 0: Choose the project and pick the tools

**Goal:** a project that proves driver skills with no hardware.

**What we did:** picked QEMU's `edu` device. It is a pretend PCI card made for learning.

**Why:** your resume had character drivers and register debugging, but no **PCI, interrupts, or DMA**. Those are the standard questions in kernel and embedded interviews (Qualcomm, NVIDIA, Cisco). A virtual device lets you practice them for free, and anyone can reproduce your result with one command.

**Honest note:** the device is simple and others have written drivers for it. The value is not the idea. The value is that you can **explain every line and every bug**.

## Step 1 (M0): Build the workshop

**Goal:** boot our own Linux in a pretend computer.

**What we did:**
1. Checked WSL2, KVM, and tools (`scripts/check_env.sh`).
2. Installed `gcc`, `make`, `qemu`, `busybox`, etc., and added you to the `kvm` group.
3. Downloaded Linux 6.6.87 and built it (`scripts/build_kernel.sh`).
4. Wrote `build_initramfs.sh` and `run_qemu.sh`.

**Why each piece exists:**
- **Build our own kernel:** a driver (`.ko`) only loads into the exact kernel it was built against. Our own kernel guarantees a match and lets us turn on debug tools later.
- **initramfs:** a tiny filesystem packed into one file. It holds busybox (small versions of `ls`, `insmod`, ...), our driver, and our tests. The VM needs no disk.
- **KVM:** lets QEMU use your real CPU for speed. We checked it is usable (it was, after joining the `kvm` group).

**What you learn:** the boot chain: QEMU starts the kernel, the kernel unpacks the initramfs and runs `/init`, our `/init` script loads the driver and runs the tests, then powers off.

**Gotcha: `~` and `$HOME` broke.** Commands launched from Windows into WSL had a mangled `HOME` (`C:UsersPPRANJALE`), so `~/work` pointed to the wrong place and the first kernel build never started. **Fix:** use absolute paths (`/home/<user>/work`) in scripts. **Lesson:** when a build "does nothing", check your paths first.

## Step 2 (M1 to M3): Load the module, find the device, touch a register

**Goal:** prove the driver can see and talk to the device.

**What we did:** wrote the first ~80 lines of `edu_driver.c`:
- An **ID table** that says "I handle vendor 0x1234, device 0x11e8".
- `probe()`: enable the device, map BAR0, read the ID register (expect `0x010000ed`), run the liveness test (write x, expect `~x`).
- `remove()`.

**Why:**
- `probe()` is the kernel saying "a device you can handle just appeared. Take it."
- **BAR0** is the device's window of registers. `pcim_iomap_regions()` maps it so `ioread32()` / `iowrite32()` reach the device (this is **MMIO**).
- The **liveness check** is cheap proof the registers really work before building more on top.
- We used the `pcim_*` and `devm_*` helpers. They **undo themselves automatically** on removal. Less cleanup code means fewer bugs.

**What you learn:** device discovery (probe/remove), MMIO, and why you check an ID register before trusting a device.

**Result:** `edu ID register = 0x010000ed (version 1.0)` and `liveness check passed`.

## Step 3 (M4): The interrupt

**Goal:** get the answer back by interrupt, not by polling.

**What we did:**
- Added `edu_irq()`: read the IRQ status, acknowledge it, store the result, call `complete()`.
- Added `edu_factorial()`: tell the device "interrupt me when done", write `n`, then `wait_for_completion_timeout()`.

**Why:**
- **Polling** ("are you done yet?" in a loop) wastes the CPU. An **interrupt** lets the CPU do other things.
- A **completion** is a simple "sleep until someone calls complete()" object. It is safer than inventing your own flag.
- The handler is tiny on purpose: interrupt context cannot sleep.
- The **timeout** means a missing interrupt gives an error instead of freezing forever.

**Gotcha: MSI never arrived.** We first used MSI (the modern interrupt kind). The device computed the right answer (3628800) and showed an interrupt pending, but our handler never ran. We tried:
1. Plain INTx (the old kind): **worked**.
2. MSI with KVM turned off: still failed.

So the **QEMU edu device simply never sends MSI** (checked on QEMU 8.2.2), even when the guest enables it. The driver was fine. We switched to INTx (a **shared** line, so the handler returns `IRQ_NONE` if the interrupt isn't ours) and wrote the finding in the code and README.

**Lessons:**
- Change one thing at a time, and test to separate "my bug" from "the device's limit".
- Do not promise MSI on a resume bullet if the device cannot do it. We corrected the docs.
- The debug print that showed `status`, `irq_stat`, and `fact` on timeout is what cracked it. **Print the device's state when something times out.**

## Step 4 (M5): A door for normal programs

**Goal:** a normal program uses the device with a few lines of C.

**What we did:**
- Created **`/dev/edu0`** with a **misc device** (the simplest way to get a device file).
- Added `ioctl` `EDU_IOC_FACTORIAL`.
- Put the request format in **`edu_uapi.h`**, shared by the driver and the tests.
- Wrote `tests/test_edu.c`: checks 0! to 12! against a reference.
- Added a **mutex** around each job.

**Why:**
- **`ioctl`** is the standard way to send "commands with data" to a driver.
- **`copy_from_user` / `copy_to_user`:** the kernel never trusts a user pointer directly. It copies safely and returns `-EFAULT` if the pointer is bad.
- **12! is the max** because the device register is 32 bits (13! overflows).
- The mutex is needed because there is **one** device engine and **one** completion. Two jobs at once would mix up results.
- The test is built **static** (`gcc -static`) because our tiny initramfs has no C library.

**Result:** `test_edu: PASS (0 failures)`. This is the "first release" point.

## Step 5 (M6): Many programs at once

**Goal:** prove the locking works when programs really run at the same time.

**What we did:**
- Gave the VM **4 CPUs** (`-smp 4`). With 1 CPU, processes take turns and never truly collide.
- Wrote `test_stress.c`: **8 processes x 500 requests**, each with different inputs, so a mixed-up answer is visible.
- Wrote `scripts/mutation_nolock.sh`: it **deletes the mutex in a COPY** of the driver and checks that the stress test now FAILS.

**Why the "break it on purpose" check?** A passing test only means something if it **can fail**. Result: without the mutex, all 8 workers got wrong answers (while the one-caller test still passed). With the mutex: 4000 of 4000 correct.

**What you learn:** race conditions, why one-at-a-time access to shared hardware matters, and **mutation testing** (the idea: break the code, confirm the test notices).

## Step 6 (M7): DMA

**Goal:** let the device move data to and from RAM by itself.

**What we did:**
- Added `EDU_IOC_DMA_LOOPBACK`: user buffer -> device (DMA) -> a second cleared buffer (DMA) -> user. The test compares them.
- Allocated two 4 KB **coherent** buffers with `dmam_alloc_coherent`.
- Set the **DMA mask to 28 bits**, and called `pci_set_master()`.
- `test_dma.c`: sizes 1, 4, 63, 64, 1000, 4095, 4096; checks nothing is written past the length; checks that lengths 0 and 4097 give `EINVAL`.
- `mutation_nodma.sh`: skip the return DMA in a copy, confirm the test fails on every size.

**Why each detail:**
- **Bus address vs kernel address.** The device does not understand kernel pointers. It uses **bus addresses**. `dmam_alloc_coherent` gives you both.
- **28-bit mask:** the edu device can only reach the first 256 MB of RAM. If you do not tell the kernel, it may hand you a buffer the device cannot reach.
- **Bus mastering:** a PCI device must be allowed to read and write RAM on its own.
- **Coherent buffers:** the CPU and device always see the same data (no cache surprises). Good for learning.
- **We clear the second buffer first** so a transfer that "does nothing" cannot accidentally pass.

**Note:** this passed on the first run. We still ran the break-it check, because "passed first try" is exactly when to be suspicious.

## Step 7 (M8): Bug detectors and CI

**Goal:** catch memory and locking bugs automatically, on every push.

**What we did:**
1. Rebuilt the kernel with **KASAN** (catches memory errors like overflows and use-after-free) and **lockdep** (`PROVE_LOCKING`, catches locking mistakes), plus atomic-sleep, mutex, and list debugging.
2. `/init` scans the kernel log after the tests for `BUG:`, `WARNING: CPU:`, `WARNING: possible...`, `KASAN`, `Call Trace`. It prints `KERNEL-CLEAN` or `KERNEL-ISSUES`, and `test.sh` fails on issues.
3. `mutation_kasan.sh`: plants a 1-byte heap overflow in a copy. All functional tests still pass, but **KASAN reports `slab-out-of-bounds in edu_ioctl`** and the run fails.
4. Wrote `.github/workflows/ci.yml`: install tools, build the kernel (cached), run `scripts/test.sh`.

**Why:** a normal test says "the answer was right". It cannot see a bug that corrupts memory quietly and still gives the right answer. KASAN and lockdep can. The planted-overflow check proved the safety net really catches something.

**Honest gap:** lockdep is on, but we never watched it catch anything (no lockdep mutation test).

## Step 8: Put it on GitHub, and fix CI

**What we did:** made a git repo, committed 18 files (build outputs are in `.gitignore`), pushed to `github.com/prathameshpranjale/edu-pci-driver`.

**CI failed twice. Both were instructive:**

| Failure | Real cause | Fix |
|---|---|---|
| `exit code 126` at "Build kernel" | The scripts were committed as `100644` (not executable). Files on the Windows drive do not keep the `chmod +x` bit, so git never recorded it. | `git update-index --chmod=+x scripts/*.sh`. Added `ci_local.sh`, which tests a **fresh clone**. |
| "Run tests" failed although all 3 tests passed | My health check matched the word `WARNING:`, and the runner's CPU prints a harmless boot notice: `RETBleed: WARNING: ... vulnerable`. | Narrowed the pattern to real warnings (`WARNING: CPU:`, `WARNING: possible`...). Re-verified KASAN is still caught. |

**Result:** CI green. About 10 minutes the first time (it builds the kernel), faster once the kernel cache is saved.

**Lessons:**
- "Works on my machine" is not "works in CI". Test a **fresh clone** (`scripts/ci_local.sh`).
- A health check that cries wolf gets ignored. Match **real** problems precisely.
- To read CI logs: Actions tab -> the run -> the failed step.

**Security lesson:** access tokens were pasted into chat. Treat any pasted token as exposed: **revoke it**. Prefer tokens limited to one repo with only the permissions needed (Contents and Workflows write).

---

# PART D: Glossary (plain words)

| Word | Plain meaning |
|---|---|
| **Driver** | Kernel code that makes one kind of device usable |
| **Kernel module (.ko)** | A driver you can load and unload while the kernel runs |
| **PCI** | The standard bus devices plug into. The kernel scans it at boot. |
| **probe()** | Kernel calls this: "a device you handle was found" |
| **BAR** | The device's window of registers in the address space |
| **MMIO** | Reading and writing device registers as if they were memory |
| **Register** | A numbered mailbox inside the device |
| **Interrupt (IRQ)** | The device taps the CPU on the shoulder: "I'm done" |
| **INTx** | The old interrupt kind: a shared wire |
| **MSI** | The modern interrupt kind: a message. (edu cannot send it.) |
| **ISR / handler** | The tiny function the kernel runs when the interrupt arrives |
| **Completion** | "Sleep until someone says done" |
| **Mutex** | A lock so only one thread works at a time |
| **DMA** | The device moves data to/from RAM by itself |
| **Bus address** | The address the DEVICE uses for RAM (different from the kernel's pointer) |
| **ioctl** | "Send a command with data to a driver" |
| **User space / kernel space** | Normal programs vs the kernel. A system call crosses the wall. |
| **initramfs** | A tiny filesystem in one file, unpacked into RAM at boot |
| **QEMU** | The program that pretends to be a computer |
| **KVM** | Uses the real CPU to make the pretend computer fast |
| **KASAN** | Debug tool: catches memory bugs (overflow, use-after-free) |
| **lockdep** | Debug tool: catches locking mistakes (deadlock risks) |
| **Mutation test** | Break the code on purpose to confirm a test notices |
| **CI** | A robot that builds and tests your code on every push |

---

# PART E: Cheat sheet

Run these inside WSL (Ubuntu). From Windows PowerShell prefix with `wsl -d Ubuntu -- bash`.

```
scripts/check_env.sh       # are the tools installed?
scripts/build_kernel.sh    # one time, ~15 minutes
scripts/test.sh            # build driver + tests, boot, run all tests, pass/fail
scripts/ci_local.sh        # test a fresh clone (what CI sees)

scripts/mutation_nolock.sh   # break the lock   -> stress test must FAIL
scripts/mutation_nodma.sh    # skip a DMA step  -> DMA test must FAIL
scripts/mutation_kasan.sh    # plant an overflow -> KASAN must report it

NOKVM=1 scripts/run_qemu.sh      # boot without KVM
scripts/run_qemu.sh -s -S        # wait for GDB on port 1234
```

The edit loop: change `driver/edu_driver.c`, run `scripts/test.sh`. Roughly a minute or less once the kernel is built.

**Reading the output:**
```
test_dma: PASS ...        each test reports itself
KERNEL-CLEAN              kernel log has no KASAN/lockdep/BUG/WARN
```

---

# PART F: Mistakes we made (and what each taught)

| Mistake | What happened | Lesson |
|---|---|---|
| Used `~` in WSL commands | `HOME` was mangled, build never started | Use absolute paths in scripts |
| Assumed edu does MSI | Handler never ran; result was correct but no interrupt | Test one change at a time; the device can be the limit, not your code |
| Promised "MSI" in docs/resume line | Wrong | Fix the claim, not the story |
| `chmod +x` on a Windows drive | Git recorded `100644`; CI exit 126 | Test a fresh clone before pushing |
| `grep WARNING:` too broad | CPU boot notice failed CI | Match real problems precisely |
| Pasted tokens in chat | Tokens exposed | Revoke; use least-privilege tokens |

---

# PART G: Next ideas (only if you want more; KISS says optional)

1. **Lockdep mutation:** write a deliberate locking mistake and confirm lockdep reports it. Closes the one honest gap.
2. **Interrupt latency measurement:** time submit-to-wake, put the number in the README.
3. **Polling vs interrupt comparison:** same job both ways, compare CPU use.
4. **Your own QEMU device with MSI:** write both sides (device model and driver). Much more work, much more distinctive.
5. **aarch64 + device tree variant:** a platform driver for a Qualcomm-style setup.

## How to explain this project in 30 seconds

> "I wrote a Linux PCI driver for QEMU's `edu` device. It handles device probe, MMIO registers, shared INTx interrupts with a short handler and a completion for sleep and wake, DMA with coherent buffers and a 28-bit mask, and a char device with a validated ioctl. I tested it with factorial, DMA, and an 8-process stress test, and I checked the tests by breaking the code on purpose. The guest kernel runs with KASAN and lockdep, and GitHub Actions boots QEMU and fails on any kernel warning."
