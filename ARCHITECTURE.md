# edu-pci-driver: Architecture

A small Linux PCI driver for QEMU's emulated `edu` device. It shows the three things every real driver does: find the device, talk to its registers, and handle its interrupt.

**KISS rule:** build the smallest version that works end to end, then add one feature at a time. Do not add a feature until the previous one runs and is tested.

---

## 1. What it does (use case)

A user program asks the device to compute a factorial. The driver hands the job to the device, puts the program to sleep, and wakes it when the device raises an interrupt with the answer.

This is the same pattern as any accelerator or NIC driver: submit work, sleep, interrupt, return result.

---

## 2. Where it runs

```
Windows 11 laptop
 â””â”€â”€ WSL2 (Ubuntu)                 build machine: gcc, make, qemu, scripts
      â””â”€â”€ QEMU                     emulates a PC with the "edu" PCI device
           â””â”€â”€ Guest Linux kernel  your driver loads here (a crash only kills the VM)
                â”œâ”€â”€ edu_driver.ko  <- the code we write
                â””â”€â”€ test_edu       <- user-space test program
```

---

## 3. Layer diagram (who talks to whom)

```
+------------------------------------------------------------+
|                      USER SPACE                            |
|                                                            |
|   test_edu  (C program)                                    |
|     open("/dev/edu0")                                      |
|     ioctl(fd, EDU_FACTORIAL, &n)   ---> sleeps until done  |
|     close(fd)                                              |
+---------------------------|--------------------------------+
                            |  system call boundary
+---------------------------v--------------------------------+
|                      KERNEL SPACE                          |
|                                                            |
|   edu_driver.ko                                            |
|   +----------------------------------------------------+   |
|   | file_operations : open / ioctl / release           |   |
|   |        |                                           |   |
|   |        v                                           |   |
|   | job logic: write input -> wait_event -> read result|   |
|   |        |                          ^                |   |
|   |        v                          | wake_up        |   |
|   | MMIO access (ioread32/iowrite32)  |                |   |
|   |        |                  IRQ handler              |   |
|   +--------|------------------^------------------------+   |
|            |                  |                            |
|   PCI core (probe / remove, BAR mapping, INTx setup)       |
+------------|------------------|----------------------------+
             | register read/write       ^ interrupt (INTx)
+------------v------------------|----------------------------+
|                  QEMU "edu" DEVICE (emulated hardware)     |
|                                                            |
|   BAR0 registers:                                          |
|     0x00  ID           (read-only, identifies device)      |
|     0x04  liveness     (write x, read back ~x)             |
|     0x08  factorial    (write n, device computes n!)       |
|     0x20  status       (bit0 = busy, bit7 = irq on done)   |
|     0x24  irq status   (raised when work finished)         |
|     0x60  irq raise    (CPU can raise an irq manually)     |
|     0x64  irq ack      (CPU clears the irq)                |
|     0x80+ DMA engine   (later milestone)                   |
+------------------------------------------------------------+
```

---

## 4. Lifecycle flow (load to unload)

```
Guest boots
    |
    v
Kernel scans PCI bus -> finds vendor 0x1234, device 0x11e8 (edu)
    |
    v
insmod edu_driver.ko
    |
    v
probe():
    1. pci_enable_device
    2. pci_request_regions
    3. pci_iomap(BAR0)             -> get a pointer to the registers
    4. read ID register            -> sanity check (expect 0x010000ed)
    5. pci_alloc_irq_vectors(LEGACY)  (edu has no MSI)
    6. request_irq(handler)
    7. register /dev/edu0          -> user programs can now use it
    |
    v
Device ready ................ (runtime: see section 5)
    |
    v
rmmod edu_driver
    |
    v
remove():  unregister /dev/edu0, free irq, iounmap, release regions, disable device
```

---

## 5. Runtime flow: one factorial request

