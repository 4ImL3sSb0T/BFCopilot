[CmdletBinding()]
param(
    [ValidateSet('bench', 'flight', 'remote')]
    [string]$Firmware = 'bench',
    [string]$Port = 'COM4',
    [ValidateRange(9600, 2000000)]
    [int]$Baud = 460800,
    [switch]$Monitor,
    [switch]$BuildOnly,
    [string]$BuildDirectory,
    [string]$DefaultsFile,
    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'
try {
    $repoRoot = Split-Path -Parent $PSScriptRoot
    $projectDirectory = $repoRoot
    $idfArguments = @()
    if ($Firmware -eq 'bench') {
        $projectDirectory = Join-Path $repoRoot 'test/bench'
        $idfArguments += @('-C', $projectDirectory)
        $defaultBuild = Join-Path $projectDirectory 'build'
        $defaultFiles = @((Join-Path $projectDirectory 'sdkconfig.defaults'))
        $localDefaults = Join-Path $projectDirectory 'local.defaults'
    } else {
        $defaultBuild = Join-Path $repoRoot "build-flash-$Firmware"
        $defaultFiles = @((Join-Path $repoRoot 'sdkconfig.defaults'))
        $localDefaults = Join-Path $repoRoot "$Firmware.local.defaults"
        $idfArguments += @('-D', "BF_REMOTE=$(if ($Firmware -eq 'remote') { '1' } else { '0' })")
    }
    if (-not $BuildDirectory) { $BuildDirectory = $defaultBuild }
    if (-not [System.IO.Path]::IsPathRooted($BuildDirectory)) {
        $BuildDirectory = Join-Path $repoRoot $BuildDirectory
    }
    $BuildDirectory = [System.IO.Path]::GetFullPath($BuildDirectory)
    if ($Firmware -eq 'remote') {
        $defaultFiles += Join-Path $PSScriptRoot 'remote.defaults'
        $idfArguments += @('-D', "SDKCONFIG=$(Join-Path $BuildDirectory 'sdkconfig')")
    }
    if ($DefaultsFile) {
        $localDefaults = (Resolve-Path -LiteralPath $DefaultsFile).Path
    }
    if (Test-Path -LiteralPath $localDefaults) {
        $defaultFiles += $localDefaults
    } elseif ($Firmware -eq 'remote' -and -not $DryRun) {
        throw 'Remote provisioning is missing. Copy scripts/radio.local.defaults.example to remote.local.defaults and fill in the peer MAC, PMK and LMK.'
    }
    $idfArguments += @('-B', $BuildDirectory, '-D', "SDKCONFIG_DEFAULTS=$($defaultFiles -join ';')")
    if ($BuildOnly) {
        $idfArguments += 'build'
    } else {
        if ($Port -notmatch '^COM[0-9]+$') { throw 'Use a Windows serial port such as COM4.' }
        $idfArguments += @('-p', $Port, '-b', "$Baud", 'flash')
        if ($Monitor) { $idfArguments += 'monitor' }
    }
    Write-Host "Firmware: $Firmware"
    Write-Host "Build: $BuildDirectory"
    Write-Host "idf.cmd $($idfArguments -join ' ')"
    if ($DryRun) { exit 0 }
    $env:CCACHE_DIR = Join-Path $repoRoot '.cache/ccache'
    & (Join-Path $PSScriptRoot 'idf.cmd') @idfArguments
    $resultCode = $LASTEXITCODE
    if ($resultCode -ne 0) {
        Write-Host 'Build/flash failed. If the serial port is busy, close its monitor and retry.'
    }
    exit $resultCode
} catch {
    Write-Error $_ -ErrorAction Continue
    exit 1
}
