# SPDX-License-Identifier: GPL-3.0-only
# Builds and runs the F4 host tests with MSVC: display text (UTF-8, Cyrillic glyphs) and the push-to-talk audio
# path (sample conversion, ring buffer, stream and privacy invariants). The Linux CI job runs run_f4.sh with cc
# instead; both compile the same sources.
#
#   .\tests\run_f4.ps1                 uses .toolchain\esp-idf for cJSON and mbedTLS
#   $env:IDF_PATH = '...'; .\tests\run_f4.ps1
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$idf = if ($env:IDF_PATH) { $env:IDF_PATH } else { Join-Path $root '.toolchain\esp-idf' }
$out = if ($env:MOBY_HOST_OUT) { $env:MOBY_HOST_OUT } else { Join-Path $env:TEMP 'moby-f4' }
New-Item -ItemType Directory -Force $out | Out-Null

if (-not (Get-Command cl -ErrorAction SilentlyContinue)) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $install = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    $saved = @{ TEMP = $env:TEMP; TMP = $env:TMP }
    $dump = cmd /c "`"$install\Common7\Tools\VsDevCmd.bat`" -arch=x64 -no_logo >nul 2>nul && set"
    foreach ($line in $dump) { if ($line -match '^([^=]+)=(.*)$') { Set-Item "env:$($Matches[1])" $Matches[2] } }
    $env:TEMP = $saved.TEMP; $env:TMP = $saved.TMP
}

$cjson = Join-Path $idf 'components\json\cJSON'
$mbed = Join-Path $idf 'components\mbedtls\mbedtls'
$host_inc = Join-Path $root 'tests\host'
$main = Join-Path $root 'main'
$proto = Join-Path $root 'components\moby_protocol'

function Invoke-Cl {
    & cl /nologo @args
    if ($LASTEXITCODE -ne 0) { throw "cl failed: $args" }
}

Push-Location $out
try {
    $common = @('/std:c11', '/W4', '/WX', '/D_CRT_SECURE_NO_WARNINGS', '/I', $main, '/I', "$proto\include", '/I', $cjson, '/I', $host_inc, '/I', "$mbed\include")

    # Display text needs only the font and the face.
    New-Item -ItemType Directory -Force "$out\text" | Out-Null
    Invoke-Cl @common "/Fo:$out\text\" "/Fe:$out\text.exe" "$main\moby_font.c" "$main\moby_face.c" "$root\tests\text.c"
    & "$out\text.exe"
    if ($LASTEXITCODE -ne 0) { throw 'text failed' }

    # The audio path runs the real session and link, so it needs the same third-party code as the F3 tests.
    Invoke-Cl /c /W0 /I $cjson "$cjson\cJSON.c"
    foreach ($name in 'md', 'sha256', 'platform_util', 'constant_time') {
        Invoke-Cl /c /W0 /I $host_inc /I "$mbed\include" /I "$mbed\library" "$mbed\library\$name.c"
    }
    $third = @('cJSON.obj', 'md.obj', 'sha256.obj', 'platform_util.obj', 'constant_time.obj')
    $ours = @(
        "$proto\c\moby_usb_codec.c", "$main\moby_json.c", "$main\moby_session.c", "$main\moby_config.c",
        "$main\moby_link.c", "$main\moby_hmac.c", "$main\moby_face.c", "$main\moby_font.c", "$main\moby_pcm.c",
        "$main\moby_ring.c"
    )
    New-Item -ItemType Directory -Force "$out\f4" | Out-Null
    Invoke-Cl @common "/Fo:$out\f4\" "/Fe:$out\f4.exe" @ours "$root\tests\f4.c" @third
    & "$out\f4.exe"
    if ($LASTEXITCODE -ne 0) { throw 'f4 failed' }
}
finally {
    Pop-Location
}
