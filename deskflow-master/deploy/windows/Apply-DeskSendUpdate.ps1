# SPDX-License-Identifier: MIT
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$InstallDir,
    [Parameter(Mandatory)][string]$Archive,
    [Parameter(Mandatory)][ValidatePattern('^[a-fA-F0-9]{64}$')][string]$ExpectedHash,
    [int]$GuiProcessId = 0,
    [switch]$NoRestart
)
$ErrorActionPreference = 'Stop'
$installRoot = [IO.Path]::GetFullPath($InstallDir).TrimEnd('\')
$workRoot = Join-Path ([IO.Path]::GetTempPath()) ('DeskSend-apply-' + [guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Path $workRoot
$logPath = Join-Path $workRoot 'update.log'
$changed = [Collections.Generic.List[object]]::new()
$copyStarted = $false
$stopped = $false

function Write-UpdateLog([string]$Message) {
    Add-Content -LiteralPath $logPath -Value $Message -Encoding UTF8
}
function Get-ChildPath([string]$Root, [string]$Relative) {
    if ([IO.Path]::IsPathRooted($Relative) -or $Relative.Contains(':')) { throw '更新包包含非法路径。' }
    $full = [IO.Path]::GetFullPath((Join-Path $Root $Relative))
    if (!$full.StartsWith($Root.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) { throw '更新路径越界。' }
    return $full
}
function Start-DeskSend {
    if (!$NoRestart) {
        Start-Process -FilePath (Join-Path $installRoot 'DeskSend.exe') -WorkingDirectory $installRoot -WindowStyle Hidden
    }
}

try {
    if (!(Test-Path -LiteralPath (Join-Path $installRoot 'DeskSend.exe') -PathType Leaf)) { throw '找不到原有 DeskSend 程序。' }
    if ((Get-FileHash -LiteralPath $Archive -Algorithm SHA256).Hash -ine $ExpectedHash) { throw '更新包 SHA-256 校验失败。' }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $stageRoot = Join-Path $workRoot 'stage'
    $null = New-Item -ItemType Directory -Path $stageRoot
    $zip = [IO.Compression.ZipFile]::OpenRead($Archive)
    try {
        foreach ($entry in $zip.Entries) {
            $destination = Get-ChildPath $stageRoot $entry.FullName
            if ($entry.FullName.EndsWith('/') -or $entry.FullName.EndsWith('\')) {
                $null = New-Item -ItemType Directory -Path $destination -Force
            } else {
                $null = New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($destination)) -Force
                [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $destination, $false)
            }
        }
    } finally { $zip.Dispose() }
    $roots = @(Get-ChildItem -LiteralPath $stageRoot)
    if ($roots.Count -ne 1 -or !$roots[0].PSIsContainer) { throw '更新包必须包含一个 DeskSend 目录。' }
    $packageRoot = $roots[0].FullName
    $manifest = Get-Content -LiteralPath (Join-Path $packageRoot 'desksend-build.json') -Encoding UTF8 -Raw | ConvertFrom-Json
    if ($manifest.product -cne 'DeskSend' -or $manifest.format -ne 1 -or !$manifest.files) { throw '更新包清单无效。' }
    $files = @($manifest.files.PSObject.Properties)
    foreach ($required in @('DeskSend.exe', 'deskflow-core.exe', 'localsend-cli.exe')) {
        if ($required -cnotin $files.Name) { throw ('完整包缺少 ' + $required) }
    }
    # 清单中的每个程序文件都必须通过校验，配置目录始终由本机保留。
    foreach ($file in $files) {
        $relative = $file.Name.Replace('/', '\')
        if ($relative -match '^(settings|\.desksend-backups)(\\|$)' -or $file.Value -notmatch '^[a-fA-F0-9]{64}$') { throw '更新清单包含受保护目录或无效校验值。' }
        $source = Get-ChildPath $packageRoot $relative
        if ((Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash -ine $file.Value) { throw ('文件校验失败：' + $relative) }
        # 禁止通过已有目录链接把程序文件写到安装目录以外。
        $target = Get-ChildPath $installRoot $relative
        $parent = [IO.Path]::GetDirectoryName($target)
        while ($parent.Length -ge $installRoot.Length) {
            if ((Test-Path -LiteralPath $parent) -and ((Get-Item -LiteralPath $parent).Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw '安装目录包含目录链接，无法自动更新。' }
            $parent = [IO.Path]::GetDirectoryName($parent)
        }
        if ((Test-Path -LiteralPath $target) -and ((Get-Item -LiteralPath $target).Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw '安装文件包含链接。' }
    }
    if ($GuiProcessId -gt 0) {
        $gui = Get-Process -Id $GuiProcessId -ErrorAction SilentlyContinue
        if ($gui -and !$gui.WaitForExit(30000)) { throw 'DeskSend 未退出，更新已取消。' }
    }
    # 只结束当前安装目录的后台组件，不影响其他目录中的程序。
    if (!$NoRestart) {
        $cimPaths = @{}
        foreach ($cim in @(Get-CimInstance -ClassName Win32_Process -Filter "Name='deskflow-core.exe' OR Name='localsend-cli.exe'" -ErrorAction SilentlyContinue)) {
            if ($cim.ExecutablePath) { $cimPaths[[int]$cim.ProcessId] = $cim.ExecutablePath }
        }
        foreach ($process in @(Get-Process -Name 'deskflow-core','localsend-cli' -ErrorAction SilentlyContinue)) {
            $procPath = $null
            try { $procPath = $process.Path } catch { $procPath = $null }
            if (!$procPath) { $procPath = $cimPaths[$process.Id] }
            if (!$procPath) {
                throw ('无法检查后台进程路径，请以管理员身份运行或手动退出后台组件后重试：' + $process.ProcessName + ' (' + $process.Id + ')')
            }
            if ([IO.Path]::GetDirectoryName($procPath).TrimEnd('\') -ieq $installRoot.TrimEnd('\')) {
                Stop-Process -Id $process.Id -Force -ErrorAction Stop
                if (!$process.WaitForExit(10000)) {
                    throw ('后台进程未能在超时内退出：' + $process.ProcessName + ' (' + $process.Id + ')')
                }
            }
        }
    }
    $stopped = $true
    $backupRoot = Join-Path $installRoot ('.desksend-backups\' + [guid]::NewGuid().ToString('N'))
    $null = New-Item -ItemType Directory -Path $backupRoot
    # 先备份所有待覆盖文件，全部成功后才开始覆盖。
    foreach ($relative in @($files.Name) + @('desksend-build.json')) {
        $target = Get-ChildPath $installRoot $relative
        $backup = Get-ChildPath $backupRoot $relative
        $existed = Test-Path -LiteralPath $target -PathType Leaf
        if ($existed) {
            $null = New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($backup)) -Force
            Copy-Item -LiteralPath $target -Destination $backup
        }
        $changed.Add([pscustomobject]@{ Relative=$relative; Target=$target; Backup=$backup; Existed=$existed })
    }
    $copyStarted = $true
    foreach ($file in $changed) {
        $null = New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($file.Target)) -Force
        Copy-Item -LiteralPath (Get-ChildPath $packageRoot $file.Relative) -Destination $file.Target -Force
    }
    Write-UpdateLog ('SUCCESS: ' + $manifest.version + '; backup=' + $backupRoot)
    Start-DeskSend
    Write-Output ('UPDATE_SUCCESS: ' + $logPath)
    exit 0
} catch {
    $failure = $_.Exception.Message
    Write-UpdateLog ('FAILED: ' + $failure)
    $rollbackErrors = @()
    if ($copyStarted) {
        foreach ($file in $changed) {
            try {
                if ($file.Existed) { Copy-Item -LiteralPath $file.Backup -Destination $file.Target -Force }
                elseif (Test-Path -LiteralPath $file.Target -PathType Leaf) { Remove-Item -LiteralPath $file.Target }
            } catch { $rollbackErrors += $_.Exception.Message }
        }
    }
    if ($rollbackErrors.Count) {
        Write-UpdateLog ('ROLLBACK_FAILED: ' + ($rollbackErrors -join '; '))
        $failure += ' 自动恢复未完全成功，请使用备份目录恢复。'
    } elseif (!$NoRestart) {
        # 即使验证阶段失败，也等待已请求退出的界面结束后恢复旧版。
        $canRestart = $stopped
        if (!$canRestart -and $GuiProcessId -gt 0) {
            $previousGui = Get-Process -Id $GuiProcessId -ErrorAction SilentlyContinue
            $canRestart = !$previousGui -or $previousGui.WaitForExit(30000)
        }
        if ($canRestart) { Start-DeskSend }
    }
    if (!$NoRestart) {
        Add-Type -AssemblyName System.Windows.Forms
        $null = [Windows.Forms.MessageBox]::Show(($failure + "`r`n日志：" + $logPath), 'DeskSend 更新失败')
    }
    Write-Output ('UPDATE_FAILED: ' + $failure + '; log=' + $logPath)
    exit 1
}
