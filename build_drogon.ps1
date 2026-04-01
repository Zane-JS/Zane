# Build Drogon for Z8
param(
    [Parameter(Mandatory=$false)]
    [ValidateSet("Debug", "Release")]
    [string]$Config = "Release"
)

$ErrorActionPreference = "Stop"

Write-Host "Building Drogon for Z8..." -ForegroundColor Cyan
Write-Host "Configuration: $Config" -ForegroundColor Yellow

# Check CMake
if (-not (Get-Command "cmake" -ErrorAction SilentlyContinue)) {
    Write-Error "CMake not found!"
    exit 1
}

# Paths
$drogonBuild = "deps/drogon/build"
$drogonInstall = "deps/drogon_install"

# Create build directory
if (-not (Test-Path $drogonBuild)) {
    New-Item -ItemType Directory -Path $drogonBuild | Out-Null
}

# Configure
Write-Host "[1/3] Configuring Drogon..." -ForegroundColor Green
Push-Location $drogonBuild

$cmakeArgs = @(
    "..",
    "-G", "NMake Makefiles",
    "-DCMAKE_BUILD_TYPE=$Config",
    "-DCMAKE_INSTALL_PREFIX=../../drogon_install",
    "-DBUILD_EXAMPLES=OFF",
    "-DBUILD_CTL=OFF",
    "-DBUILD_ORM=OFF",
    "-DBUILD_REDIS=OFF",
    "-DBUILD_TESTING=OFF",
    "-DBUILD_YAML_CONFIG=OFF",
    "-DCMAKE_CXX_STANDARD=17"
)

& cmake $cmakeArgs
if ($LASTEXITCODE -ne 0) {
    Pop-Location
    exit 1
}

# Build
Write-Host "[2/3] Building Drogon..." -ForegroundColor Green
& cmake --build . --config $Config --parallel
if ($LASTEXITCODE -ne 0) {
    Pop-Location
    exit 1
}

# Install
Write-Host "[3/3] Installing Drogon..." -ForegroundColor Green
& cmake --install . --config $Config
if ($LASTEXITCODE -ne 0) {
    Pop-Location
    exit 1
}

Pop-Location
Write-Host "Drogon built successfully!" -ForegroundColor Green
