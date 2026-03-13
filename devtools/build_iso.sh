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
TMP_DIR_NAME="${TMPDIR:-tmp_user}"
TMP_ROOT="${REPO_ROOT}/${TMP_DIR_NAME}"
KERNEL_BIN="${TMP_ROOT}/boot/kernel.bin"
INSTALLER_RAMDISK_BIN="${TMP_ROOT}/boot/installer_ramdisk.img"
ISO_OUT="${REPO_ROOT}/EYNOS.iso"

if [[ ! -f "${KERNEL_BIN}" ]]; then
  alt_kernel="${REPO_ROOT}/tmp/boot/kernel.bin"
  if [[ -f "${alt_kernel}" ]]; then
    KERNEL_BIN="${alt_kernel}"
    TMP_ROOT="${REPO_ROOT}/tmp"
  else
    echo "Error: kernel binary not found at ${KERNEL_BIN}" >&2
    echo "       Tried fallback: ${alt_kernel}" >&2
    exit 2
  fi
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

if [[ -f "${INSTALLER_RAMDISK_BIN}" ]]; then
  cp "${INSTALLER_RAMDISK_BIN}" "${stage_dir}/boot/"
else
  echo "Warning: installer ramdisk not found at ${INSTALLER_RAMDISK_BIN}; ISO will boot without RAM:/ payload" >&2
fi

cat >"${stage_dir}/boot/grub/grub.cfg" <<'EOF'
set default=0
set timeout=0
set gfxmode=text
set gfxpayload=text
set colour_normal=white/black
set colour_highlight=black/white

menuentry "EYN-OS" {
  multiboot /boot/kernel.bin installer=1
  module /boot/installer_ramdisk.img ramdisk
    boot
}
EOF

rm -f "${ISO_OUT}"

find_grub_platform_dir() {
  local platform="$1"
  local base
  for base in /usr/share/grub2 /usr/lib/grub /usr/share/grub; do
    if [[ -d "${base}/${platform}" ]]; then
      echo "${base}/${platform}"
      return 0
    fi
  done
  return 1
}


# If the system has a partial/broken x86_64-efi directory (e.g. contains only *.efi,
# missing moddep.lst/modules), grub2-mkrescue errors out even for BIOS/QEMU use.
# Work around by overriding the base directory to one that contains only i386-pc.
mkrescue_dir_arg=()
if grub_i386_dir="$(find_grub_platform_dir i386-pc 2>/dev/null)"; then
  # If x86_64-efi exists but is missing moddep.lst, grub2-mkrescue tends to error.
  # For qemu-system-i386 we only need i386-pc, so force that directory.
  if [[ -d "$(dirname "${grub_i386_dir}")/x86_64-efi" && ! -f "$(dirname "${grub_i386_dir}")/x86_64-efi/moddep.lst" ]]; then
    echo "Note: Detected incomplete GRUB x86_64-efi; forcing BIOS-only build using ${grub_i386_dir}." >&2
    mkrescue_dir_arg=(--directory="${grub_i386_dir}")
  fi
fi

run_mkrescue() {
  local compress_mode="$1"
  if [[ "${compress_mode}" == "none" ]]; then
    compress_mode="no"
  fi
  echo "Running grub-mkrescue (compress=${compress_mode})..."
  TMPDIR="${TMP_ROOT}" "${GRUB_MKRESCUE_BIN}" "${mkrescue_dir_arg[@]}" --modules="multiboot" --locales="" --themes="" --fonts="" --compress="${compress_mode}" -o "${ISO_OUT}" "${stage_dir}/"
}

# Some grub2-mkrescue builds (distro-dependent) can fail while staging EFI files with:
#   ... moddep.lst ... File exists
# Retrying with --compress=none avoids that code path.
compress_pref="${EYNOS_GRUB_COMPRESS:-xz}"
set +e
run_mkrescue "${compress_pref}"
mkrescue_rc=$?
set -e

if (( mkrescue_rc != 0 )); then
  echo "Warning: grub-mkrescue failed with compress=${compress_pref}. Retrying with --compress=no..." >&2
  rm -f "${ISO_OUT}" || true
  rm_rf_best_effort "${TMP_ROOT}/grub."*
  run_mkrescue "no"
fi

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
