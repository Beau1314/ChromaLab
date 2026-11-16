# build-windows.ps1 -- the SDK and its host, with MinGW g++/ar. No CMake.
# Windows counterpart of build.sh (macOS/clang++); added locally under the
# project's MIT licence -- this checkout is not a git repository.
#
#   ./build-windows.ps1            build\libspklib.a + build\spklib-host.exe
#   ./build-windows.ps1 -Test      also build build\spklib-tests.exe (not Windows-clean)
#
# Prereqs (this machine's layout, override with parameters if it moves):
#   g++ 13.1.0 (MinGW-Builds, ships with the Qt 6.11.3 offline toolchain)
#   SQLite amalgamation at D:\spk-deps\sqlite (sqlite3.c/sqlite3.h, public domain)
param(
    [string]$Gxx     = "D:\spk-deps\qt-6.11.3\Tools\mingw1310_64\bin\g++.exe",
    [string]$Gcc     = "D:\spk-deps\qt-6.11.3\Tools\mingw1310_64\bin\gcc.exe",
    [string]$Ar      = "D:\spk-deps\qt-6.11.3\Tools\mingw1310_64\bin\ar.exe",
    [string]$Sqlite  = "D:\spk-deps\sqlite",
    [switch]$Test
)
$ErrorActionPreference = "Stop"
Set-Location -LiteralPath $PSScriptRoot

if (-not (Test-Path $Gxx))     { throw "g++ not found: $Gxx" }
if (-not (Test-Path "$Sqlite\sqlite3.c")) { throw "sqlite amalgamation not found: $Sqlite" }

$Version = (Get-Content -LiteralPath "VERSION" -Raw).Trim()
$Out     = "build"
$ObjDir  = Join-Path $Out "obj"
New-Item -ItemType Directory -Force -Path $ObjDir | Out-Null

# Same flags as build.sh's CXXFLAGS, MinGW-adjusted:
#   -std=c++20 -O2 -Wall -Wextra -Wno-unused-parameter, the same include roots,
#   the same SPKLIB_VERSION / SPKLIB_FIXTURES defines.
#   No -mmacosx-version-min; no -g here (release-style build, -DNDEBUG added).
#   -D_FILE_OFFSET_BITS=64 is default-on for mingw-w64 64-bit targets.
# The \"...\" is literal: gcc needs the escaped quotes on the command line or
# "0.4.0" reaches it as the (invalid) number literal 0.4.0.
$Fixtures = (Get-Location).Path + "\tests\fixtures"
$Common = @(
    "-std=c++20", "-O2", "-DNDEBUG", "-Wall", "-Wextra", "-Wno-unused-parameter",
    "-Iinclude", "-Isrc", "-Ithird_party", "-I$Sqlite",
    ('-DSPKLIB_VERSION=\"' + $Version + '\"'),
    ('-DSPKLIB_FIXTURES=\"' + $Fixtures + '\"')
)

$LibSources  = @("src\sqlite.cpp", "src\capture_one.cpp", "src\library.cpp", "src\api.cpp")
$SqliteFlags = @("-DSQLITE_ENABLE_FTS5", "-DSQLITE_ENABLE_RTREE", "-DSQLITE_THREADSAFE=1")

# 1. sqlite3.c (the amalgamation, compiled into the archive like build.sh links -lsqlite3).
#    gcc, not g++: the amalgamation is C, and g++ would choke on identifiers
#    like `new` that are legal C but C++ keywords.
& $Gcc @Common @SqliteFlags -c "$Sqlite\sqlite3.c" -o "$ObjDir\sqlite3.o"
if ($LASTEXITCODE -ne 0) { throw "sqlite3.c failed" }

# 2. the library sources
$objs = @("$ObjDir\sqlite3.o")
foreach ($s in $LibSources) {
    $o = Join-Path $ObjDir ((Split-Path $s -Leaf) -replace '\.cpp$', '.o')
    & $Gxx @Common -c $s -o $o
    if ($LASTEXITCODE -ne 0) { throw "$s failed" }
    $objs += $o
}

# 3. libspklib.a (ar, the mingw analogue of libtool -static)
$Archive = Join-Path $Out "libspklib.a"
if (Test-Path $Archive) { Remove-Item $Archive -Force }
& $Ar rcs $Archive @objs
if ($LASTEXITCODE -ne 0) { throw "ar failed" }

# 4. spklib-host.exe (stdio line-JSON helper)
& $Gxx @Common "host\main.cpp" $Archive -o "$Out\spklib-host.exe" -lws2_32
if ($LASTEXITCODE -ne 0) { throw "host failed" }

# 5. optional: the upstream test binary. Its POSIX includes are not shimmed;
#    kept behind -Test so the library build stays the supported surface.
if ($Test) {
    & $Gxx @Common "tests\tests.cpp" $Archive -o "$Out\spklib-tests.exe" -lws2_32
    if ($LASTEXITCODE -ne 0) { throw "tests failed" }
}

& "$Out\spklib-host.exe" --version
Write-Host "built: $Archive"
Write-Host "built: $Out\spklib-host.exe"
