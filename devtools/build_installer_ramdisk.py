#!/usr/bin/env python3
import os
import struct
import shutil
import subprocess
import sys
import tempfile

from create_partitioned_disk import create_partitioned_disk

KERNEL_RAW_LBA = 1024
PAYLOAD_MAGIC = b"EYNPKG1\0"
PAYLOAD_TYPE_FILE = 1
PAYLOAD_TYPE_DIR = 2
PAYLOAD_FLAG_RLE = 1

MB = 1024 * 1024


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

    prune_extra = os.environ.get("EYN_INSTALLER_RAMDISK_PRUNE_EXTRA", "1") != "0"
    pruned_top_dirs = {"code"} if prune_dev else set()
    if prune_dev and prune_extra:
        pruned_top_dirs.update({"programs", "images"})

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


def copy_tree_into_prefix(src: str, dst_root: str, dst_prefix: str) -> None:
    if not os.path.isdir(src):
        return
    for root, dirs, files in os.walk(src):
        rel = os.path.relpath(root, src)
        if rel == ".":
            out_root = os.path.join(dst_root, dst_prefix)
        else:
            out_root = os.path.join(dst_root, dst_prefix, rel)
        os.makedirs(out_root, exist_ok=True)
        for d in dirs:
            os.makedirs(os.path.join(out_root, d), exist_ok=True)
        for f in files:
            shutil.copy2(os.path.join(root, f), os.path.join(out_root, f))


def copy_fonts_subset(src_fonts_dir: str, dst_root: str) -> None:
    os.makedirs(dst_root, exist_ok=True)
    profile = os.environ.get("EYN_INSTALLER_RAMDISK_FONT_PROFILE", "minimal")
    if profile == "none":
        return

    if profile == "full":
        copy_tree_into_prefix(src_fonts_dir, dst_root, "fonts")
        return

    # minimal profile: keep only one default runtime font.
    os.makedirs(os.path.join(dst_root, "fonts"), exist_ok=True)
    src_file = os.path.join(src_fonts_dir, "unscii-8.hex")
    if os.path.isfile(src_file):
        shutil.copy2(src_file, os.path.join(dst_root, "fonts", "unscii-8.hex"))


def rle_packbits_encode(data: bytes) -> bytes:
    out = bytearray()
    i = 0
    n = len(data)
    while i < n:
        run_len = 1
        while i + run_len < n and run_len < 128 and data[i + run_len] == data[i]:
            run_len += 1

        if run_len >= 3:
            out.append(127 + run_len)
            out.append(data[i])
            i += run_len
            continue

        lit_start = i
        lit_len = 0
        while i < n and lit_len < 128:
            run_probe = 1
            while i + run_probe < n and run_probe < 128 and data[i + run_probe] == data[i]:
                run_probe += 1
            if run_probe >= 3:
                break
            i += 1
            lit_len += 1

        out.append(lit_len - 1)
        out.extend(data[lit_start:lit_start + lit_len])

    return bytes(out)


def create_payload_archive(src_root: str, out_file: str) -> None:
    os.makedirs(os.path.dirname(out_file), exist_ok=True)

    entries = []
    for root, dirs, files in os.walk(src_root):
        dirs.sort()
        files.sort()
        rel_root = os.path.relpath(root, src_root)
        rel_root = "" if rel_root == "." else rel_root.replace("\\", "/")

        if rel_root:
            entries.append((PAYLOAD_TYPE_DIR, "/" + rel_root, None))

        for name in files:
            rel = (name if not rel_root else (rel_root + "/" + name)).replace("\\", "/")
            entries.append((PAYLOAD_TYPE_FILE, "/" + rel, os.path.join(root, name)))

    with open(out_file, "wb") as out:
        out.write(PAYLOAD_MAGIC)

        for etype, path, fpath in entries:
            p = path.encode("utf-8")
            if len(p) > 0xFFFF:
                raise RuntimeError(f"payload path too long: {path}")

            if etype == PAYLOAD_TYPE_DIR:
                out.write(struct.pack("<HBBII", len(p), etype, 0, 0, 0))
                out.write(p)
                continue

            with open(fpath, "rb") as f:
                raw = f.read()
            comp = rle_packbits_encode(raw)
            if len(comp) < len(raw):
                flags = PAYLOAD_FLAG_RLE
                payload = comp
            else:
                flags = 0
                payload = raw

            out.write(struct.pack("<HBBII", len(p), etype, flags, len(raw), len(payload)))
            out.write(p)
            out.write(payload)

        out.write(struct.pack("<HBBII", 0, 0, 0, 0, 0))


def compute_tree_stats(root: str) -> tuple[int, int, int]:
    total_bytes = 0
    file_count = 0
    dir_count = 0
    for cur_root, dirs, files in os.walk(root):
        dir_count += len(dirs)
        for name in files:
            file_count += 1
            total_bytes += os.path.getsize(os.path.join(cur_root, name))
    return total_bytes, file_count, dir_count


