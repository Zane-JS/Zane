# Zane V8 (Z8) Master Build Script (Incremental)

param(
    [Parameter(Mandatory=$false)]
    [ValidateSet("Debug", "Release")]
    [string]$Config = "Release",
    
    [Parameter(Mandatory=$false)]
    [switch]$UseIOCP = $true
)

# --- Helper Functions ---

function Get-LatestWriteTime {
    param($Paths)
    $latest = [DateTime]::MinValue
    foreach ($p in $Paths) {
        if (Test-Path $p) {
            if ((Get-Item $p).PSIsContainer) {
                # Directory: check all source/header files inside
                $files = Get-ChildItem -Path $p -Recurse -File -Include *.h,*.c,*.cpp,*.hpp
                if ($files) {
                    $m = ($files | Measure-Object -Property LastWriteTime -Maximum).Maximum
                    if ($m -gt $latest) { $latest = $m }
                }
            } else {
                # Single file
                $m = (Get-Item $p).LastWriteTime
                if ($m -gt $latest) { $latest = $m }
            }
        }
    }
    return $latest
}

function Needs-Rebuild {
    param($Sources, $Target)
    if (-not (Test-Path $Target)) { return $true }
    $targetTime = (Get-Item $Target).LastWriteTime
    $sourceTime = Get-LatestWriteTime -Paths $Sources
    return $sourceTime -gt $targetTime
}

# --- Step 0: Check coding style ---
Write-Host "[Step 0/4] Checking coding style..."
if (Test-Path "tools/check_style.py") {
    python tools/check_style.py ./src/
    if ($LASTEXITCODE -ne 0) {
        Write-Error "Coding style check failed! Please fix the errors before building."
        exit 1
    }
} else {
    Write-Warning "Style checker (tools/check_style.py) not found. Skipping..."
}

