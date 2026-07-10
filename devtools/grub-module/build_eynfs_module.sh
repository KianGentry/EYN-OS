#!/usr/bin/env bash
set -euo pipefail

# Build helper for adding EYNFS filesystem support to a local GRUB source tree.
#
# Usage:
#   devtools/grub-module/build_eynfs_module.sh /path/to/grub-source
#
# Notes:
# - This script does not install GRUB system-wide. It only patches and builds
#   the provided source tree.
# - You still need to package/install resulting GRUB binaries for your target.

if [[ $# -ne 1 ]]; then
  echo "Usage: $0 <grub-source-tree>" >&2
  exit 2
fi

GRUB_SRC="$1"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
MODULE_SRC="${REPO_ROOT}/grub-module-src/eynfs.c"

if [[ ! -d "${GRUB_SRC}" ]]; then
  echo "error: GRUB source tree not found: ${GRUB_SRC}" >&2
  exit 2
fi

if [[ ! -f "${MODULE_SRC}" ]]; then
  echo "error: module source not found: ${MODULE_SRC}" >&2
  exit 2
fi

cd "${GRUB_SRC}"

if [[ ! -d grub-core/fs ]]; then
  echo "error: invalid GRUB tree (missing grub-core/fs)" >&2
  exit 2
fi

cp -f "${MODULE_SRC}" grub-core/fs/eynfs.c

echo "Inserted grub-core/fs/eynfs.c"

if ! grep -Eq '^\s*name\s*=\s*eynfs;\s*$' grub-core/Makefile.core.def; then
  cat >> grub-core/Makefile.core.def <<'EOF'

module = {
  name = eynfs;
  common = fs/eynfs.c;
};
EOF
  echo "Registered eynfs module in grub-core/Makefile.core.def"
else
  echo "eynfs module already registered in Makefile.core.def"
fi

if [[ -x ./autogen.sh ]]; then
  ./autogen.sh
fi

./configure --with-platform=pc
make -j"$(getconf _NPROCESSORS_ONLN)"

echo "Done. GRUB build now includes the eynfs module."
