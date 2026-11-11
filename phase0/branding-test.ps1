# SPDX-License-Identifier: MIT
param([Parameter(Mandatory)][string]$DeskflowDir, [Parameter(Mandatory)][string]$OutDir)
$ErrorActionPreference = 'Stop'
$null = New-Item -ItemType Directory -Path $OutDir -Force
$guiExe = Join-Path $DeskflowDir 'DeskSend.exe'
if (!(Test-Path -LiteralPath $guiExe)) { throw 'DeskSend.exe missing' }
foreach ($program in @('DeskSend.exe','deskflow-core.exe')) {
    $info = (Get-Item -LiteralPath (Join-Path $DeskflowDir $program)).VersionInfo
    if ($info.ProductName -cne 'DeskSend' -or $info.ProductVersion -cne '1.0.0.0') {
        throw ('Incorrect product identity: ' + $program + ': ' + $info.ProductName + ' ' + $info.ProductVersion)
    }
}
$stdout = Join-Path $OutDir 'version.txt'
$stderr = Join-Path $OutDir 'version-error.txt'
$process = Start-Process -FilePath $guiExe -ArgumentList '--version' -WorkingDirectory $DeskflowDir -WindowStyle Hidden -RedirectStandardOutput $stdout -RedirectStandardError $stderr -PassThru
if (!$process.WaitForExit(20000)) { Stop-Process -Id $process.Id; throw 'GUI version check timed out' }
if ($process.ExitCode -ne 0 -or (Get-Content -LiteralPath $stdout -Raw -Encoding UTF8) -notmatch 'DeskSend: 1\.0\.0') {
    throw ('GUI failed version smoke test: ' + (Get-Content -LiteralPath $stderr -Raw -Encoding UTF8))
}
Add-Type -AssemblyName System.Drawing
$icon = [Drawing.Icon]::ExtractAssociatedIcon($guiExe)
try {
    $bitmap = $icon.ToBitmap()
    try { $bitmap.Save((Join-Path $OutDir 'desksend-exe-icon.png'), [Drawing.Imaging.ImageFormat]::Png) }
    finally { $bitmap.Dispose() }
} finally { $icon.Dispose() }
Write-Output 'BRANDING_TEST_PASSED: DeskSend.exe; product=DeskSend; version=1.0.0'
