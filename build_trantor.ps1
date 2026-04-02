# Build Trantor for Z8
param(
    [Parameter(Mandatory=$false)]
    [ValidateSet("Debug", "Release")]
    [string]$Config = "Release"
)

$ErrorActionPreference = "Stop"

Write-Host "Building Trantor for Z8..." -ForegroundColor Cyan
Write-Host "Configuration: $Config" -ForegroundColor Yellow

# Check CMake
if (-not (Get-Command "cmake" -ErrorAction SilentlyContinue)) {
    Write-Error "CMake not found!"
    exit 1
}

# Paths
$trantorBuild = "deps/trantor/build"
$trantorInstall = "deps/trantor_install"

# Create build directory
if (-not (Test-Path $trantorBuild)) {
    New-Item -ItemType Directory -Path $trantorBuild | Out-Null
}

# Configure
Write-Host "[1/2] Configuring Trantor..." -ForegroundColor Green
Push-Location $trantorBuild

$cmakeArgs = @(
    "..",
    "-G", "NMake Makefiles",
    "-DCMAKE_BUILD_TYPE=$Config",
    "-DCMAKE_INSTALL_PREFIX=../../trantor_install",
    "-DBUILD_TESTING=OFF",
    "-DCMAKE_CXX_STANDARD=17",
    "-DCMAKE_POSITION_INDEPENDENT_CODE=ON",
    "-DCMAKE_C_FLAGS_RELEASE=/MT",
    "-DCMAKE_CXX_FLAGS_RELEASE=/MT",
    "-DCMAKE_C_FLAGS_DEBUG=/MTd",
    "-DCMAKE_CXX_FLAGS_DEBUG=/MTd"
)

# Optional: Add OpenSSL path if available
if (Test-Path "../../deps/openssl_install") {
    $cmakeArgs += "-DOPENSSL_ROOT_DIR=../../deps/openssl_install"
} else {
    # If no OpenSSL, try to disable it or let it fail gracefully
    Write-Warning "OpenSSL not found in deps/openssl_install. HTTPS might be disabled."
}

& cmake $cmakeArgs
if ($LASTEXITCODE -ne 0) {
    Pop-Location
    exit 1
}

# Build and Install
Write-Host "[2/2] Building and Installing Trantor..." -ForegroundColor Green
& cmake --build . --config $Config --target install
if ($LASTEXITCODE -ne 0) {
    Pop-Location
    exit 1
}

Pop-Location
Write-Host "Trantor built successfully!" -ForegroundColor Green
