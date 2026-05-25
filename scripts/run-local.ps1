param(
    [ValidateSet("mklm", "gguf", "stories15", "net", "nvidia", "ext2")]
    [string]$Mode = "mklm",

    [string]$WslPath = "",

    [string]$VfioHost = "",

    [ValidateSet("tcg", "kvm")]
    [string]$Accel = "tcg",

    [switch]$NoRun
)

$ErrorActionPreference = "Stop"

function Fail($Message) {
    Write-Error $Message
    exit 1
}

function Invoke-Wsl($Command) {
    wsl --cd $WslPath sh -c $Command
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
}

function ConvertTo-WslPath($Path) {
    if ($Path -match "^([A-Za-z]):\\(.*)$") {
        $drive = $Matches[1].ToLowerInvariant()
        $rest = $Matches[2] -replace "\\", "/"
        return "/mnt/$drive/$rest"
    }
    return $Path -replace "\\", "/"
}

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")

if (-not (Get-Command wsl.exe -ErrorAction SilentlyContinue)) {
    Fail "WSL no esta disponible. Instala WSL y una distro Linux para compilar MicroK en esta PC."
}

if (-not $WslPath) {
    $winPath = $repoRoot.Path
    $WslPath = ConvertTo-WslPath $winPath
}

$required = @("gcc", "nasm", "ld", "python3", "qemu-system-i386")
if ($Mode -in @("mklm", "gguf", "stories15", "net", "nvidia")) {
    $required += @("mkfs.fat", "mcopy", "mmd")
}
if ($Mode -eq "ext2") {
    $required += @("mkfs.ext2", "debugfs")
}

$check = ($required | ForEach-Object { "command -v $_ >/dev/null 2>&1 || echo $_" }) -join "; "
$missing = & wsl --cd $WslPath sh -c $check
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}
if ($missing) {
    $packages = "build-essential gcc-multilib nasm qemu-system-x86 python3"
    if ($missing -match "mkfs\.fat|mcopy|mmd") {
        $packages += " dosfstools mtools"
    }
    if ($missing -match "mkfs\.ext2|debugfs") {
        $packages += " e2fsprogs"
    }
    Fail "Faltan herramientas en WSL: $($missing -join ', '). En Ubuntu/Debian instala: sudo apt install $packages"
}

if ($Mode -eq "nvidia") {
    if (-not $NoRun -and -not $VfioHost) {
        Fail "Para -Mode nvidia falta -VfioHost. Ejemplo: .\scripts\run-local.ps1 -Mode nvidia -VfioHost 0000:01:00.0"
    }
    if (-not $NoRun -and $Accel -eq "tcg") {
        $Accel = "kvm"
    }
}

if ($NoRun) {
    $target = switch ($Mode) {
        "mklm" { "image-mklm" }
        "gguf" { "image-gguf" }
        "stories15" { "image-gguf" }
        "net" { "image-net" }
        "nvidia" { "image-mklm" }
        "ext2" { "image-ext2" }
    }
} else {
    $target = switch ($Mode) {
        "mklm" { "qemu" }
        "gguf" { "qemu-gguf" }
        "stories15" { "qemu-stories15" }
        "net" { "qemu-net" }
        "nvidia" { "qemu-nvidia" }
        "ext2" { "qemu-ext2" }
    }
}

Write-Host "MicroK local: modo=$Mode target=$target WSL=$WslPath accel=$Accel"
if ($Mode -eq "nvidia" -and -not $NoRun) {
    Invoke-Wsl "make QEMU_ACCEL=$Accel VFIO_HOST=$VfioHost $target"
} else {
    Invoke-Wsl "make QEMU_ACCEL=$Accel $target"
}
