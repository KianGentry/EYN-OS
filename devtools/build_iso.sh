#!/usr/bin/env bash
set -euo pipefail

# Build a minimal multiboot ISO, optionally stripping EFI content.
# Keeps all temporary directories inside the repo's tmp/ directory and cleans them on exit.
#
# Usage:
#   devtools/build_iso.sh <path-to-grub-mkrescue>
#
# Environment:
#   CURDIR is not required; script uses its own location to locate repo root.

GRUB_MKRESCUE_BIN="${1:-}"
if [[ -z "${GRUB_MKRESCUE_BIN}" ]]; then
  echo "Error: missing grub-mkrescue path" >&2
  echo "Usage: $0 <path-to-grub-mkrescue>" >&2
  exit 2
fi

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TMP_ROOT="${REPO_ROOT}/tmp"
KERNEL_BIN="${REPO_ROOT}/tmp/boot/kernel.bin"
ISO_OUT="${REPO_ROOT}/EYNOS.iso"

if [[ ! -f "${KERNEL_BIN}" ]]; then
  echo "Error: kernel binary not found at ${KERNEL_BIN}" >&2
  exit 2
fi

mkdir -p "${TMP_ROOT}"

# These are set later if EFI cleanup runs.
iso_edit_dir=""
iso_clean_dir=""

rm_rf_best_effort() {
  # Remove paths as the current user; if they still exist and sudo is available, remove via sudo.
  # This helps clean up temp directories created by prior sudo runs.
  local paths=("$@")
  rm -rf "${paths[@]}" >/dev/null 2>&1 || true

  local any_left=0
  for p in "${paths[@]}"; do
    # Handle globs that didn't match.
    if [[ "$p" == *"*"* || "$p" == *"?"* || "$p" == *"["* ]]; then
      continue
    fi
    if [[ -e "$p" ]]; then
      any_left=1
      break
    fi
  done

  if (( any_left )) && sudo -n true >/dev/null 2>&1; then
    sudo rm -rf "${paths[@]}" >/dev/null 2>&1 || true
  fi
}

cleanup() {
  # Best-effort unmount and remove temp dirs.
  if [[ -n "${iso_edit_dir}" ]]; then
    sudo umount "${iso_edit_dir}" >/dev/null 2>&1 || true
  fi
  rm_rf_best_effort "${iso_edit_dir}" "${iso_clean_dir}" "${stage_dir}"
  rm_rf_best_effort "${TMP_ROOT}/grub."*
}

# Remove any stale mkrescue/xorriso temp dirs from interrupted builds.
rm_rf_best_effort "${TMP_ROOT}/grub."*

# If any remain, they're typically from a prior `sudo make ...` run and are root-owned.
if compgen -G "${TMP_ROOT}/grub.*" >/dev/null 2>&1; then
  echo "Warning: leftover ${TMP_ROOT}/grub.* temp dirs exist (likely root-owned from a prior sudo build)." >&2
  echo "         Remove with: sudo rm -rf '${TMP_ROOT}/grub.'*" >&2
fi

stage_dir="$(mktemp -d "${TMP_ROOT}/grub_ultra_minimal.XXXXXX")"
trap cleanup EXIT INT TERM

install -d -m 0755 "${stage_dir}/boot/grub"
cp "${KERNEL_BIN}" "${stage_dir}/boot/"

cat >"${stage_dir}/boot/grub/grub.cfg" <<'EOF'
set default=0
set timeout=0
set gfxmode=text
set gfxpayload=text
set color_normal=white/black
set color_highlight=black/white

menuentry "EYN-OS" {
    multiboot /boot/kernel.bin
    boot
}
EOF

rm -f "${ISO_OUT}"
TMPDIR="${TMP_ROOT}" "${GRUB_MKRESCUE_BIN}" --modules="multiboot" --locales="" --themes="" --fonts="" --compress=xz -o "${ISO_OUT}" "${stage_dir}/"

echo "Ultra-minimal ISO created: EYNOS.iso"
ls -lh "${ISO_OUT}"

echo "Attempting to strip EFI content (optional)..."
# Skip EFI cleanup if sudo isn't available non-interactively; the ISO from grub2-mkrescue works fine for QEMU.
if sudo -n true 2>/dev/null; then
  echo "Cleaning ISO EFI content with sudo..."
  iso_edit_dir="$(mktemp -d "${TMP_ROOT}/iso_edit.XXXXXX")"
  iso_clean_dir="$(mktemp -d "${TMP_ROOT}/iso_clean.XXXXXX")"

  sudo mount -o loop "${ISO_OUT}" "${iso_edit_dir}"
  cp -a "${iso_edit_dir}/." "${iso_clean_dir}/"

  rm -rf "${iso_clean_dir}/efi"* || true
  rm -rf "${iso_clean_dir}/boot/grub/i386-efi" || true
  rm -rf "${iso_clean_dir}/boot/grub/x86_64-efi" || true

  sudo umount "${iso_edit_dir}"
  rm -f "${ISO_OUT}"

  TMPDIR="${TMP_ROOT}" xorriso -as mkisofs -o "${ISO_OUT}" \
    -b boot/grub/i386-pc/eltorito.img -no-emul-boot -boot-load-size 4 -boot-info-table \
    --grub2-boot-info --grub2-mbr /usr/lib/grub/i386-pc/boot_hybrid.img \
    -r -V "EYN-OS" -iso-level 3 -joliet-long "${iso_clean_dir}"

  echo "EFI content stripped from ISO."
else
  echo "Skipping EFI cleanup (no sudo available). Using original grub2-mkrescue ISO."
fi

echo "ISO ready: EYNOS.iso"
ls -lh "${ISO_OUT}"
