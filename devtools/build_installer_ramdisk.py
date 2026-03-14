#!/usr/bin/env python3
import os
import shutil
import subprocess
import sys
import tempfile

from create_partitioned_disk import create_partitioned_disk

KERNEL_RAW_LBA = 1024


def find_grub_bootimg() -> str:
    candidates = [
        "/usr/lib/grub/i386-pc/boot.img",
        "/usr/share/grub2/i386-pc/boot.img",
        "/usr/share/grub/i386-pc/boot.img",
    ]
    for c in candidates:
        if os.path.isfile(c):
            return c
    return ""


def find_grub_mkimage() -> str:
    candidates = [
        "grub2-mkimage",
        "grub-mkimage",
    ]
    for cmd in candidates:
        path = shutil.which(cmd)
        if path:
            return path
    return ""


def build_grub_core_image(repo_root: str, out_core: str, kernel_bin: str) -> bool:
    mkimage = find_grub_mkimage()
    if not mkimage:
        print("Warning: grub-mkimage not found; core.img will not be generated")
        return False

    if not os.path.isfile(kernel_bin):
        print(f"Warning: kernel not found for embedded core image: {kernel_bin}")
        return False

    kernel_size = os.path.getsize(kernel_bin)
    kernel_sectors = (kernel_size + 511) // 512

    with tempfile.TemporaryDirectory(prefix="installer_grub_core_") as td:
        cfg_path = os.path.join(td, "grub.cfg")
        with open(cfg_path, "w", encoding="utf-8") as f:
            f.write(
                "set timeout=0\n"
                f"multiboot (hd0){KERNEL_RAW_LBA}+{kernel_sectors}\n"
                "boot\n"
            )

        modules = ["biosdisk", "multiboot"]
        cmd = [
            mkimage,
            "-O", "i386-pc",
            "-o", out_core,
            "-c", cfg_path,
            "-p", "(hd0)",
            *modules,
        ]

        try:
            subprocess.check_call(cmd)
        except subprocess.CalledProcessError:
            print("Warning: failed to build GRUB core image; installer bootloader step may fail")
            if os.path.exists(out_core):
                os.remove(out_core)
            return False

    return os.path.isfile(out_core)


def copy_tree(src: str, dst: str, prune_dev: bool = True) -> None:
    if not os.path.isdir(src):
        return

    pruned_top_dirs = {"code"} if prune_dev else set()

    for root, dirs, files in os.walk(src):
        if prune_dev:
            rel = os.path.relpath(root, src)
            if rel == ".":
                dirs[:] = [d for d in dirs if d not in pruned_top_dirs]

        rel = os.path.relpath(root, src)
        out_root = dst if rel == "." else os.path.join(dst, rel)
        os.makedirs(out_root, exist_ok=True)
        for d in dirs:
            os.makedirs(os.path.join(out_root, d), exist_ok=True)
        for f in files:
            shutil.copy2(os.path.join(root, f), os.path.join(out_root, f))


def main() -> int:
    repo_root = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
    out_img = sys.argv[1] if len(sys.argv) > 1 else os.path.join(repo_root, "tmp_user", "boot", "installer_ramdisk.img")

    os.makedirs(os.path.dirname(out_img), exist_ok=True)

    testdir = os.path.join(repo_root, "testdir")
    kernel_bin = os.path.join(repo_root, "tmp_user", "boot", "kernel.bin")

    prune_dev = os.environ.get("EYN_INSTALLER_RAMDISK_PRUNE", "1") != "0"
    if prune_dev:
        print("Installer ramdisk: pruning dev-only payload (excluding testdir/code)")

    with tempfile.TemporaryDirectory(prefix="installer_ramdisk_src_") as stage:
        copy_tree(testdir, stage, prune_dev=prune_dev)

        # Ensure installer payload includes kernel for installed target.
        if os.path.isfile(kernel_bin):
            boot_dir = os.path.join(stage, "boot")
            os.makedirs(boot_dir, exist_ok=True)
            shutil.copy2(kernel_bin, os.path.join(boot_dir, "kernel.bin"))

        # Optional GRUB boot sector payload for MBR write step.
        bootimg = find_grub_bootimg()
        coreimg = ""
        if bootimg:
            grub_dir = os.path.join(stage, "installer", "grub")
            os.makedirs(grub_dir, exist_ok=True)
            shutil.copy2(bootimg, os.path.join(grub_dir, "boot.img"))
            coreimg = os.path.join(grub_dir, "core.img")
            build_grub_core_image(repo_root, coreimg, kernel_bin)

        create_partitioned_disk(out_img, total_size_mb=28, part1_size_mb=24, part2_size_mb=4)

        subprocess.check_call([
            sys.executable,
            os.path.join(repo_root, "devtools", "copy_testdir_to_eynfs.py"),
            stage,
            out_img,
        ])

    print(f"Installer ramdisk ready: {out_img}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
