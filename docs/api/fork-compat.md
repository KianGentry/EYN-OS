# Fork/Vfork Compatibility Design

EYN-OS should not attempt to fake full POSIX `fork()` semantics in libc.
The ABI contract of `fork()` is a true returns-twice address-space split, and
that is not reproducible in a single-process userspace shim without kernel VM
support.

What we can support well
- `vfork()`/`fork()` call sites where the child immediately performs only fd
  remaps, environment tweaks, `exec*()`, or `_exit()`.
- `waitpid()` on the spawned child PID.
- `execvp()`/`execlp()` and environment propagation so package code can move to
  a spawn-backed path with minimal source-level adaptation.

What we should not promise
- Arbitrary child-side stack/local-variable mutation after `fork()`.
- Copy-on-write or independent heap state from libc alone.
- Full job-control/process-group semantics before kernel support exists.

Recommended compatibility model
1. Treat EYN-OS like a NOMMU target.
2. Prefer package paths that already support `vfork()+exec()` or
   `posix_spawn()`-style execution.
3. Provide libc helpers and optional compat headers that collapse common
   `fork`-then-`exec` patterns into `spawn_ex` + `waitpid`.
4. Keep raw `fork()`/`vfork()` unavailable until there is either kernel support
   or a package-specific shim that can rewrite the control flow safely.

Practical package strategy
- `toybox`: enable/use its NOMMU-friendly configuration paths. Toybox already
  has code paths designed around `vfork()` and immediate `exec()`.
- `nano`: patch its small number of `fork()+exec*()` call sites to use a thin
  helper built on `posix_spawn_file_actions`.
- `tinycc`: focus on `execvp()`/tool invocation first; generic `fork()` is not
  the main blocker.

Groundwork now present
- `execve()` supports `envp`.
- crt0 now publishes `envp` to libc so `getenv()` and PATH-based `execvp()` can
  work.
- `execvp()` / `execlp()` can search PATH and call `execve()`.

Next implementation slice
- Add a small helper API, e.g. `eyn_spawn_exec(...)`, that takes stdio remaps,
  argv, envp, and optional wait mode.
- Use that helper in targeted package patches or a package-specific compat
  header instead of trying to expose a fake general-purpose `fork()` symbol.