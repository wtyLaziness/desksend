# SPDX-License-Identifier: MIT
$ErrorActionPreference = 'Stop'
$taskRoot = Join-Path $PSScriptRoot '../deskflow-bin/update-tests'
$null = New-Item -ItemType Directory -Path $taskRoot -Force
$runRoot = Join-Path $taskRoot ([guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Path $runRoot
$helper = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../deskflow-master/deploy/windows/Apply-DeskSendUpdate.ps1'))
$shell = Join-Path $env:SystemRoot 'System32/WindowsPowerShell/v1.0/powershell.exe'
$passed = 0

function Assert([bool]$Condition, [string]$Message) { if (!$Condition) { throw $Message } }
function Run-Case([string]$Name, [string]$Fault = '') {
    $caseRoot = Join-Path $runRoot $Name
    $install = Join-Path $caseRoot '已有 程序'
    $package = Join-Path $caseRoot 'package/DeskSend-test'
    $null = New-Item -ItemType Directory -Path (Join-Path $install 'settings/tls'),$package -Force
    [IO.File]::WriteAllText((Join-Path $install 'DeskSend.exe'), 'old-gui')
    [IO.File]::WriteAllText((Join-Path $install 'settings/Deskflow.conf'), 'existing-pair-and-layout')
    [IO.File]::WriteAllText((Join-Path $install 'settings/tls/deskflow.pem'), 'existing-private-key')
    $hashes = [ordered]@{}
    foreach ($component in @('DeskSend.exe','deskflow-core.exe','localsend-cli.exe')) {
        if ($Fault -eq 'missing-cli' -and $component -eq 'localsend-cli.exe') { continue }
        $file = Join-Path $package $component
        [IO.File]::WriteAllText($file, ('new-' + $component))
        $hashes[$component] = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant()
    }
    if ($Fault -eq 'inner-hash') { [IO.File]::WriteAllText((Join-Path $package 'DeskSend.exe'), 'corrupted') }
    $manifest = @{product='DeskSend';format=1;version='9.9.9';files=$hashes}
    $manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $package 'desksend-build.json') -Encoding UTF8
    $archive = Join-Path $caseRoot 'update.zip'
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [IO.Compression.ZipFile]::CreateFromDirectory((Join-Path $caseRoot 'package'), $archive)
    if ($Fault -eq 'traversal') {
        $zip = [IO.Compression.ZipFile]::Open($archive, [IO.Compression.ZipArchiveMode]::Update)
        try { $null = $zip.CreateEntry('../outside.txt') } finally { $zip.Dispose() }
    }
    $expected = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash
    if ($Fault -eq 'outer-hash') { $expected = '0' * 64 }
    $result = & $shell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $helper -InstallDir $install -Archive $archive -ExpectedHash $expected -NoRestart
    if ($Fault) {
        Assert ($LASTEXITCODE -eq 1) ('Expected failure: ' + $Name + ': ' + $result)
        Assert (([IO.File]::ReadAllText((Join-Path $install 'DeskSend.exe'))) -ceq 'old-gui') ('Original overwritten: ' + $Name)
    } else {
        Assert ($LASTEXITCODE -eq 0) ('Update failed: ' + $result)
        Assert (([IO.File]::ReadAllText((Join-Path $install 'DeskSend.exe'))) -ceq 'new-DeskSend.exe') 'GUI not updated'
        Assert (Test-Path -LiteralPath (Join-Path $install 'localsend-cli.exe')) 'CLI missing'
        Assert (@(Get-ChildItem -LiteralPath (Join-Path $install '.desksend-backups') -Recurse -Filter DeskSend.exe).Count -eq 1) 'Backup missing'
    }
    Assert (([IO.File]::ReadAllText((Join-Path $install 'settings/Deskflow.conf'))) -ceq 'existing-pair-and-layout') 'Settings changed'
    Assert (([IO.File]::ReadAllText((Join-Path $install 'settings/tls/deskflow.pem'))) -ceq 'existing-private-key') 'Certificate changed'
    Write-Output ('PASS: ' + $Name)
    $script:passed++
}
Run-Case 'success'
Run-Case 'wrong-archive-hash' 'outer-hash'
Run-Case 'missing-cli' 'missing-cli'
Run-Case 'wrong-file-hash' 'inner-hash'
Run-Case 'zip-slip' 'traversal'
Write-Output ('UPDATE_TESTS_PASSED: ' + $passed + '; evidence=' + $runRoot)

# 最后一个拒绝用例会留下非零退出码，显式报告整组测试成功。
exit 0
