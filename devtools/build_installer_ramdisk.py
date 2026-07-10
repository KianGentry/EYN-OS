#!/usr/bin/env python3
import json
import os
import shutil
import subprocess
import sys
import tempfile

from create_partitioned_disk import create_partitioned_disk

KERNEL_RAW_LBA = 1024

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


def copy_required_file(src: str, dst: str, label: str) -> None:
    if not os.path.isfile(src):
        raise RuntimeError(f"Missing required {label}: {src}")
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copy2(src, dst)


def copy_required_tree(src: str, dst: str, label: str) -> None:
    if not os.path.isdir(src):
        raise RuntimeError(f"Missing required {label}: {src}")
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copytree(src, dst, dirs_exist_ok=True)


def copy_optional_tree(src: str, dst: str, label: str) -> bool:
    if not os.path.isdir(src):
        print(f"Warning: optional {label} missing: {src}")
        return False
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copytree(src, dst, dirs_exist_ok=True)
    return True


def copy_optional_file(src: str, dst: str, label: str) -> bool:
    if not os.path.isfile(src):
        print(f"Warning: optional {label} missing: {src}")
        return False
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copy2(src, dst)
    return True


def read_manifest_packages(manifest_path: str) -> list[str]:
    packages: list[str] = []
    with open(manifest_path, "r", encoding="utf-8") as f:
        for raw_line in f:
            line = raw_line.strip()
            if not line or line.startswith("#"):
                continue
            token = line.split()[0]
            packages.append(token)
    return packages


def resolve_manifest_archive_sources(index_json_path: str, manifest_packages: list[str]) -> dict[str, str]:
    with open(index_json_path, "r", encoding="utf-8") as f:
        index_data = json.load(f)

    packages = index_data.get("packages")
    if not isinstance(packages, dict):
        raise RuntimeError(f"Invalid package index format: {index_json_path}")

    archive_sources: dict[str, str] = {}
    for package_name in manifest_packages:
        package_entry = packages.get(package_name)
        if not isinstance(package_entry, dict):
            raise RuntimeError(f"Package '{package_name}' missing in index.json")

        latest_version = package_entry.get("latest")
        versions = package_entry.get("versions")
        if not isinstance(latest_version, str) or not latest_version:
            raise RuntimeError(f"Package '{package_name}' missing latest version")
        if not isinstance(versions, dict) or latest_version not in versions:
            raise RuntimeError(f"Package '{package_name}' latest version not found in versions table")

        version_entry = versions[latest_version]
        if not isinstance(version_entry, dict):
            raise RuntimeError(f"Package '{package_name}' version entry is malformed")

        url = version_entry.get("url")
        if not isinstance(url, str) or "/" not in url:
            raise RuntimeError(f"Package '{package_name}' has invalid URL in index.json")

        archive_name = url.rsplit("/", 1)[1]
        if not archive_name:
            raise RuntimeError(f"Package '{package_name}' resolved to an empty archive name")

        archive_sources[package_name] = archive_name

    return archive_sources


def cache_archive_name_for_package(package_name: str) -> str:
    # EYNFS stores names in a 32-byte field including NUL; keep <= 31 chars.
    candidate = f"{package_name}.pkg"
    if len(candidate) > 31:
        raise RuntimeError(
            f"Package name '{package_name}' is too long for EYNFS cache archive naming"
        )
    return candidate


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

    print("Installer ramdisk: using package-based workflow")

    with tempfile.TemporaryDirectory(prefix="installer_ramdisk_src_") as stage:
        os.makedirs(os.path.join(stage, "binaries"), exist_ok=True)
        copy_required_file(
            os.path.join(testdir, "binaries", "installer"),
            os.path.join(stage, "binaries", "installer"),
            "installer binary",
        )
        copy_required_file(
            os.path.join(testdir, "binaries", "install"),
            os.path.join(stage, "binaries", "install"),
            "install binary",
        )
        copy_required_file(
            os.path.join(testdir, "binaries", "extract"),
            os.path.join(stage, "binaries", "extract"),
            "extract binary",
        )

        copy_required_file(
            os.path.join(testdir, "etc", "resolv.conf"),
            os.path.join(stage, "etc", "resolv.conf"),
            "resolver config",
        )
        copy_required_tree(
            os.path.join(testdir, "config"),
            os.path.join(stage, "config"),
            "config directory",
        )
        copy_required_tree(
            os.path.join(testdir, "icons"),
            os.path.join(stage, "icons"),
            "icons directory",
        )
        copy_required_tree(
            os.path.join(testdir, "icons16"),
            os.path.join(stage, "icons16"),
            "icons16 directory",
        )
        copy_required_tree(
            os.path.join(testdir, ".view"),
            os.path.join(stage, ".view"),
            "view backend directory",
        )

        copy_optional_tree(
            os.path.join(testdir, "fonts"),
            os.path.join(stage, "fonts"),
            "fonts directory",
        )
        copy_optional_file(
            os.path.join(testdir, "programs", "chibicc"),
            os.path.join(stage, "programs", "chibicc"),
            "chibicc program",
        )

        copy_required_file(
            kernel_bin,
            os.path.join(stage, "boot", "kernel.bin"),
            "kernel image",
        )

        packages_root = os.path.join(repo_root, "EYN-packages")
        copy_required_file(
            os.path.join(packages_root, "index.json"),
            os.path.join(stage, "installer", "index.json"),
            "local package index",
        )
        copy_required_file(
            os.path.join(packages_root, "www", "base.manifest"),
            os.path.join(stage, "installer", "base.manifest"),
            "base package manifest",
        )
        copy_required_file(
            os.path.join(packages_root, "www", "base.pkg"),
            os.path.join(stage, "installer", "base.pkg"),
            "base package archive",
        )

        manifest_packages = read_manifest_packages(os.path.join(packages_root, "www", "base.manifest"))
        archive_sources = resolve_manifest_archive_sources(
            os.path.join(packages_root, "index.json"),
            manifest_packages,
        )
        for package_name in sorted(archive_sources.keys()):
            archive_name = archive_sources[package_name]
            cache_name = cache_archive_name_for_package(package_name)
            copy_required_file(
                os.path.join(packages_root, "www", "releases", archive_name),
                os.path.join(stage, "installer", "pkg", cache_name),
                f"package archive {package_name}",
            )

        # Optional GRUB boot sector assets for MBR write step.
        bootimg = find_grub_bootimg()
        coreimg = ""
        if bootimg:
            grub_dir = os.path.join(stage, "installer", "grub")
            os.makedirs(grub_dir, exist_ok=True)
            shutil.copy2(bootimg, os.path.join(grub_dir, "boot.img"))
            coreimg = os.path.join(grub_dir, "core.img")
            build_grub_core_image(repo_root, coreimg, kernel_bin)

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
