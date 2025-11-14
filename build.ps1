<#
    build.ps1 - Windows 11 PowerShell build helper for EYN-OS

    This script provides functional parity (as practical on Windows) with the GNU Makefile:
      Targets supported:
        build        - build kernel objects, link kernel, create ISO, generate eynfs.img, docs
        run          - build + launch QEMU (CD: ISO, HDD: eynfs.img)
        qemu-debug   - build + launch QEMU with serial/stdio and debug flags
        qemu-gdb     - build + launch QEMU waiting for GDB (:1234)
        clean        - remove build artifacts
        eynfsimg     - create filesystem image and populate from testdir
        testimg      - create secondary blank image (for test)
        test         - run with testimg as second drive
        fat32img     - create FAT32 image (requires mkfs.fat from e.g. MSYS2 'dosfstools')
        fat32img-populate - populate FAT32 image with testdir contents (requires mtools)
        runfat32     - run with FAT32 as secondary drive
        analyze      - static analyzer pass (GCC -fanalyzer) over sources
        fsck_eynfs   - run integrity check script (requires Python)

    Usage examples:
        pwsh -File build.ps1 build
        pwsh -File build.ps1 run
        ./build.ps1 analyze

    Cross compilation toolchain assumptions (recommended via MSYS2 or Scoop):
        i686-elf-gcc, i686-elf-ld, nasm, grub-mkrescue (or grub2-mkrescue), qemu-system-i386, python3

    You may install via MSYS2 (pacman -S ...):
        pacman -S mingw-w64-i686-binutils mingw-w64-i686-gcc nasm qemu grub mtools dosfstools python

    Or via Scoop/Chocolatey (community packages) for: qemu, nasm, python, gcc (then build cross-compiler separately).

    NOTE: Windows paths & mounting loopback ISO modifications (EFI strip) are skipped.
    The produced ISO from grub-mkrescue is sufficient for QEMU/testing.
#>

