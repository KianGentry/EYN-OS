import datetime
import os
import re
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Tuple


@dataclass
class CommandInfo:
    name: str
    description: str
    example: str
    binary_file: str
    source_file: str


CMDMETA_RE = re.compile(
    r'EYN_CMDMETA_V1\s*\(\s*"((?:[^"\\]|\\.)*)"\s*,\s*"((?:[^"\\]|\\.)*)"\s*\)',
    re.DOTALL,
)


def _unescape_c_string(value: str) -> str:
    return value.replace(r'\n', '\n').replace(r'\"', '"').replace(r'\\', '\\')


def _command_name_from_binary(filename: str) -> str:
    if filename.endswith(".uelf"):
        return filename[:-5]
    return filename


def scan_binary_commands(binaries_dir: Path) -> List[Tuple[str, str]]:
    commands: Dict[str, str] = {}
    if not binaries_dir.exists():
        return []

    for entry in sorted(binaries_dir.iterdir(), key=lambda p: p.name.lower()):
        if not entry.is_file():
            continue
        if entry.name.startswith('.'):
            continue

        cmd_name = _command_name_from_binary(entry.name)
        if not cmd_name:
            continue

        commands[cmd_name] = entry.name

    return sorted(commands.items(), key=lambda item: item[0].lower())


def scan_userland_metadata(code_dir: Path) -> Dict[str, Tuple[str, str, str]]:
    metadata: Dict[str, Tuple[str, str, str]] = {}
    if not code_dir.exists():
        return metadata

    for src in sorted(code_dir.glob("*_uelf.c"), key=lambda p: p.name.lower()):
        cmd_name = src.stem
        if cmd_name.endswith("_uelf"):
            cmd_name = cmd_name[:-5]

        text = src.read_text(encoding="utf-8", errors="ignore")
        match = CMDMETA_RE.search(text)
        if not match:
            continue

        desc = _unescape_c_string(match.group(1)).strip()
        example = _unescape_c_string(match.group(2)).strip()
        metadata[cmd_name] = (desc, example, src.name)

    return metadata


def build_command_list(binaries_dir: Path, code_dir: Path) -> List[CommandInfo]:
    binaries = scan_binary_commands(binaries_dir)
    metadata = scan_userland_metadata(code_dir)

    commands: List[CommandInfo] = []
    for cmd_name, binary_file in binaries:
        desc, example, source_file = metadata.get(
            cmd_name,
            (f"Run the {cmd_name} command.", cmd_name, "(metadata not found)"),
        )
        commands.append(
            CommandInfo(
                name=cmd_name,
                description=desc,
                example=example,
                binary_file=binary_file,
                source_file=source_file,
            )
        )

    return commands


def categorize_commands(commands: List[CommandInfo]) -> Dict[str, List[CommandInfo]]:
    categories: Dict[str, List[CommandInfo]] = {
        "Filesystem Commands": [],
        "System Commands": [],
        "Network Commands": [],
        "Memory Commands": [],
        "GUI/Window Commands": [],
        "Development Commands": [],
        "Utility Commands": [],
    }

    filesystem_cmds = {
        "ls", "cd", "read", "write", "del", "copy", "move", "makedir", "deldir", "size",
        "fscheck", "fatfix", "create", "delete", "pwd", "head", "tail",
    }
    system_cmds = {
        "help", "exit", "clear", "ver", "init", "portable", "drive", "lsata", "serialtest",
    }
    network_cmds = {"pciscan", "e1000probe", "e1000", "ping", "netstat", "netcfg"}
    memory_cmds = {"memory", "predict", "memory_stats", "mmap", "munmap", "msync", "pagingguards"}
    gui_cmds = {
        "stats", "kstats", "win_test", "kwin_test", "view", "vieww", "draw", "theme",
        "setbg", "clearbg", "setfont", "rect", "title",
    }
    dev_cmds = {"run", "assemble", "link", "ring3", "pf", "panic", "assertfail", "hexdump", "validate", "error", "log", "crashlog"}

    for cmd in commands:
        if cmd.name in filesystem_cmds:
            categories["Filesystem Commands"].append(cmd)
        elif cmd.name in system_cmds:
            categories["System Commands"].append(cmd)
        elif cmd.name in network_cmds:
            categories["Network Commands"].append(cmd)
        elif cmd.name in memory_cmds:
            categories["Memory Commands"].append(cmd)
        elif cmd.name in gui_cmds:
            categories["GUI/Window Commands"].append(cmd)
        elif cmd.name in dev_cmds:
            categories["Development Commands"].append(cmd)
        else:
            categories["Utility Commands"].append(cmd)

    return {k: v for k, v in categories.items() if v}


