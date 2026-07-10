# EYN-OS System-Service TODO

This document drafts a microkernel-leaning direction for EYN-OS where more OS policy and platform logic moves out of the kernel into trusted mid-layer system programs.

## Goals

- [ ] Keep the kernel focused on mechanisms rather than policy
- [ ] Move suitable OS components into trusted ring-3 system services
- [ ] Preserve a hard distinction between normal applications and trusted services
- [ ] Make trusted services replaceable and customizable without patching the kernel
- [ ] Improve crash isolation, restartability, and system recoverability
- [ ] Make malware persistence and privilege escalation materially harder

## Non-goals

- [ ] Do not treat file extension alone as a security boundary
- [ ] Do not treat root approval alone as sufficient trust
- [ ] Do not let trusted services run as normal shell-launchable executables
- [ ] Do not move low-level hardware-critical mechanisms out before IPC and supervision are mature
- [ ] Do not recreate a monolithic kernel via privileged shortcuts hidden behind user binaries

## Terminology

### Layer 0: kernel

The kernel remains the smallest trusted core and keeps ownership of:

- [ ] Scheduling mechanisms
- [ ] Virtual memory and address-space control
- [ ] Interrupt handling and trap/syscall entry
- [ ] IPC primitives and capability validation
- [ ] Lowest-level hardware control and security-critical resource arbitration

### Layer 1/2: trusted system services

These are ring-3 programs launched only by the OS service manager. They are trusted more than normal apps because they are verified, capability-scoped, supervised, and assigned system-service resource policy.

Candidate examples:

- [ ] GUI manager / compositor / session manager
- [ ] Network manager
- [ ] Package/install/update manager
- [ ] Login/session policy service
- [ ] Higher-level filesystem policy helpers
- [ ] Settings/configuration manager

### Layer 3: normal user applications

Normal UELF applications continue to use the standard userspace ABI and do not receive service-tier activation, trust, or resource policy.

## Artifact model

Trusted services should be distinct artifacts, but distinction must be enforced by policy, not just by naming.

- [ ] Define a dedicated trusted-service artifact class
- [ ] Allow a distinct extension for operator clarity if useful
- [ ] Store trusted services in a separate root-owned namespace from normal apps
- [ ] Ensure only the service manager can activate trusted services
- [ ] Refuse direct exec of trusted services from the shell or ordinary app context

## Service manifest

Every trusted service should carry a manifest embedded in the binary or bound to it during install.

Required manifest fields:

- [ ] Service name and version
- [ ] Service ABI version
- [ ] Required kernel features
- [ ] Declared capabilities
- [ ] Declared IPC endpoints provided and consumed
- [ ] Scheduler class and minimum scheduling floor
- [ ] Memory reservation / floor / ceiling
- [ ] Essential vs optional classification
- [ ] Restart policy
- [ ] Boot phase ordering
- [ ] Upgrade compatibility policy
- [ ] Health check / heartbeat expectations

## Trust and admission model

Trusted services must pass a stricter admission flow than layer-3 apps.

- [ ] Root approval is required to enable a trusted service
- [ ] Binary measurement must be checked before activation
- [ ] Signed service packages should be preferred
- [ ] If signing is unavailable, a root-owned hash registry should pin approved binaries
- [ ] Manifest and measured binary must match approved policy at boot and on reload
- [ ] Service activation must fail closed on manifest drift or measurement mismatch
- [ ] Capability grants must be denied by default and approved explicitly

## Execution model

Trusted services should still run in ring 3.

- [ ] Keep trusted services out of ring 0 by default
- [ ] Pass privileges only via capabilities and bootstrap handles
- [ ] Avoid ambient authority inherited from global kernel state
- [ ] Launch services only via a service-manager bootstrap protocol
- [ ] Provide lifecycle hooks for init, reload, health check, shutdown, and recovery

## Resource policy

Trusted services should be more reliable and responsive than layer-3 apps because the kernel gives them explicit policy, not because they are magically more correct.

### Scheduling

- [ ] Add a service scheduler class distinct from ordinary layer-3 tasks
- [ ] Give critical services a scheduling floor so they do not collapse under normal app load
- [ ] Define demotion rules that differ from ordinary MLFQ behavior where necessary
- [ ] Add service-aware latency targets for interactive/system-critical services
- [ ] Prevent service starvation under heavy user workload

### Memory

- [ ] Add service memory reservations or protected floors for essential services
- [ ] Distinguish reclaim policy for services vs ordinary apps
- [ ] Define OOM ordering so optional apps die before essential services
- [ ] Add memory-pressure notifications so services can degrade gracefully

### Reliability

- [ ] Add automatic restart for restartable trusted services
- [ ] Track crash loops and quarantine broken services
- [ ] Define fallback behavior for essential-service failure
- [ ] Support safe mode / rescue mode with minimal trusted service set

## Security model

Security must assume malware exists and focus on prevention, containment, visibility, and recovery.

### Service containment

- [ ] Make all privileged operations capability-scoped
- [ ] Split coarse capabilities into finer-grained authorities where needed
- [ ] Ensure compromise of one trusted service does not imply compromise of unrelated services
- [ ] Use explicit IPC rather than hidden in-kernel shortcuts
- [ ] Add per-service audit trails for privileged actions

### Installation and updates

- [ ] Support signed service packages and signed updates
- [ ] Add rollback protection for trusted-service updates
- [ ] Make service updates atomic and recoverable
- [ ] Prevent unprivileged writes into trusted service storage
- [ ] Refuse activation of modified trusted binaries until re-approved