# --- Step 1: Ensure MSVC environment is loaded ---
Write-Host "[Step 1/4] Ensuring MSVC environment is loaded..."
if (-not $env:INCLUDE) {
    Write-Host "MSVC environment not detected. Attempting to locate Visual Studio..."
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $installPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($installPath) {
            $vcvars = Join-Path $installPath "VC\Auxiliary\Build\vcvars64.bat"
            if (Test-Path $vcvars) {
                Write-Host "Loading environment from $vcvars..."
                $envVars = cmd /c "`"$vcvars`" x64 && set"
                foreach ($line in $envVars) {
                    if ($line -match "^([^=]+)=(.*)$") {
                        $name = $matches[1]
                        $value = $matches[2]
                        Set-Item -Path "Env:\$name" -Value $value
                    }
                }
            }
        }
    }
}

if (-not (Get-Command "cl.exe" -ErrorAction SilentlyContinue)) {
    Write-Error "cl.exe not found in PATH. Please run this script from a Developer Command Prompt or ensure Visual Studio is installed."
    exit 1
}

# Ensure build directories exist
$buildDirs = @("build", "build\obj", "build\lib")
foreach ($dir in $buildDirs) {
    if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Path $dir | Out-Null }
}

Write-Host "Building Zane V8 (Z8)..."
Write-Host "Configuration: $Config"

# --- Step 2: Extract Shims ---
Write-Host "[Step 2/4] Extracting Temporal shims..."
if (Test-Path "v8/libs/v8_monolith.lib") {
    python tools/extract_shims.py
}

# --- Flags Setup ---
$cppFlags = @(
    "/std:c++latest", "/Zc:__cplusplus", "/EHsc", 
    "/O2", "/Oi", "/Ot", "/MT", "/DNDEBUG",
    "/DV8_COMPRESS_POINTERS",
    "/nologo", "/c",
    "/Iv8/include", "/Isrc", "/Ideps/zlib", "/Ideps/brotli/c/include", "/Ideps/zstd/lib",
    "/Ideps/uSockets/src"
)

# Enable native IOCP for Windows (Z8's competitive advantage)
if ($UseIOCP) {
    Write-Host "Using native Windows IOCP backend (high performance, zero libuv dependency)"
    $cppFlags += "/DLIBUS_USE_IOCP"
    $cppFlags += "/DLIBUS_NO_SSL"  # Disable SSL for now
} else {
    Write-Host "Using libuv backend (fallback mode)"
    $cppFlags += "/DLIBUS_USE_LIBUV"
}

$linkFlags = @(
    "/OUT:z8.exe", "/SUBSYSTEM:CONSOLE", "/MACHINE:X64", "/NOLOGO",
    "V8\out.gn\x64.release\obj\v8_monolith.lib",
    "libcmt.lib", "libcpmt.lib",
    "winmm.lib", "dbghelp.lib", "shlwapi.lib", "user32.lib", "iphlpapi.lib",
    "advapi32.lib", "shell32.lib", "ole32.lib", "uuid.lib", "rpcrt4.lib", "ntdll.lib", "userenv.lib",
    "ws2_32.lib"
)

if ($Config -eq "Release") {
    Write-Host "Enabling Whole Program Optimization (/GL /LTCG)..."
    $cppFlags += "/GL"
    $linkFlags += "/LTCG"
}

if ($Config -eq "Debug") {
    Write-Host "Enabling Z8_DEBUG_VERBOSE macro..."
    $cppFlags += "/DZ8_DEBUG_VERBOSE"
    $linkFlags += "/DEBUG"
}

# --- Step 3: Incremental Compilation ---
Write-Host "[3/4] Compiling modules..."

# 3.1: ZLib
$zlibSources = @(
    "deps/zlib/adler32.c", "deps/zlib/compress.c", "deps/zlib/crc32.c", "deps/zlib/deflate.c", 
    "deps/zlib/infback.c", "deps/zlib/inffast.c", "deps/zlib/inflate.c", "deps/zlib/inftrees.c", 
    "deps/zlib/trees.c", "deps/zlib/uncompr.c", "deps/zlib/zutil.c"
)
$zlibLib = "build\lib\zlib.lib"
if (Needs-Rebuild -Sources @("deps/zlib") -Target $zlibLib) {
    Write-Host "Rebuilding zlib..."
    & cl.exe $cppFlags /Fo"build\obj\" $zlibSources
    if ($LASTEXITCODE -ne 0) { exit 1 }
    $objs = $zlibSources | ForEach-Object { "build\obj\" + [System.IO.Path]::GetFileNameWithoutExtension($_) + ".obj" }
    & lib.exe /NOLOGO /OUT:$zlibLib $objs
    if ($LASTEXITCODE -ne 0) { exit 1 }
}
$linkFlags += $zlibLib

# 3.2: Brotli
$brotliSources = @(
    "deps/brotli/c/common/constants.c", "deps/brotli/c/common/context.c", "deps/brotli/c/common/dictionary.c", 
    "deps/brotli/c/common/platform.c", "deps/brotli/c/common/shared_dictionary.c", "deps/brotli/c/common/transform.c",
    "deps/brotli/c/dec/bit_reader.c", "deps/brotli/c/dec/decode.c", "deps/brotli/c/dec/huffman.c", 
    "deps/brotli/c/dec/prefix.c", "deps/brotli/c/dec/state.c",
    "deps/brotli/c/enc/backward_references.c", "deps/brotli/c/enc/backward_references_hq.c", "deps/brotli/c/enc/bit_cost.c", 
    "deps/brotli/c/enc/block_splitter.c", "deps/brotli/c/enc/brotli_bit_stream.c", "deps/brotli/c/enc/cluster.c", 
    "deps/brotli/c/enc/command.c", "deps/brotli/c/enc/compound_dictionary.c", "deps/brotli/c/enc/compress_fragment.c", 
    "deps/brotli/c/enc/compress_fragment_two_pass.c", "deps/brotli/c/enc/dictionary_hash.c", "deps/brotli/c/enc/encode.c", 
    "deps/brotli/c/enc/encoder_dict.c", "deps/brotli/c/enc/entropy_encode.c", "deps/brotli/c/enc/fast_log.c", 
    "deps/brotli/c/enc/histogram.c", "deps/brotli/c/enc/literal_cost.c", "deps/brotli/c/enc/memory.c", 
    "deps/brotli/c/enc/metablock.c", "deps/brotli/c/enc/static_dict.c", "deps/brotli/c/enc/static_dict_lut.c", "deps/brotli/c/enc/utf8_util.c"
)
$brotliLib = "build\lib\brotli.lib"
if (Needs-Rebuild -Sources @("deps/brotli/c") -Target $brotliLib) {
    Write-Host "Rebuilding brotli..."
    & cl.exe $cppFlags /Fo"build\obj\" $brotliSources
    if ($LASTEXITCODE -ne 0) { exit 1 }
    # Handle the specific files individually to avoid collisions, then archive
    & cl.exe $cppFlags /Fobuild\obj\static_init_dec.obj deps/brotli/c/dec/static_init.c
    & cl.exe $cppFlags /Fobuild\obj\static_init_enc.obj deps/brotli/c/enc/static_init.c
    
    $objs = $brotliSources | ForEach-Object { "build\obj\" + [System.IO.Path]::GetFileNameWithoutExtension($_) + ".obj" }
    $objs += "build\obj\static_init_dec.obj"
    $objs += "build\obj\static_init_enc.obj"
    & lib.exe /NOLOGO /OUT:$brotliLib $objs
    if ($LASTEXITCODE -ne 0) { exit 1 }
}
$linkFlags += $brotliLib

# 3.3: ZStd
$zstdSources = @(
    "deps/zstd/lib/common/debug.c", "deps/zstd/lib/common/entropy_common.c", "deps/zstd/lib/common/error_private.c", 
    "deps/zstd/lib/common/fse_decompress.c", "deps/zstd/lib/common/pool.c", "deps/zstd/lib/common/threading.c", 
    "deps/zstd/lib/common/xxhash.c", "deps/zstd/lib/common/zstd_common.c",
    "deps/zstd/lib/compress/fse_compress.c", "deps/zstd/lib/compress/hist.c", "deps/zstd/lib/compress/huf_compress.c", 
    "deps/zstd/lib/compress/zstd_compress.c", "deps/zstd/lib/compress/zstd_compress_literals.c", 
    "deps/zstd/lib/compress/zstd_compress_sequences.c", "deps/zstd/lib/compress/zstd_compress_superblock.c", 
    "deps/zstd/lib/compress/zstd_double_fast.c", "deps/zstd/lib/compress/zstd_fast.c", "deps/zstd/lib/compress/zstd_lazy.c", 
    "deps/zstd/lib/compress/zstd_ldm.c", "deps/zstd/lib/compress/zstd_opt.c", "deps/zstd/lib/compress/zstdmt_compress.c",
    "deps/zstd/lib/decompress/huf_decompress.c", "deps/zstd/lib/decompress/zstd_ddict.c", 
    "deps/zstd/lib/decompress/zstd_decompress.c", "deps/zstd/lib/decompress/zstd_decompress_block.c"
)
$zstdLib = "build\lib\zstd.lib"
if (Needs-Rebuild -Sources @("deps/zstd/lib") -Target $zstdLib) {
    Write-Host "Rebuilding zstd..."
    & cl.exe $cppFlags /Fo"build\obj\" $zstdSources
    if ($LASTEXITCODE -ne 0) { exit 1 }
    & cl.exe $cppFlags /Fobuild\obj\zstd_preSplit.obj deps/zstd/lib/compress/zstd_preSplit.c
}
$linkFlags += $zstdLib

# 3.5: Z8 Core
$coreSources = @(
    "src/main.cpp", "src/temporal_shims.cpp", "src/module/console.cpp", "src/module/node/fs/fs.cpp", 
    "src/module/node/path/path.cpp", "src/module/node/os/os.cpp", "src/module/node/process/process.cpp", 
    "src/module/node/util/util.cpp", "src/module/node/buffer/buffer.cpp", "src/module/node/zlib/zlib.cpp",
    "src/module/node/events/events.cpp", "src/module/node/stream/stream.cpp", "src/module/node/http/http.cpp",
    "src/module/timer.cpp"
)

$coreObjs = @()
foreach ($src in $coreSources) {
    $obj = "build\obj\" + [System.IO.Path]::GetFileNameWithoutExtension($src) + ".obj"
    $coreObjs += $obj
    if (Needs-Rebuild -Sources $src -Target $obj) {
        Write-Host "Compiling $src..."
        & cl.exe $cppFlags /Fo$obj $src
        if ($LASTEXITCODE -ne 0) { exit 1 }
    }
}
$linkFlags = $coreObjs + $linkFlags

# --- Step 4: Link ---
Write-Host "[4/4] Linking z8.exe..."
& link.exe $linkFlags

if ($LASTEXITCODE -ne 0) {
    Write-Host "Linking failed!"
    exit 1
}

Write-Host "Zane V8 (Z8) is ready ($Config)!"
Write-Host "Run it with: .\z8.exe test.js"