def generate_markdown(commands: List[CommandInfo], output_file: Path) -> None:
    categorized = categorize_commands(commands)

    with output_file.open("w", encoding="utf-8") as f:
        f.write("# EYN-OS Command Reference\n\n")
        f.write("This document is auto-generated from userland command metadata and binaries. ")
        f.write(f"Last updated: {datetime.datetime.now().strftime('%Y-%m-%d %H:%M:%S')}\n\n")
        f.write(f"**Total Commands:** {len(commands)}\n\n")

        f.write("## Table of Contents\n\n")
        for category in categorized.keys():
            f.write(f"- [{category}](#{category.lower().replace(' ', '-')})\n")
        f.write("\n")

        for category, cmd_list in categorized.items():
            cmd_list.sort(key=lambda x: x.name)
            f.write(f"## {category}\n\n")

            for cmd in cmd_list:
                f.write(f"### {cmd.name}\n\n")
                f.write(f"**Binary:** `testdir/binaries/{cmd.binary_file}`\n\n")
                f.write(f"**Metadata Source:** `testdir/code/{cmd.source_file}`\n\n")
                f.write(f"**Description:**\n{cmd.description}\n\n")
                f.write(f"**Example:**\n```bash\n{cmd.example}\n```\n\n")
                f.write("---\n\n")

        f.write("## Command Statistics\n\n")
        f.write("| Category | Count |\n")
        f.write("|----------|-------|\n")
        for category, cmd_list in categorized.items():
            f.write(f"| {category} | {len(cmd_list)} |\n")
        f.write("\n")


def generate_help_text(commands: List[CommandInfo], output_file: Path) -> None:
    categorized = categorize_commands(commands)

    with output_file.open("w", encoding="utf-8") as f:
        f.write("EYN-OS Command Help\n")
        f.write("==================\n\n")

        for category, cmd_list in categorized.items():
            cmd_list.sort(key=lambda x: x.name)
            f.write(f"{category}:\n")
            f.write("-" * len(category) + "\n")

            for cmd in cmd_list:
                first_line = cmd.description.splitlines()[0].strip() if cmd.description else ""
                if first_line and not first_line.endswith('.'):
                    first_line += "."
                f.write(f"  {cmd.name:<15} - {first_line}\n")
            f.write("\n")


def main() -> None:
    repo_root = Path(__file__).resolve().parents[1]
    if len(sys.argv) >= 2:
        candidate = Path(sys.argv[1])
        if candidate.is_dir():
            resolved = candidate.resolve()
            if (resolved / "testdir" / "binaries").exists():
                repo_root = resolved
            elif resolved.name == "src" and (resolved.parent / "testdir" / "binaries").exists():
                repo_root = resolved.parent

    binaries_dir = repo_root / "testdir" / "binaries"
    code_dir = repo_root / "testdir" / "code"

    if not binaries_dir.exists():
        print(f"Error: binaries directory not found: {binaries_dir}")
        sys.exit(1)

    print(f"Scanning binaries in {binaries_dir} ...")
    commands = build_command_list(binaries_dir, code_dir)

    if not commands:
        print("No commands found in testdir/binaries")
        sys.exit(1)

    docs_dir = repo_root / "docs"
    docs_dir.mkdir(parents=True, exist_ok=True)

    markdown_file = docs_dir / "command-reference.md"
    help_file = docs_dir / "help-text.txt"

    generate_markdown(commands, markdown_file)
    generate_help_text(commands, help_file)

    print(f"Found {len(commands)} commands")
    print(f"Generated Markdown documentation: {markdown_file}")
    print(f"Generated help text: {help_file}")


if __name__ == "__main__":
    main()