### Runtime security

- [ ] Add service integrity checks at boot and service start
- [ ] Record service launch, restart, disable, and verification failures in audit logs
- [ ] Add policy for emergency disable of compromised trusted services
- [ ] Consider measured boot later for stronger trust chaining

## Developer experience

Developers should write trusted services as system daemons with a strict contract, not as kernel patches and not as ordinary apps with extra flags.

### Authoring model

- [ ] Keep trusted services in C with the existing UELF-style build flow as the base
- [ ] Add a service SDK for lifecycle, IPC, bootstrap context, and health reporting
- [ ] Add an embedded service metadata section similar to package metadata
- [ ] Add a dedicated build target for trusted services
- [ ] Add packaging support for install/enable/disable/status workflows

### Developer workflow

- [ ] Create a service package directory and source files
- [ ] Embed a service manifest section in the binary
- [ ] Build with a service-aware build wrapper
- [ ] Install into a trusted service namespace
- [ ] Enable via a root-only service registry command
- [ ] Launch only through the service manager

### Customization model

Developers should mostly customize EYN-OS by replacing or extending policy services rather than modifying the kernel.

- [ ] Support custom GUI managers as trusted services
- [ ] Support alternate network/session/package policy services
- [ ] Keep kernel modifications reserved for mechanism changes
- [ ] Define what counts as a safe service replacement vs a kernel feature addition

## Service manager

The service manager is the control point for the trusted-service tier.

Responsibilities:

- [ ] Discover installed trusted services
- [ ] Validate manifest, measurement, and policy
- [ ] Allocate capabilities and bootstrap handles
- [ ] Start services in boot-phase order
- [ ] Supervise runtime health and restart policy
- [ ] Quarantine crash-looping or invalid services
- [ ] Expose admin commands for enable/disable/status/logs
- [ ] Coordinate safe mode and fallback boot profiles

## IPC model

Trusted services need a better structured IPC layer than ordinary command-style syscall use.

- [ ] Define kernel IPC primitives suitable for long-lived services
- [ ] Add capability-addressed endpoints
- [ ] Support request/reply, notifications, and subscriptions
- [ ] Add timeout, cancellation, and failure semantics
- [ ] Define bootstrap channels for early service startup

## What can move first

Good early candidates are high-level policy-heavy components that benefit from restartability and user customization.

- [ ] GUI manager / compositor
- [ ] Session manager
- [ ] Network configuration/policy manager
- [ ] Package/update manager

## What should stay in kernel initially

These should stay in the kernel until service IPC, recovery, and trust policy are mature.

- [ ] Low-level ATA/NVMe/AHCI interrupt and DMA control
- [ ] Core virtual memory and page-fault handling
- [ ] Capability registry and validation core
- [ ] Scheduler core policy enforcement
- [ ] Trap/syscall entry and low-level context switching

## Migration phases

### Phase 1: groundwork

- [ ] Define service artifact class and manifest format
- [ ] Define trusted service storage/registry layout
- [ ] Add root-only service approval and enable/disable state
- [ ] Add binary hash pinning or signature verification support
- [ ] Add service-manager skeleton and boot integration

### Phase 2: kernel primitives

- [ ] Add service-class scheduler policy
- [ ] Add service memory reservation/floor policy
- [ ] Add structured IPC primitives for trusted services
- [ ] Add bootstrap capability grants and lifecycle hooks
- [ ] Add audit logging for trusted-service events

### Phase 3: first service migration

- [ ] Move the GUI manager to the trusted-service tier
- [ ] Keep a fallback console/session path in case GUI service fails
- [ ] Validate restartability and safe-mode boot
- [ ] Verify capability boundaries and resource policy under stress

### Phase 4: platform services

- [ ] Move network policy/config management to a trusted service
- [ ] Move package/update management to a trusted service
- [ ] Add service-to-service IPC contracts and dependency ordering

### Phase 5: hardening and ecosystem

- [ ] Narrow capability granularity for trusted services
- [ ] Add signed service packages and rollback-safe updates
- [ ] Add service SDK and documentation for third-party customization
- [ ] Add validation tools for manifests, signatures, and policy review

## Failure and recovery model

- [ ] Define essential-service failure policy
- [ ] Add fallback console if the GUI service fails
- [ ] Add service crash-loop detection and quarantine
- [ ] Add admin-visible recovery workflow for broken trusted services
- [ ] Ensure boot can continue in degraded mode when possible

## Open design questions

- [ ] Should trusted services use a distinct file extension, or only a distinct install namespace plus manifest?
- [ ] Which services truly need resource floors, and which only need restart supervision?
- [ ] How fine-grained should capability classes become before trusted-service rollout?
- [ ] Should service verification rely on signatures only, or allow local root-pinned hashes as a first step?
- [ ] Which IPC model best fits EYN-OS resource limits without recreating a huge userspace runtime?
- [ ] How much of session/input policy should move out with the GUI manager vs remain in kernel initially?

## Success criteria

- [ ] Normal apps cannot activate or impersonate trusted services
- [ ] Trusted services are replaceable without rebuilding the kernel
- [ ] A broken trusted service does not require a full reboot into an unusable system state
- [ ] Essential services remain responsive under ordinary app load
- [ ] Service compromise is contained, logged, and recoverable
- [ ] EYN-OS becomes easier to customize without making privilege escalation easy