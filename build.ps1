$ErrorActionPreference = 'Stop'

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) {
    throw "vswhere.exe not found. Install Visual Studio 2022 with Desktop development with C++."
}

$tmp = Join-Path $env:TEMP ("yx-vswhere-" + [Guid]::NewGuid().ToString("N") + ".txt")
try {
    cmd /c "`"$vswhere`" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath -utf8 > `"$tmp`""
    $vs = Get-Content -LiteralPath $tmp -Encoding UTF8 |
        Where-Object { $_.Trim().Length -gt 0 } |
        Select-Object -First 1
    if ($vs) { $vs = $vs.Trim() }
}
finally {
    if (Test-Path -LiteralPath $tmp) { Remove-Item -LiteralPath $tmp -Force }
}

if (-not $vs) {
    throw "No Visual Studio installation with the C++ toolchain was found."
}

$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path -LiteralPath $vcvars)) {
    throw "vcvars64.bat not found under $vs"
}

Push-Location $PSScriptRoot
try {
    cmd /c "call `"$vcvars`" >nul && build.bat"
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
}
finally {
    Pop-Location
}
