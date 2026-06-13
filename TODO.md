# EYN-OS TODO

## Core compatibility (POSIX)
- [ ] Finish process-control syscall behavior for `fork`, `vfork`, `exec`, `select`, and `poll`
- [ ] Broaden POSIX-like process semantics so common userland tools behave correctly
- [ ] Complete shell pipeline support for complex pipelines, input redirection, background execution, and mixed redirection + pipes
- [ ] Expand the X11/Xlib compatibility layer beyond core drawing and events
- [ ] Implement full Unicode text rendering and input mapping in the UI pipeline
- [ ] Improve terminal, PTY, and session behavior to feel closer to a conventional Unix system

## Hardware support
- [ ] Add AHCI/SATA support
- [ ] Add NVMe support
- [ ] Add USB mass-storage support
- [ ] Add ACPI and power-management support
- [ ] Add SMP and APIC support
- [ ] Improve PCI/MMIO resource handling and BAR mapping
- [ ] Expand input-device support beyond the current core set

## Storage and reliability
- [ ] Add stronger crash recovery for storage and filesystem operations
- [ ] Expand filesystem integrity coverage across all supported media and workflows
- [ ] Add rollback-safe update and install flows
- [ ] Improve logging and post-crash diagnostics
- [ ] Add structured test coverage for boot, storage, and filesystem failure cases

## Security
- [ ] Add stronger memory-protection hardening where supported
- [ ] Add a clearer permission and credential model
- [ ] Harden capability checks and audit capability use
- [ ] Add sandboxing or per-process isolation beyond the current baseline
- [ ] Add signed packages or signed updates
- [ ] Add audit logging for sensitive operations
- [ ] Add safer default policies for network, file, and device access

## Networking
- [ ] Add DHCP client support
- [ ] Add IPv4 fragmentation and reassembly
- [ ] Add more complete TCP behavior and concurrency
- [ ] Add interrupt-driven packet reception
- [ ] Support multiple NICs cleanly
- [ ] Improve offloads, VLAN handling, and descriptor flexibility

## Optimisations
- [ ] Replace ATA PIO-heavy hot paths with DMA-first transfers and reduce busy-wait polling
- [ ] Add block cache and read-ahead/write-back strategy to reduce storage I/O latency
- [ ] Rework network RX/TX toward interrupt-first handling with budgeted drain instead of command-loop polling
- [ ] Replace per-byte pipe/PTY copy loops with bulk ring-buffer transfers
- [ ] Reduce allocator scan/coalesce overhead in hot paths (slab-first and less global merge churn)
- [ ] Add true append/offset write paths to avoid full read-concatenate-rewrite file appends
- [ ] Improve scheduler wait paths toward event/wait-queue driven wakeups with less syscall-side polling
- [ ] Replace linear capability-registry lookups with O(1)-ish indexed or hashed lookup

## Developer tooling
- [ ] Add more assembler features and syntax coverage
- [ ] Improve chibicc diagnostics and optimization quality
- [ ] Add debuginfo support for userland builds
- [ ] Add more complete libc/runtime coverage for native builds
- [ ] Add integration tests for the in-OS build toolchain

## More niche
- [ ] Add clipboard and selection history integration
- [ ] Add accessibility hooks and keyboard-navigation policies
- [ ] Add locale, timezone, and international input support
- [ ] Add suspend/resume or hibernation support
- [ ] Add remote administration or recovery workflows
- [ ] Add crash dumps with easier postmortem inspection
- [ ] Add signed-update and rollback protection

## Fun stuff
- [ ] Make capability-first GUI and file access the default security model
- [ ] Add deterministic execution or replay mode for debugging and teaching
- [ ] Improve self-hosting so the OS can build, inspect, and repair itself end-to-end
- [ ] Add a polished low-memory appliance mode with predictable behavior
- [ ] Tighten docs, commands, and metadata so the system stays self-describing