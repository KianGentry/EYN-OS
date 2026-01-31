import os
import re
import sys
from pathlib import Path
from typing import List, Dict, Tuple

class CommandInfo:
    def __init__(self, name: str, handler: str, cmd_type: str, description: str, example: str, file: str):
        self.name = name
        self.handler = handler
        self.cmd_type = cmd_type
        self.description = description
        self.example = example
        self.file = file

def parse_register_macro(line: str) -> Tuple[str, str, str, str, str, str]:
    # Pattern to match the macro
    pattern = r'(?:REGISTER_SHELL_COMMAND|EYNOS_REGISTER_SHELL_COMMAND)\s*\(\s*(\w+)\s*,\s*"([^"]+)"\s*,\s*(\w+)\s*,\s*(\w+)\s*,\s*"([^"]+)"\s*,\s*"([^"]+)"\s*\)'
    match = re.search(pattern, line)
    
    if match:
        var_name = match.group(1)
        cmd_name = match.group(2)
        handler = match.group(3)
        cmd_type = match.group(4)
        description = match.group(5)
        example = match.group(6)
        return var_name, cmd_name, handler, cmd_type, description, example
    
    return None

def find_commands_in_file(file_path: str) -> List[CommandInfo]:
    commands = []
    
    try:
        with open(file_path, 'r', encoding='utf-8') as f:
            lines = f.readlines()
            
        for line_num, line in enumerate(lines, 1):
            result = parse_register_macro(line.strip())
            if result:
                var_name, cmd_name, handler, cmd_type, description, example = result
                commands.append(CommandInfo(
                    name=cmd_name,
                    handler=handler,
                    cmd_type=cmd_type,
                    description=description.replace('\\n', '\n'),
                    example=example,
                    file=os.path.basename(file_path)
                ))
    except Exception as e:
        print(f"Warning: Could not parse {file_path}: {e}")
    
    return commands

def scan_source_directory(src_dir: str) -> List[CommandInfo]:
    # Scan the source directory for all command registrations
    all_commands = []
    
    for root, dirs, files in os.walk(src_dir):
        for file in files:
            if file.endswith('.c'):
                file_path = os.path.join(root, file)
                commands = find_commands_in_file(file_path)
                all_commands.extend(commands)

    # Deduplicate by command name.
    # The portable registry TU intentionally re-registers a subset of commands
    # for AArch64-full; the canonical documentation should come from the
    # command's implementation module, not the portable registry.
    prefer_avoid_files = {"command_registry_portable.c"}
    by_name: Dict[str, CommandInfo] = {}
    for cmd in all_commands:
        existing = by_name.get(cmd.name)
        if not existing:
            by_name[cmd.name] = cmd
            continue

        existing_is_avoid = existing.file in prefer_avoid_files
        cmd_is_avoid = cmd.file in prefer_avoid_files

        # Prefer non-portable-registry definition when there is a conflict.
        if existing_is_avoid and not cmd_is_avoid:
            by_name[cmd.name] = cmd
            continue

        # Otherwise keep the existing one.
        # (If both are non-avoid or both are avoid, the first seen wins.)
    
    return list(by_name.values())

def categorize_commands(commands: List[CommandInfo]) -> Dict[str, List[CommandInfo]]:
    # Categorize commands by type
    categories = {
        'Essential Commands': [],
        'Streaming Commands': [],
        'Filesystem Commands': [],
        'System Commands': [],
        'Utility Commands': [],
        'Development Commands': []
    }
    
    # Define command categories
    filesystem_cmds = {'ls', 'cd', 'read', 'write', 'del', 'copy', 'move', 'makedir', 'deldir', 'fscheck', 'format', 'fdisk'}
    system_cmds = {'init', 'exit', 'clear', 'help', 'status', 'memory', 'portable', 'drive', 'lsata'}
    utility_cmds = {'echo', 'ver', 'calc', 'random', 'sort', 'search', 'history', 'log', 'hexdump', 'draw', 'spam'}
    dev_cmds = {'assemble', 'run'}
    
    for cmd in commands:
        if cmd.cmd_type == 'CMD_ESSENTIAL':
            categories['Essential Commands'].append(cmd)
        elif cmd.name in filesystem_cmds:
            categories['Filesystem Commands'].append(cmd)
        elif cmd.name in system_cmds:
            categories['System Commands'].append(cmd)
        elif cmd.name in dev_cmds:
            categories['Development Commands'].append(cmd)
        elif cmd.name in utility_cmds:
            categories['Utility Commands'].append(cmd)
        else:
            categories['Streaming Commands'].append(cmd)
    
    # Remove empty categories
    return {k: v for k, v in categories.items() if v}