def pick_partition_sizes(stage_root: str) -> tuple[int, int, int, int]:
    total_bytes, file_count, dir_count = compute_tree_stats(stage_root)

    # Account for EYNFS data blocks + directory entries + bitmap/name table overhead.
    # Keep this conservative to avoid out-of-space during copy.
    metadata_overhead = (file_count * 1024) + (dir_count * 1024) + (768 * 1024)
    headroom_ratio = float(os.environ.get("EYN_INSTALLER_RAMDISK_HEADROOM", "0.10"))
    required_bytes = int((total_bytes + metadata_overhead) * (1.0 + headroom_ratio))

    default_min_part1_sectors = (3 * MB) // 512
    min_part1_sectors = int(os.environ.get("EYN_INSTALLER_RAMDISK_MIN_EYNFS_SECTORS", str(default_min_part1_sectors)))
    part1_sectors = max(min_part1_sectors, (required_bytes + 511) // 512)

    # Swap in installer module is not used for paging; keep minimal by default.
    part2_mb = max(0, int(os.environ.get("EYN_INSTALLER_RAMDISK_SWAP_MB", "0")))
    part2_sectors = (part2_mb * MB) // 512

    part1_start_sector = max(1, int(os.environ.get("EYN_INSTALLER_RAMDISK_PART1_START_SECTOR", "1")))
    total_sectors = part1_start_sector + part1_sectors + part2_sectors
    return total_sectors, part1_sectors, part2_sectors, part1_start_sector


def main() -> int:
    repo_root = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
    out_img = sys.argv[1] if len(sys.argv) > 1 else os.path.join(repo_root, "tmp_user", "boot", "installer_ramdisk.img")

    os.makedirs(os.path.dirname(out_img), exist_ok=True)

    testdir = os.path.join(repo_root, "testdir")
    kernel_bin = os.path.join(repo_root, "tmp_user", "boot", "kernel.bin")

    prune_dev = os.environ.get("EYN_INSTALLER_RAMDISK_PRUNE", "1") != "0"
    full_stage = os.environ.get("EYN_INSTALLER_RAMDISK_FULL", "0") == "1"
    if prune_dev:
        print("Installer ramdisk: pruning dev-only payload")

    with tempfile.TemporaryDirectory(prefix="installer_ramdisk_src_") as stage, tempfile.TemporaryDirectory(prefix="installer_payload_src_") as payload_src:
        copy_tree(testdir, payload_src, prune_dev=prune_dev)

        include_fonts = os.environ.get("EYN_INSTALLER_RAMDISK_INCLUDE_FONTS", "0") != "0"
        fonts_dir = os.path.join(repo_root, "fonts")
        if include_fonts and os.path.isdir(fonts_dir):
            copy_fonts_subset(fonts_dir, payload_src)

        userland_include = os.path.join(repo_root, "userland", "include")
        include_headers = os.environ.get("EYN_INSTALLER_RAMDISK_INCLUDE_HEADERS", "0") != "0"
        if include_headers and os.path.isdir(userland_include):
            copy_tree_into_prefix(userland_include, payload_src, "include")

        if full_stage:
            copy_tree(payload_src, stage, prune_dev=False)
        else:
            os.makedirs(os.path.join(stage, "binaries"), exist_ok=True)
            installer_bin = os.path.join(testdir, "binaries", "installer")
            if os.path.isfile(installer_bin):
                shutil.copy2(installer_bin, os.path.join(stage, "binaries", "installer"))

        # Ensure installer payload includes kernel for installed target.
        if os.path.isfile(kernel_bin):
            payload_boot_dir = os.path.join(payload_src, "boot")
            os.makedirs(payload_boot_dir, exist_ok=True)
            shutil.copy2(kernel_bin, os.path.join(payload_boot_dir, "kernel.bin"))

        # Optional GRUB boot sector payload for MBR write step.
        bootimg = find_grub_bootimg()
        coreimg = ""
        if bootimg:
            grub_dir = os.path.join(stage, "installer", "grub")
            os.makedirs(grub_dir, exist_ok=True)
            shutil.copy2(bootimg, os.path.join(grub_dir, "boot.img"))
            coreimg = os.path.join(grub_dir, "core.img")
            build_grub_core_image(repo_root, coreimg, kernel_bin)

        payload_archive = os.path.join(stage, "installer", "payload.eynpkg")
        create_payload_archive(payload_src, payload_archive)

        total_sectors, part1_sectors, part2_sectors, part1_start_sector = pick_partition_sizes(stage)
        print(
            "Installer ramdisk sizing: "
            f"total={total_sectors} sectors part1={part1_sectors} part2={part2_sectors} start={part1_start_sector}"
        )
        create_partitioned_disk(
            out_img,
            part1_start_sector=part1_start_sector,
            total_sectors_override=total_sectors,
            part1_sectors_override=part1_sectors,
            part2_sectors_override=part2_sectors,
        )

        copy_env = os.environ.copy()
        copy_env["EYNFS_COPY_FONTS"] = "0"
        copy_env["EYNFS_COPY_HEADERS"] = "0"

        subprocess.check_call([
            sys.executable,
            os.path.join(repo_root, "devtools", "copy_testdir_to_eynfs.py"),
            stage,
            out_img,
        ], env=copy_env)

    print(f"Installer ramdisk ready: {out_img}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
