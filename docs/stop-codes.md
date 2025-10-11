# EYN-OS Stop Codes

When a critical error occurs, EYN-OS shows a stop code like `EYNOS_5E77ABA9` on the panic screen. This document helps interpret common codes and what you can do next.

## What is a stop code?
A stop code is a short hash derived from the panic message, source file, and line number. While it is not a full stack trace, it is stable for the same error location and message and can be used to search logs or documentation.

## Common categories
- ASSERT: An internal consistency check failed (ASSERT). Typically indicates a logic error.
- PAGING: Page fault or paging misconfiguration. Could be a null dereference or write to read‑only code/data if guards are enabled.
- FILESYSTEM: Errors reported by EYNFS/FAT32 drivers or fsck invariants.
- IRQ: Interrupt/IDT configuration or unexpected interrupt behavior.
- GENERAL: Anything else not recognized by a keyword scan.

## Interpreting the screen
- Reason: A brief description provided by the panic site.
- Location: Source file and line number where the panic was raised.
- Diagnostics (below the summary):
  - Stop code (EYNOS_XXXXXXXX)
  - Category (one of the above)
  - Source (file:line)
- A detailed backtrace is written to the serial log (COM1). Use the debug run with serial stdio to capture it.

## What to do next
1. If possible, capture the serial output (COM1) for the backtrace and any extra messages.
2. Search this document for your category and scenario below.
3. If you file an issue, include: stop code, category, location, and the serial backtrace.

## Known examples

### EYNOS_00000000 (placeholder)
- Meaning: Example placeholder. Replace with real entries as we collect them.
- Next steps: Provide reproduction details, recent actions, and attach serial logs.

---

Tip: The `panic` and `assertfail` shell commands can be used to test the panic screen and verify serial logging in your environment.