def generate_markdown(commands: List[CommandInfo], output_file: str):
    # Generate Markdown documentation
    categorized = categorize_commands(commands)
    
    with open(output_file, 'w', encoding='utf-8') as f:
        f.write("# EYN-OS Command Reference\n\n")
        f.write("This document is auto-generated from the source code. ")
        f.write(f"Last updated: {__import__('datetime').datetime.now().strftime('%Y-%m-%d %H:%M:%S')}\n\n")
        
        f.write(f"**Total Commands:** {len(commands)}\n\n")
        
        # Table of Contents
        f.write("## Table of Contents\n\n")
        for category in categorized.keys():
            f.write(f"- [{category}](#{category.lower().replace(' ', '-')})\n")
        f.write("\n")
        
        # Generate sections for each category
        for category, cmd_list in categorized.items():
            if not cmd_list:
                continue
                
            f.write(f"## {category}\n\n")
            
            # Sort commands alphabetically
            cmd_list.sort(key=lambda x: x.name)
            
            for cmd in cmd_list:
                f.write(f"### {cmd.name}\n\n")
                f.write(f"**Handler:** `{cmd.handler}`\n\n")
                f.write(f"**Type:** {cmd.cmd_type}\n\n")
                f.write(f"**File:** `{cmd.file}`\n\n")
                f.write(f"**Description:**\n{cmd.description}\n\n")
                f.write(f"**Example:**\n```bash\n{cmd.example}\n```\n\n")
                f.write("---\n\n")
        
        # Add usage statistics
        f.write("## Command Statistics\n\n")
        f.write("| Category | Count |\n")
        f.write("|----------|-------|\n")
        for category, cmd_list in categorized.items():
            f.write(f"| {category} | {len(cmd_list)} |\n")
        f.write("\n")

def generate_help_text(commands: List[CommandInfo], output_file: str):
    # Generate a simple help text format for the OS
    categorized = categorize_commands(commands)
    
    with open(output_file, 'w', encoding='utf-8') as f:
        f.write("EYN-OS Command Help\n")
        f.write("==================\n\n")
        
        for category, cmd_list in categorized.items():
            if not cmd_list:
                continue
                
            f.write(f"{category}:\n")
            f.write("-" * len(category) + "\n")
            
            # Sort commands alphabetically
            cmd_list.sort(key=lambda x: x.name)
            
            for cmd in cmd_list:
                # Get the first sentence, but handle multiple periods better
                description = cmd.description
                # Find the first sentence (up to first period followed by space or newline)
                first_sentence = description
                for i, char in enumerate(description):
                    if char == '.' and (i + 1 >= len(description) or description[i + 1] in ' \n'):
                        first_sentence = description[:i + 1]
                        break
                
                f.write(f"  {cmd.name:<15} - {first_sentence}\n")
            f.write("\n")

def main():
    if len(sys.argv) < 2:
        print("Usage: python3 generate_command_docs.py <src_directory>")
        print("Example: python3 generate_command_docs.py src/")
        sys.exit(1)
    
    src_dir = sys.argv[1]
    
    if not os.path.exists(src_dir):
        print(f"Error: Source directory '{src_dir}' does not exist")
        sys.exit(1)
    
    print(f"Scanning {src_dir} for commands...")
    commands = scan_source_directory(src_dir)
    
    if not commands:
        print("No commands found!")
        sys.exit(1)
    
    print(f"Found {len(commands)} commands")
    
    # Generate documentation
    os.makedirs('docs', exist_ok=True)
    
    # Generate Markdown documentation
    markdown_file = 'docs/command-reference.md'
    generate_markdown(commands, markdown_file)
    print(f"Generated Markdown documentation: {markdown_file}")
    
    # Generate help text
    help_file = 'docs/help-text.txt'
    generate_help_text(commands, help_file)
    print(f"Generated help text: {help_file}")
    
    # Print summary
    categorized = categorize_commands(commands)
    print("\nCommand Summary:")
    for category, cmd_list in categorized.items():
        print(f"  {category}: {len(cmd_list)} commands")

if __name__ == "__main__":
    main() 