```
 test_edu                 driver                         edu device
    |                        |                               |
    | ioctl(FACTORIAL, 10)   |                               |
    |----------------------->|                               |
    |                        | take lock, mark job pending   |
    |                        | iowrite32(10, BAR0+0x08)      |
    |                        |------------------------------>|
    |                        |                               | computes 10!
    | (process sleeps in     |                               | ...
    |  wait_event)           |                               |
    |                        |          INTx interrupt       |
    |                        |<------------------------------|
    |                        | IRQ handler:                  |
    |                        |   read irq status             |
    |                        |   read result (BAR0+0x08)     |
    |                        |   ack irq (BAR0+0x64)         |
    |                        |   mark job done, wake_up      |
    |                        | wait_event returns            |
    | result = 3628800       |                               |
    |<-----------------------|                               |
```

Keep the IRQ handler short: read, acknowledge, store the result, wake the sleeper. No heavy work there.

---

## 6. Components

| Component | File | Responsibility |
|---|---|---|
| PCI glue | `driver/edu_pci.c` | ID table, `probe`, `remove`, BAR mapping, INTx setup |
| Register layer | `driver/edu_regs.h` | Register offsets and small inline read/write helpers |
| Char device | `driver/edu_cdev.c` | `/dev/edu0`, `open`, `ioctl`, `release` |
| IRQ handler | `driver/edu_pci.c` | Read result, ack, wake the waiting process |
| Test program | `tests/test_edu.c` | Calls ioctl, checks the result against a known factorial |
| Build scripts | `scripts/` | Build kernel, build initramfs, run QEMU |

Start with the driver in a single file (`edu_driver.c`). Split into the files above only if it grows past a few hundred lines.

---

## 7. Milestones (do one at a time)

| # | Milestone | Done when |
|---|---|---|
| M0 | Environment | WSL2 + QEMU boots a tiny kernel and prints "hello" |
| M1 | Module loads | `insmod` prints a message in `dmesg`; `rmmod` works |
| M2 | PCI probe | `probe()` runs for the `edu` device and reads the ID register |
| M3 | MMIO | Liveness register test passes (write x, read ~x) |
| M4 | Interrupt | Factorial result arrives via IRQ handler, logged in `dmesg` |
| M5 | Char device | `test_edu` gets a correct result through `ioctl` |
| M6 | Concurrency | Several processes submit jobs at once; the lock keeps results correct |
| M7 | DMA | Copy a buffer to and from the device |
| M8 | CI | GitHub Actions boots QEMU and runs the tests under KASAN and lockdep |

**Stop point for a first release:** M5 plus a clear README. M6 to M8 make it stand out.

---

## 8. Repo layout (start small)

```
edu-pci-driver/
  ARCHITECTURE.md     this file
  README.md           how to build, run, and results (write at M5)
  driver/
    edu_driver.c      all driver code at first
    Makefile
  tests/
    test_edu.c
  scripts/
    run_qemu.sh       boots QEMU with -device edu
    build_initramfs.sh
```

---

## 9. QEMU command (target shape)

```
qemu-system-x86_64 \
  -kernel bzImage \
  -initrd initramfs.cpio.gz \
  -append "console=ttyS0 panic=1" \
  -device edu \
  -nographic \
  -m 512M
```

Add `-enable-kvm` if KVM works in your WSL2. Add `-s -S` to attach GDB.

---

## 10. Risks and what to watch for

| Risk | Why | Mitigation |
|---|---|---|
| Module built against the wrong kernel | `insmod` fails with a version mismatch | Build the module against the exact guest kernel tree |
| Handler does too much | Interrupt context can't sleep | Only read, ack, store, wake_up |
| Lost wakeup | IRQ arrives before the process sleeps | Use `wait_event` with a condition flag set under the lock |
| Wrong register offsets | Device ignores writes | Check against the QEMU `edu` source (`hw/misc/edu.c`) |
| Over-building | Scope creep | Follow the milestones; no new feature until the last one is tested |

---

## 11. Finding: edu does not deliver MSI

Verified in M4 (QEMU 8.2.2): the guest enables MSI successfully, the device computes the result and marks its IRQ pending, but no MSI message is ever sent, with KVM on or off. Legacy INTx works. The driver therefore uses INTx (shared IRQ). MSI would need a custom QEMU device model, which is a possible later extension.

