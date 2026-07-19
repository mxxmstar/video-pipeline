param(
    [ValidateSet("build", "run", "test", "clean")]
    [string]$Action = "build",

    [ValidateSet("Debug", "Release", "RelWithDebInfo", "MinSizeRel")]
    [string]$Config = "Debug",

    [ValidateSet("tauri", "native", "all", "custom")]
    [string]$Profile = "tauri",

    [switch]$Tests,
    [switch]$NoPcap,
    [switch]$NoCli,
    [switch]$TauriModule,
    [switch]$NativePlayer,
    [string]$DepsRoot = "C:\vcpkg_env\vcpkg_installed\x64-windows",
    [string]$BoostRoot = ""
)

$ErrorActionPreference = "Stop"

$ProjectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$BuildDir = Join-Path $ProjectRoot "build"
$BuildTests = if ($Tests -or $Action -eq "test") { "ON" } else { "OFF" }
$EnablePcap = if ($NoPcap) { "OFF" } else { "ON" }
$BuildCli = if ($NoCli) { "OFF" } else { "ON" }
$BuildTauriModule = if ($TauriModule) { "ON" } else { "OFF" }
$BuildNativePlayer = if ($NativePlayer) { "ON" } else { "OFF" }
if (-not [string]::IsNullOrWhiteSpace($BoostRoot)) {
    $DepsRoot = $BoostRoot
}
$DepsRootArg = if ([string]::IsNullOrWhiteSpace($DepsRoot)) { "" } else { " -DVIDEO_PIPELINE_DEPS_ROOT=`"$DepsRoot`"" }

function Find-VcVars64 {
    $candidates = @(
        "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat",
        "C:\Program Files\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat",
        "C:\Program Files\Microsoft Visual Studio\17\Community\VC\Auxiliary\Build\vcvars64.bat",
        "C:\Program Files\Microsoft Visual Studio\17\BuildTools\VC\Auxiliary\Build\vcvars64.bat",
        "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat",
        "C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
    )

    foreach ($candidate in $candidates) {
        if (Test-Path $candidate) {
            return $candidate
        }
    }

    return $null
}

$VcVars64 = $null
if ($IsWindows -or $env:OS -eq "Windows_NT") {
    $hasCompiler = (Get-Command cl -ErrorAction SilentlyContinue) -and
                   (Get-Command nmake -ErrorAction SilentlyContinue)
    if (-not $hasCompiler) {
        $VcVars64 = Find-VcVars64
    }
}

function Invoke-Native {
    param(
        [Parameter(Mandatory = $true)]
        [string]$FilePath,
        [Parameter(ValueFromRemainingArguments = $true)]
        [string[]]$Arguments
    )

    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$FilePath failed with exit code $LASTEXITCODE"
    }
}

function Invoke-CMakeInEnvironment {
    param(
        [Parameter(Mandatory = $true)]
        [string]$CommandLine
    )

    if ($VcVars64) {
        $cmd = "call `"$VcVars64`" >nul && $CommandLine"
        & cmd.exe /d /s /c $cmd
        if ($LASTEXITCODE -ne 0) {
            throw "Command failed with exit code $LASTEXITCODE`: $CommandLine"
        }
    } else {
        & cmd.exe /d /s /c $CommandLine
        if ($LASTEXITCODE -ne 0) {
            throw "Command failed with exit code $LASTEXITCODE`: $CommandLine"
        }
    }
}

function Configure-Project {
    if ($VcVars64) {
        Invoke-CMakeInEnvironment "cmake -S `"$ProjectRoot`" -B `"$BuildDir`" -G `"NMake Makefiles`" -DBUILD_TESTS=$BuildTests -DENABLE_PCAP=$EnablePcap -DVIDEO_PIPELINE_PROFILE=$Profile -DVIDEO_PIPELINE_BUILD_CLI=$BuildCli -DVIDEO_PIPELINE_BUILD_TAURI_MODULE=$BuildTauriModule -DVIDEO_PIPELINE_BUILD_NATIVE_PLAYER=$BuildNativePlayer$DepsRootArg -DCMAKE_BUILD_TYPE=$Config"
    } else {
        $configureArgs = @(
            "-S", $ProjectRoot,
            "-B", $BuildDir,
            "-DBUILD_TESTS=$BuildTests",
            "-DENABLE_PCAP=$EnablePcap",
            "-DVIDEO_PIPELINE_PROFILE=$Profile",
            "-DVIDEO_PIPELINE_BUILD_CLI=$BuildCli",
            "-DVIDEO_PIPELINE_BUILD_TAURI_MODULE=$BuildTauriModule",
            "-DVIDEO_PIPELINE_BUILD_NATIVE_PLAYER=$BuildNativePlayer",
            "-DCMAKE_BUILD_TYPE=$Config"
        )
        if (-not [string]::IsNullOrWhiteSpace($DepsRoot)) {
            $configureArgs += "-DVIDEO_PIPELINE_DEPS_ROOT=$DepsRoot"
        }
        Invoke-Native cmake @configureArgs
    }
}

function Build-Project {
    Configure-Project
    if ($VcVars64) {
        Invoke-CMakeInEnvironment "cmake --build `"$BuildDir`" --config $Config"
    } else {
        Invoke-Native cmake --build $BuildDir --config $Config --parallel
    }
}

function Get-AppPath {
    $candidates = @(
        (Join-Path $BuildDir "bin\video-pipeline.exe"),
        (Join-Path $BuildDir "bin\$Config\video-pipeline.exe"),
        (Join-Path $BuildDir "$Config\video-pipeline.exe"),
        (Join-Path $BuildDir "video-pipeline.exe")
    )

    foreach ($candidate in $candidates) {
        if (Test-Path $candidate) {
            return $candidate
        }
    }

    throw "video-pipeline executable was not found. Build output was checked under $BuildDir."
}

switch ($Action) {
    "clean" {
        if (Test-Path $BuildDir) {
            $resolvedProject = [System.IO.Path]::GetFullPath($ProjectRoot)
            $resolvedBuild = [System.IO.Path]::GetFullPath($BuildDir)
            if (-not $resolvedBuild.StartsWith($resolvedProject, [System.StringComparison]::OrdinalIgnoreCase)) {
                throw "Refusing to delete path outside project: $resolvedBuild"
            }
            Remove-Item -LiteralPath $BuildDir -Recurse -Force
        }
        Write-Host "Clean completed."
    }
    "build" {
        Build-Project
        if ($BuildCli -eq "ON") {
            Write-Host "Build completed: $(Get-AppPath)"
        } else {
            Write-Host "Build completed."
        }
    }
    "run" {
        if ($BuildCli -ne "ON") {
            throw "Action 'run' requires VIDEO_PIPELINE_BUILD_CLI=ON. Remove -NoCli and try again."
        }
        Build-Project
        $app = Get-AppPath
        Write-Host "Running: $app"
        & $app
    }
    "test" {
        Build-Project
        if ($VcVars64) {
            Invoke-CMakeInEnvironment "ctest --test-dir `"$BuildDir`" -C $Config --output-on-failure"
        } else {
            Invoke-Native ctest --test-dir $BuildDir -C $Config --output-on-failure
        }
    }
}
