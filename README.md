# edu-pci-driver

A Linux PCI driver for QEMU's emulated `edu` device, with a user-space test. It runs in a QEMU VM, so no hardware is needed.

A program calls `ioctl(fd, EDU_IOC_FACTORIAL, &req)` on `/dev/edu0`. The driver writes `n` to the device, sleeps, and wakes when the device's interrupt fires with the result.

```
test_edu --ioctl--> /dev/edu0 (misc device) --MMIO--> edu device
                         ^                                  |
                         +------ wake_up <-- IRQ handler <--+
```

Details and diagrams: [ARCHITECTURE.md](ARCHITECTURE.md).

## What it covers

- PCI probe/remove with managed (`pcim_*`) resources
- BAR0 MMIO register access
- Interrupt handling (shared legacy INTx) with a handler that only reads, acks, stores and wakes
- A `completion` for sleep/wake, a mutex so jobs do not overlap
- A misc character device with a validated `ioctl` (`copy_from_user` / `copy_to_user`)
- DMA in both directions with coherent buffers and a 28-bit DMA mask
- User-space tests: factorial, DMA loopback, and a multi-process stress test

## Run it

Needs Linux (tested on WSL2 Ubuntu 24.04) with `gcc`, `make`, `qemu-system-x86`, `busybox-static`, and a kernel tree built by `scripts/build_kernel.sh` (Linux 6.6.87).

```
scripts/check_env.sh      # verify prerequisites
scripts/build_kernel.sh   # one time, about 15 minutes
scripts/test.sh           # build driver + test, boot QEMU, run the test
```

Expected output:

```
edu_driver 0000:00:04.0: edu ID register = 0x010000ed (version 1.0)
edu_driver 0000:00:04.0: ready: /dev/edu0, irq 10
test_edu: PASS (0 failures)
edu_driver 0000:00:04.0: edu removed
```

Set `NOKVM=1` to boot without KVM. Pass `-s -S` to `scripts/run_qemu.sh` to attach GDB.

## Notes

- **INTx, not MSI.** With MSI enabled, the device finished its work and flagged the interrupt, but the handler never ran (QEMU 8.2.2, with and without KVM). QEMU's `edu` model does implement MSI (`msi_init` / `msi_notify` in `hw/misc/edu.c`), so this is an unsolved problem in our setup, not a device limit. The driver uses legacy INTx, which works.
- Factorial results are 32 bits, so `n` above 12 overflows.

## Concurrency testing

`tests/test_stress.c` forks 8 processes that each send 500 requests with different inputs, on a 4-CPU VM. A request answered with someone else's result fails the check.

The test is validated by a mutation check: `scripts/mutation_nolock.sh` deletes the driver's mutex in a copy of the project and confirms the stress test then fails (8 of 8 workers got wrong results; the single-caller test still passed). With the mutex, 4000 requests all pass.

## DMA

`EDU_IOC_DMA_LOOPBACK` sends a user buffer (1 to 4096 bytes) to the device's internal buffer by DMA, clears a second buffer, DMAs the data back into it, and returns it. `tests/test_dma.c` checks sizes 1 to 4096, that nothing is written past the requested length, and that bad lengths give `EINVAL`.

Points the code deals with:
- The device only addresses 28 bits of RAM, so the driver sets `dma_set_mask_and_coherent(..., DMA_BIT_MASK(28))`.
- The device needs bus mastering (`pci_set_master`) before it can touch RAM.
- The device sees bus addresses from `dmam_alloc_coherent`, not kernel virtual addresses.
- One shared interrupt handler wakes the waiter for both factorial and DMA completion.

`scripts/mutation_nodma.sh` skips the return transfer in a copy and confirms the test then fails on every size.

## Kernel bug checking and CI

The guest kernel is built with KASAN (memory errors) and lockdep (`PROVE_LOCKING`), plus atomic-sleep, mutex and list debugging. After the tests, the VM scans its kernel log for `BUG:`, `WARNING:`, `KASAN`, lockdep reports and call traces. `scripts/test.sh` fails if any appear, even when every functional test passed.

`scripts/mutation_kasan.sh` checks that gate: it plants a 1-byte heap overflow in a copy of the driver, and KASAN reports `slab-out-of-bounds in edu_ioctl` while all functional tests still pass. I did not write a matching lockdep mutation, so lockdep is enabled but not verified to fire.

`.github/workflows/ci.yml` installs the tools, enables KVM if the runner has it, builds the kernel (cached by the hash of `scripts/build_kernel.sh`), then runs `scripts/test.sh`. It has not been run on GitHub yet; the first run builds the kernel from scratch and will be slow.

## Status

M0 to M8 done.
