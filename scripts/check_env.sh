#!/usr/bin/env bash
# Report which build/run prerequisites are present in this Linux environment.
echo "kernel: $(uname -r)"
echo "user: $(id -un) groups: $(id -Gn)"
if [ -r /dev/kvm ] && [ -w /dev/kvm ]; then echo "kvm: usable"; else echo "kvm: present but NOT accessible (add user to kvm group)"; fi
for t in gcc make qemu-system-x86_64 git busybox flex bison bc cpio gdb; do
  if command -v "$t" >/dev/null; then echo "$t: ok"; else echo "$t: MISSING"; fi
done
for h in libssl-dev libelf-dev; do
  dpkg -s "$h" >/dev/null 2>&1 && echo "$h: ok" || echo "$h: MISSING"
done