[CmdletBinding()] param(
    [Parameter(Position=0)] [string] $Target = 'build',
    [switch] $VerboseMode
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Write-Info($msg) { Write-Host "[INFO] $msg" -ForegroundColor Cyan }
function Write-Err($msg)  { Write-Host "[ERROR] $msg" -ForegroundColor Red }
function Write-Warn($msg) { Write-Host "[WARN] $msg" -ForegroundColor Yellow }

function Assert-Tool($name, [string]$hint) {
    if (-not (Get-Command $name -ErrorAction SilentlyContinue)) {
        Write-Err "Required tool '$name' not found. $hint"
        throw "Missing tool: $name"
    }
}

function Resolve-GrubMkrescue {
    $candidate = (Get-Command grub2-mkrescue -ErrorAction SilentlyContinue) ?? (Get-Command grub-mkrescue -ErrorAction SilentlyContinue)
    if (-not $candidate) { Write-Err "Neither grub2-mkrescue nor grub-mkrescue found."; throw }
    return $candidate.Name
}

# Compiler / flags (mirroring Makefile)
$COMPILER = 'i686-elf-gcc'
$LINKER   = 'i686-elf-ld'
$ASSEMBLER = 'nasm'
$EMULATOR = 'qemu-system-i386'

$KERNEL_CFLAGS = @(
    '-m32','-c','-ffreestanding','-fno-builtin','-fno-omit-frame-pointer','-fno-common',
    '-Os','-fno-strict-overflow','-fwrapv','-fdata-sections','-ffunction-sections',
    '-I','include/','-I','include/cpu','-I','include/drivers','-I','include/misc','-I','include/graphics','-I','include/network','-I','include/utilities','-I','include/utilities/shell',
    '-Wall','-Wextra','-Werror=implicit-function-declaration','-Wformat=2','-Wformat-security',
    '-Wno-unused-parameter','-Wno-unused-variable','-Wnull-dereference','-Wmissing-prototypes','-Wstrict-prototypes','-Wold-style-definition',
    '-Wpointer-arith','-Wshadow','-Wundef','-Wredundant-decls','-Wswitch-enum','-Wswitch-default'
)
$GUI_CFLAGS = $KERNEL_CFLAGS | ForEach-Object { if ($_ -eq '-Os') { '-O2' } else { $_ } }
$ASFLAGS = @('-f','elf32')
$LDFLAGS = @('-m','elf_i386','-T','src/boot/link.ld','--gc-sections','-Map','tmp/boot/kernel.map','-s')
$HOST_CFLAGS = @('-O2','-g','-Wall','-Wextra','-Wformat=2','-Wformat-security','-fstack-protector-strong','-D_FORTIFY_SOURCE=2',
    '-I','include','-I','include/misc','-I','include/drivers','-I','include/cpu','-I','include/utilities','-I','include/graphics','-I','include/network')
$HOST_LDFLAGS = @()

$OBJS = @(
 'obj/kasm.o','obj/kc.o','obj/idt.o','obj/isr.o','obj/syscall.o','obj/kb.o','obj/string.o','obj/system.o','obj/util.o','obj/shell.o','obj/math.o','obj/vga.o','obj/serial.o','obj/fat32.o','obj/ata.o','obj/eynfs.o','obj/rei.o','obj/shell_commands.o','obj/fs_commands.o','obj/fdisk_commands.o','obj/format_command.o','obj/write_editor.o','obj/tui.o','obj/help_tui.o','obj/assemble.o','obj/instruction_set.o','obj/run_command.o','obj/shell_script.o','obj/history.o','obj/subcommands.o','obj/predictive_memory.o','obj/predictive_commands.o','obj/zero_copy.o','obj/zero_copy_commands.o','obj/paging.o','obj/pipeline.o','obj/kernel_api.o','obj/native_exec.o','obj/native_run.o','obj/sched.o','obj/irq.o','obj/irq_stubs.o','obj/mouse.o','obj/draw_gui.o','obj/image_viewer_gui.o','obj/window_test.o','obj/vfs.o','obj/stats_gui.o','obj/panic.o','obj/watchdog.o','obj/linux_syscalls.o',
 'obj/tiling_manager.o','obj/tiling_cmd.o','obj/terminals.o'
)

function Ensure-Dirs { New-Item -ItemType Directory -Force -Path 'obj','tmp','tmp/boot' | Out-Null }

function Compile-Asm($src,$out) {
    Ensure-Dirs
    & $ASSEMBLER $ASFLAGS -o $out $src
}

function Compile-C($src,$out,[switch]$Gui) {
    Ensure-Dirs
    $flags = if ($Gui) { $GUI_CFLAGS } else { $KERNEL_CFLAGS }
    & $COMPILER $flags $src -o $out
}

function Build-KernelObjects {
    Write-Info 'Compiling kernel objects...'
    # Map source -> object rules (condensed; manual list)
    Compile-Asm 'src/boot/kernel.asm' 'obj/kasm.o'
    Compile-Asm 'src/cpu/syscall.asm' 'obj/syscall.o'
    Compile-Asm 'src/cpu/irq.asm' 'obj/irq_stubs.o'

    Compile-C 'src/entry/kernel.c' 'obj/kc.o'
    Compile-C 'src/cpu/idt.c' 'obj/idt.o'
    Compile-C 'src/drivers/kb.c' 'obj/kb.o'
    Compile-C 'src/cpu/isr.c' 'obj/isr.o'
    Compile-C 'src/utilities/shell/string.c' 'obj/string.o'
    Compile-C 'src/cpu/system.c' 'obj/system.o'
    Compile-C 'src/utilities/util.c' 'obj/util.o'
    Compile-C 'src/utilities/shell/shell.c' 'obj/shell.o'
    Compile-C 'src/utilities/basic/math.c' 'obj/math.o'
    Compile-C 'src/drivers/vga.c' 'obj/vga.o'
    Compile-C 'src/drivers/mouse.c' 'obj/mouse.o'
    Compile-C 'src/drivers/serial.c' 'obj/serial.o'
    Compile-C 'src/misc/panic.c' 'obj/panic.o'
    Compile-C 'src/misc/watchdog.c' 'obj/watchdog.o'
    Compile-C 'src/drivers/fat32.c' 'obj/fat32.o'
    Compile-C 'src/drivers/ata.c' 'obj/ata.o'
    Compile-C 'src/drivers/eynfs.c' 'obj/eynfs.o'
    Compile-C 'src/drivers/rei.c' 'obj/rei.o'
    Compile-C 'src/utilities/shell/shell_commands.c' 'obj/shell_commands.o'
    Compile-C 'src/utilities/shell/fs_commands.c' 'obj/fs_commands.o'
    Compile-C 'src/utilities/shell/fdisk_commands.c' 'obj/fdisk_commands.o'
    Compile-C 'src/utilities/shell/format_command.c' 'obj/format_command.o'
    Compile-C 'src/utilities/shell/write_editor.c' 'obj/write_editor.o'
    Compile-C 'src/utilities/shell/run_command.c' 'obj/run_command.o'
    Compile-C 'src/utilities/shell/shell_script.c' 'obj/shell_script.o'
    Compile-C 'src/utilities/shell/history.c' 'obj/history.o'
    Compile-C 'src/utilities/tui/tui.c' 'obj/tui.o' -Gui
    Compile-C 'src/utilities/shell/help_tui.c' 'obj/help_tui.o'
    Compile-C 'src/utilities/assembler/assemble.c' 'obj/assemble.o'
    Compile-C 'src/utilities/assembler/instruction_set.c' 'obj/instruction_set.o'
    Compile-C 'src/utilities/shell/subcommands.c' 'obj/subcommands.o'
    Compile-C 'src/utilities/predictive_memory.c' 'obj/predictive_memory.o'
    Compile-C 'src/utilities/shell/predictive_commands.c' 'obj/predictive_commands.o'
    Compile-C 'src/utilities/zero_copy.c' 'obj/zero_copy.o'
    Compile-C 'src/utilities/shell/zero_copy_commands.c' 'obj/zero_copy_commands.o'
    Compile-C 'src/cpu/paging.c' 'obj/paging.o'
    Compile-C 'src/utilities/shell/pipeline.c' 'obj/pipeline.o'
    Compile-C 'src/cpu/kernel_api.c' 'obj/kernel_api.o'
    Compile-C 'src/cpu/native_exec.c' 'obj/native_exec.o'
    Compile-C 'src/utilities/shell/native_run.c' 'obj/native_run.o'
    Compile-C 'src/cpu/sched.c' 'obj/sched.o'
    Compile-C 'src/cpu/irq.c' 'obj/irq.o'
    Compile-C 'src/utilities/tui/tiling_manager.c' 'obj/tiling_manager.o' -Gui
    Compile-C 'src/utilities/tui/terminals.c' 'obj/terminals.o' -Gui
    Compile-C 'src/utilities/shell/tiling_cmd.c' 'obj/tiling_cmd.o'
    Compile-C 'src/utilities/shell/draw_gui.c' 'obj/draw_gui.o' -Gui
    Compile-C 'src/utilities/shell/image_viewer_gui.c' 'obj/image_viewer_gui.o' -Gui
    Compile-C 'src/utilities/shell/window_test.c' 'obj/window_test.o'
    Compile-C 'src/utilities/shell/stats_gui.c' 'obj/stats_gui.o'
    Compile-C 'src/fs/vfs.c' 'obj/vfs.o'
    Compile-C 'src/cpu/linux_syscalls.c' 'obj/linux_syscalls.o'
}

function Link-Kernel {
    Ensure-Dirs
    Write-Info 'Linking kernel...'
    & $LINKER $LDFLAGS -o 'tmp/boot/kernel.bin' $OBJS
}

function Generate-Docs {
    if (Get-Command python3 -ErrorAction SilentlyContinue) {
        Write-Info 'Generating command docs (python3)...'
        & python3 devtools/generate_command_docs.py src/ | Out-Null
    } else { Write-Warn 'python3 not found; skipping docs.' }
}

function Create-ISO {
    Write-Info 'Creating ISO...'
    $grub = Resolve-GrubMkrescue
    $stageDir = Join-Path 'tmp' ('grub_ultra_minimal.' + ([System.IO.Path]::GetRandomFileName()).Split('.')[0])
    New-Item -ItemType Directory -Force -Path "$stageDir/boot/grub" | Out-Null
    Copy-Item 'tmp/boot/kernel.bin' "$stageDir/boot/" -Force
    $cfg = @(
        'set default=0','set timeout=0','set gfxmode=text','set gfxpayload=text',
        'set color_normal=white/black','set color_highlight=black/white','',
        'menuentry "EYN-OS" {','    multiboot /boot/kernel.bin','    boot','}'
    )
    $cfg | Set-Content -Encoding ASCII "$stageDir/boot/grub/grub.cfg"
    & $grub --modules="multiboot" --locales="" --themes="" --fonts="" --compress=xz -o EYNOS.iso $stageDir/
    Write-Info "ISO created: $(Get-Item EYNOS.iso).Length bytes"
}

function Build-EynfsFormat { & $COMPILER $HOST_CFLAGS eynfs_format.c -o eynfs_format $HOST_LDFLAGS }

function Create-EynfsImage {
    Write-Info 'Creating eynfs filesystem image (10MB)...'
    if (Test-Path eynfs.img) { Remove-Item eynfs.img -Force }
    # Use fsutil to allocate size if available; fallback to zero write
    $sizeBytes = 10MB
    $fs = [IO.File]::OpenWrite('eynfs.img'); $fs.SetLength($sizeBytes); $fs.Close()
    Build-EynfsFormat
    # Sector count: 20480
    & ./eynfs_format eynfs.img 20480
    if (Get-Command python3 -ErrorAction SilentlyContinue) {
        & python3 devtools/copy_testdir_to_eynfs.py testdir/ | Out-Null
    } else { Write-Warn 'python3 not found; skipping testdir population.' }
}

function Create-TestImage {
    Write-Info 'Creating testimg.img (10MB)...'
    if (Test-Path testimg.img) { Remove-Item testimg.img -Force }
    $sizeBytes = 10MB
    $fs = [IO.File]::OpenWrite('testimg.img'); $fs.SetLength($sizeBytes); $fs.Close()
    Build-EynfsFormat
    & ./eynfs_format testimg.img 20480
}

function Create-FAT32Image {
    Write-Info 'Creating fat32.img (64MB)...'
    if (Test-Path fat32.img) { Remove-Item fat32.img -Force }
    $sizeBytes = 64MB
    $fs = [IO.File]::OpenWrite('fat32.img'); $fs.SetLength($sizeBytes); $fs.Close()
    if (Get-Command mkfs.fat -ErrorAction SilentlyContinue) {
        & mkfs.fat -F 32 -n EYNOS fat32.img
    } elseif (Get-Command mkfs.vfat -ErrorAction SilentlyContinue) {
        & mkfs.vfat -F 32 -n EYNOS fat32.img
    } else { Write-Err 'mkfs.fat/mkfs.vfat not found; install dosfstools.'; throw }
}

function Populate-FAT32Image {
    if (-not (Test-Path fat32.img)) { Create-FAT32Image }
    if (Get-Command mcopy -ErrorAction SilentlyContinue) {
        Write-Info 'Populating fat32.img with testdir contents (mtools)...'
        & mcopy -i fat32.img -s testdir/* ::/ 2>$null
    } else { Write-Warn 'mtools not found; skipping population.' }
}

function Run-Qemu($extraArgs) {
    Assert-Tool $EMULATOR 'Install QEMU (e.g. choco install qemu).'
    $args = @('-cdrom','EYNOS.iso','-hda','eynfs.img','-boot','d') + $extraArgs
    Write-Info "Launching QEMU: $EMULATOR $($args -join ' ')"
    & $EMULATOR $args
}

function Build-All {
    Assert-Tool $COMPILER 'Install cross-compiler i686-elf-gcc.'
    Assert-Tool $LINKER 'Install binutils for i686-elf.'
    Assert-Tool $ASSEMBLER 'Install NASM.'
    Build-KernelObjects
    Link-Kernel
    Generate-Docs
    Create-EynfsImage
    Create-ISO
}

function Analyze-Sources {
    Assert-Tool $COMPILER 'Install cross-compiler.'
    Write-Info 'Running static analyzer (-fanalyzer) over C sources...'
    Get-ChildItem -Recurse -Include *.c -Path src | ForEach-Object {
        Write-Host "Analyzing $($_.FullName)" -ForegroundColor DarkGray
        & $COMPILER $KERNEL_CFLAGS '-fanalyzer' $_.FullName '-o' $null 2>$null
    }
}

function Clean-Artifacts {
    Write-Info 'Cleaning artifacts...'
    Remove-Item -ErrorAction SilentlyContinue obj/*.o,tmp/boot/kernel.bin,*.img,EYNOS.iso,eynfs_format,fat32.img,testimg.img | Out-Null
    Get-ChildItem tmp -Filter 'grub_ultra_minimal.*' -Directory -ErrorAction SilentlyContinue | Remove-Item -Recurse -Force -ErrorAction SilentlyContinue
}

switch ($Target) {
    'build'        { Build-All }
    'run'          { Build-All; Run-Qemu @('-m','9M') }
    'qemu-debug'   { Build-All; Run-Qemu @('-serial','stdio','-d','int,cpu_reset','-no-reboot','-no-shutdown','-m','9M') }
    'qemu-gdb'     { Build-All; Run-Qemu @('-S','-s','-serial','stdio','-m','64M') }
    'clean'        { Clean-Artifacts }
    'eynfsimg'     { Create-EynfsImage }
    'testimg'      { Create-TestImage }
    'test'         { Build-All; Create-TestImage; Run-Qemu @('-hdb','testimg.img','-m','64M') }
    'fat32img'     { Create-FAT32Image }
    'fat32img-populate' { Populate-FAT32Image }
    'runfat32'     { Build-All; Create-FAT32Image; Run-Qemu @('-hdb','fat32.img','-m','64M') }
    'analyze'      { Analyze-Sources }
    'fsck_eynfs'   { Create-EynfsImage; if (Get-Command python3 -ErrorAction SilentlyContinue) { & python3 devtools/fsck_eynfs.py eynfs.img } else { Write-Warn 'python3 not found; cannot run fsck.' } }
    default        { Write-Err "Unknown target '$Target'"; Write-Host 'Valid targets: build, run, qemu-debug, qemu-gdb, clean, eynfsimg, testimg, test, fat32img, fat32img-populate, runfat32, analyze, fsck_eynfs' }
}

Write-Info "Target '$Target' completed." -ForegroundColor Green
