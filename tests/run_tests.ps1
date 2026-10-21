$ErrorActionPreference = 'Stop'

$root = Split-Path $PSScriptRoot -Parent
$yx   = Join-Path $root 'yx.exe'
$bat  = Join-Path $root 'handeye.bat'

function Assert-True([bool]$condition, [string]$message) {
    if (-not $condition) { throw "FAIL: $message" }
}

function Invoke-Json([string]$exe, [string[]]$arguments) {
    $text = (& $exe @arguments | Out-String).Trim()
    if (-not $text) { throw "FAIL: no output from $exe $($arguments -join ' ')" }
    return ($text | ConvertFrom-Json)
}

Write-Host '1. binaries'
Assert-True (Test-Path $yx) 'yx.exe missing; run build.ps1 first'
Assert-True (Test-Path $bat) 'handeye.bat compatibility shim missing'

Write-Host '2. status'
$status = Invoke-Json $yx @('status')
Assert-True ($status.ok -eq $true) 'status.ok is not true'

Write-Host '3. framing L0 (local fail-open)'
$frame = Invoke-Json $yx @('framing', '--level', 'L0')
Assert-True ($frame.ok -eq $true) 'framing.ok is not true'
Assert-True ($frame.text.StartsWith('[L0]')) 'L0 text does not start with [L0]'
Assert-True ($frame.line_count -gt 0) 'L0 has no OCR lines'

Write-Host '4. ledger write + verify'
$dir = Join-Path $PSScriptRoot 'tmp\ledger'
if (Test-Path $dir) { Remove-Item $dir -Recurse -Force }
$null = & $yx log write --dir $dir
Assert-True ($LASTEXITCODE -eq 0) 'log write failed'
$null = & $yx log verify --dir $dir
Assert-True ($LASTEXITCODE -eq 0) 'clean ledger did not verify'
$log = Join-Path $dir 'yx.log'

Write-Host '5. ledger detects one changed line'
$text = [IO.File]::ReadAllText($log, [Text.Encoding]::UTF8)
$text = $text.Replace('n=1', 'n=2')
[IO.File]::WriteAllText($log, $text, (New-Object Text.UTF8Encoding($false)))
$null = & $yx log verify --dir $dir
Assert-True ($LASTEXITCODE -eq 5) 'tampered ledger was not rejected'

Write-Host '6. ledger detects deleted last line'
$dir2 = Join-Path $PSScriptRoot 'tmp\ledger-trunc'
if (Test-Path $dir2) { Remove-Item $dir2 -Recurse -Force }
1..3 | ForEach-Object { $null = & $yx log write --dir $dir2 }
$p2 = Join-Path $dir2 'yx.log'
$lines = [IO.File]::ReadAllLines($p2, [Text.Encoding]::UTF8)
[IO.File]::WriteAllLines($p2, $lines[0..($lines.Count - 2)], (New-Object Text.UTF8Encoding($false)))
$null = & $yx log verify --dir $dir2
Assert-True ($LASTEXITCODE -eq 5) 'truncated ledger was not rejected'

Write-Host '7. server framing + zoom'
$server = Start-Process -FilePath $yx -ArgumentList 'serve' -PassThru -WindowStyle Hidden
try {
    Start-Sleep -Milliseconds 800
    $f = Invoke-Json $yx @('framing', '--level', 'L0')
    Assert-True ($f.ok -eq $true) 'server framing failed'
    $z = Invoke-Json $yx @('zoom', '--ref', $f.ref)
    Assert-True ($z.ok -eq $true) 'zoom failed'
    Assert-True ($z.cached -eq $true) 'zoom did not use the cached frame'
}
finally {
    if ($server -and -not $server.HasExited) {
        Stop-Process -Id $server.Id -Force -ErrorAction SilentlyContinue
    }
}

Write-Host '8. organ command inside yx'
$cursor = Invoke-Json $yx @('cursor')
Assert-True ($cursor.ok -eq $true) 'yx cursor failed'

Write-Host '8b. handeye.bat compatibility'
$cursor2 = Invoke-Json $bat @('cursor')
Assert-True ($cursor2.ok -eq $true) 'handeye.bat cursor failed'

Write-Host '9. words table'
$words = Invoke-Json $yx @('words', '--json')
Assert-True ($words.Count -ge 30) "word table too small: $($words.Count)"
$names = @($words | ForEach-Object { $_.name })
Assert-True ($names -contains 'devmgmt') 'devmgmt word missing'
$schema = Invoke-Json $yx @('schema')
Assert-True ($schema.words -eq $words.Count) 'schema word count mismatch'

Write-Host '10. run a read-only command'
$who = Invoke-Json $yx @('run', 'whoami')
Assert-True ($who.ok -eq $true) 'yx run whoami failed'
Assert-True ($who.stdout -match 'User Name') 'whoami output missing User Name'

Write-Host ''
Write-Host 'ALL TESTS PASSED'
