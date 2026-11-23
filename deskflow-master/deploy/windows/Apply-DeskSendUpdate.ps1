# SPDX-License-Identifier: MIT
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$InstallDir,
    [Parameter(Mandatory)][string]$Archive,
    [Parameter(Mandatory)][ValidatePattern('^[a-fA-F0-9]{64}$')][string]$ExpectedHash,
    [int]$GuiProcessId = 0,
    [switch]$NoRestart,
    [switch]$Msi
)
$ErrorActionPreference = 'Stop'
$installRoot = [IO.Path]::GetFullPath($InstallDir).TrimEnd('\')
$workRoot = Join-Path ([IO.Path]::GetTempPath()) ('DeskSend-apply-' + [guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Path $workRoot
$logPath = Join-Path $workRoot 'update.log'
$changed = [Collections.Generic.List[object]]::new()
$copyStarted = $false
$stopped = $false
$stageRoot = $null

function Write-UpdateLog([string]$Message) {
    Add-Content -LiteralPath $logPath -Value $Message -Encoding UTF8
}
function Get-ChildPath([string]$Root, [string]$Relative) {
    if ([IO.Path]::IsPathRooted($Relative) -or $Relative.Contains(':')) { throw '更新包包含非法路径。' }
    foreach ($segment in ($Relative.TrimEnd('/', '\') -split '[/\\]')) {
        if (!$segment -or $segment.EndsWith('.') -or $segment.EndsWith(' ')) { throw '更新包包含非法路径。' }
    }
    $full = [IO.Path]::GetFullPath((Join-Path $Root $Relative))
    if (!$full.StartsWith($Root.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) { throw '更新路径越界。' }
    return $full
}
function Assert-NoReparsePoint([string]$Path) {
    $parents = [Collections.Generic.Stack[string]]::new()
    for ($current = [IO.Path]::GetFullPath($Path); $current; $current = [IO.Path]::GetDirectoryName($current)) {
        $parents.Push($current)
    }
    while ($parents.Count) {
        $current = $parents.Pop()
        $item = Get-Item -LiteralPath $current -Force -ErrorAction SilentlyContinue
        if ($item -and ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw ('Reparse point in update path: ' + $current)
        }
    }
}

function Assert-UpdateFileTarget([string]$Path) {
    Assert-NoReparsePoint $Path
    if (Test-Path -LiteralPath $Path -PathType Container) {
        throw ('更新需要写入文件，但该位置已有同名目录：' + $Path)
    }
}

function Remove-UpdateDirectory([string]$Path) {
    Assert-NoReparsePoint $Path
    foreach ($item in Get-ChildItem -LiteralPath $Path -Force) {
        Assert-NoReparsePoint $item.FullName
        if ($item.PSIsContainer) {
            Remove-UpdateDirectory $item.FullName
        } else {
            Remove-Item -LiteralPath $item.FullName -Force
        }
    }
    [IO.Directory]::Delete($Path)
}

$archiveParent = [IO.Path]::GetDirectoryName([IO.Path]::GetFullPath($Archive))
$tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')
$ownsDownload = [IO.Path]::GetDirectoryName($archiveParent) -ieq $tempRoot -and
    [IO.Path]::GetFileName($archiveParent) -match '^DeskSend-update-[0-9a-f]{8}(-[0-9a-f]{4}){3}-[0-9a-f]{12}$' -and
    [IO.Path]::GetFileName($Archive) -in @('update.zip', 'update.msi') -and
    $PSCommandPath -ieq (Join-Path $archiveParent 'Apply-DeskSendUpdate.ps1') -and
    !(($installRoot + '\').StartsWith($archiveParent + '\', [StringComparison]::OrdinalIgnoreCase))
function Start-DeskSend {
    if (!$NoRestart) {
        Start-Process -FilePath (Join-Path $installRoot 'DeskSend.exe') -WorkingDirectory $installRoot -WindowStyle Hidden
    }
}

try {
    Assert-NoReparsePoint $installRoot
    Assert-NoReparsePoint $workRoot
    if ($ownsDownload) { Assert-NoReparsePoint $Archive }
    if (!(Test-Path -LiteralPath (Join-Path $installRoot 'DeskSend.exe') -PathType Leaf)) { throw '找不到原有 DeskSend 程序。' }
    if ((Get-FileHash -LiteralPath $Archive -Algorithm SHA256).Hash -ine $ExpectedHash) { throw '更新包 SHA-256 校验失败。' }
    if ($Msi) {
        if ([IO.Path]::GetExtension($Archive) -ine '.msi') { throw 'MSI 更新包扩展名无效。' }
        if ($GuiProcessId -gt 0) {
            $gui = Get-Process -Id $GuiProcessId -ErrorAction SilentlyContinue
            if ($gui -and !$gui.WaitForExit(30000)) { throw 'DeskSend 未退出，更新已取消。' }
        }
        if (!$NoRestart) {
            $cimPaths = @{}
            foreach ($cim in @(Get-CimInstance -ClassName Win32_Process -Filter "Name='deskflow-core.exe' OR Name='deskflow-drop-agent.exe' OR Name='localsend-cli.exe'" -ErrorAction SilentlyContinue)) {
                if ($cim.ExecutablePath) { $cimPaths[[int]$cim.ProcessId] = $cim.ExecutablePath }
            }
            foreach ($process in @(Get-Process -Name 'deskflow-core','deskflow-drop-agent','localsend-cli' -ErrorAction SilentlyContinue)) {
                $procPath = $null
                try { $procPath = $process.Path } catch { $procPath = $null }
                if (!$procPath) { $procPath = $cimPaths[$process.Id] }
                if ($procPath -and ([IO.Path]::GetDirectoryName($procPath).TrimEnd('\') -ieq $installRoot.TrimEnd('\'))) {
                    try {
                        Stop-Process -Id $process.Id -Force -ErrorAction Stop
                        $null = $process.WaitForExit(10000)
                    } catch {
                        Write-UpdateLog ('WARN: 跳过由 MSI 服务控制接管的进程：' + $process.ProcessName + ' (' + $process.Id + ')')
                    }
                }
            }
        }
        $stopped = $true
        $msiLogPath = Join-Path $workRoot 'msi-install.log'
        $msiArgs = "/i `"$Archive`" INSTALL_ROOT=`"$installRoot`" /passive /norestart /l*v `"$msiLogPath`""
        $msiProc = Start-Process -FilePath 'msiexec.exe' -ArgumentList $msiArgs -WindowStyle Normal -Wait -PassThru
        if ($msiProc.ExitCode -ne 0 -and $msiProc.ExitCode -ne 3010) {
            throw ('MSI 更新安装失败（退出码 ' + $msiProc.ExitCode + '），详见 ' + $msiLogPath)
        }
        Write-UpdateLog ('SUCCESS: MSI exit=' + $msiProc.ExitCode + '; msiLog=' + $msiLogPath)
        Start-DeskSend
        Write-Output ('UPDATE_SUCCESS: ' + $logPath)
        exit 0
    }
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
    # 验证即将写入或清理的目标路径，禁止穿越目录链接。
    $newRelatives = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $null = $newRelatives.Add('desksend-build.json')
    foreach ($file in $files) {
        $relative = $file.Name.Replace('/', '\')
        if ($relative -match '^(settings|\.desksend-backups)(\\|$)' -or $file.Value -notmatch '^[a-fA-F0-9]{64}$') { throw '更新清单包含受保护目录或无效校验值。' }
        $null = $newRelatives.Add($relative)
        $source = Get-ChildPath $packageRoot $relative
        if ((Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash -ine $file.Value) { throw ('文件校验失败：' + $relative) }
        # 禁止通过已有目录链接把程序文件写到安装目录以外。
        $target = Get-ChildPath $installRoot $relative
        Assert-UpdateFileTarget $target
    }
    $obsoleteRelatives = [Collections.Generic.List[string]]::new()
    $prevManifestPath = Join-Path $installRoot 'desksend-build.json'
    Assert-UpdateFileTarget $prevManifestPath
    if (Test-Path -LiteralPath $prevManifestPath -PathType Leaf) {
        $prevManifest = $null
        try {
            $prevManifest = Get-Content -LiteralPath $prevManifestPath -Encoding UTF8 -Raw | ConvertFrom-Json
        } catch {
            Write-UpdateLog ('WARN: invalid previous update manifest: ' + $_.Exception.Message)
        }
        if ($prevManifest -and $prevManifest.files) {
            foreach ($oldFile in @($prevManifest.files.PSObject.Properties)) {
                $oldRel = $oldFile.Name.Replace('/', '\')
                if ($oldRel -match '^(settings|\.desksend-backups)(\\|$)' -or $newRelatives.Contains($oldRel)) { continue }
                $oldTarget = Get-ChildPath $installRoot $oldRel
                Assert-NoReparsePoint $oldTarget
                if (Test-Path -LiteralPath $oldTarget -PathType Leaf) {
                    $obsoleteRelatives.Add($oldRel)
                }
            }
        }
    }
    if ($GuiProcessId -gt 0) {
        $gui = Get-Process -Id $GuiProcessId -ErrorAction SilentlyContinue
        if ($gui -and !$gui.WaitForExit(30000)) { throw 'DeskSend 未退出，更新已取消。' }
    }
    # 只结束当前安装目录的后台组件，不影响其他目录中的程序。
    if (!$NoRestart) {
        $cimPaths = @{}
        foreach ($cim in @(Get-CimInstance -ClassName Win32_Process -Filter "Name='deskflow-core.exe' OR Name='deskflow-drop-agent.exe' OR Name='localsend-cli.exe'" -ErrorAction SilentlyContinue)) {
            if ($cim.ExecutablePath) { $cimPaths[[int]$cim.ProcessId] = $cim.ExecutablePath }
        }
        foreach ($process in @(Get-Process -Name 'deskflow-core','deskflow-drop-agent','localsend-cli' -ErrorAction SilentlyContinue)) {
            $procPath = $null
            try { $procPath = $process.Path } catch { $procPath = $null }
            if (!$procPath) { $procPath = $cimPaths[$process.Id] }
            if (!$procPath) {
                throw ('无法检查后台进程路径，请以管理员身份运行或手动退出后台组件后重试：' + $process.ProcessName + ' (' + $process.Id + ')')
            }
            if ([IO.Path]::GetDirectoryName($procPath).TrimEnd('\') -ieq $installRoot.TrimEnd('\')) {
                try {
                    Stop-Process -Id $process.Id -Force -ErrorAction Stop
                } catch [System.ComponentModel.Win32Exception], [UnauthorizedAccessException] {
                    throw ('无法结束高权限后台进程，请以管理员身份运行或手动退出后台组件后重试：' + $process.ProcessName + ' (' + $process.Id + ')')
                }
                if (!$process.WaitForExit(10000)) {
                    throw ('后台进程未能在超时内退出：' + $process.ProcessName + ' (' + $process.Id + ')')
                }
            }
        }
    }
    $stopped = $true
    $backupRoot = Join-Path $installRoot ('.desksend-backups\' + [guid]::NewGuid().ToString('N'))
    Assert-NoReparsePoint $backupRoot
    $null = New-Item -ItemType Directory -Path $backupRoot
    # 先备份所有待覆盖与待清理的旧版文件，全部成功后才开始写入。
    foreach ($relative in @($files.Name) + @('desksend-build.json')) {
        $target = Get-ChildPath $installRoot $relative
        $backup = Get-ChildPath $backupRoot $relative
        Assert-NoReparsePoint $target
        Assert-NoReparsePoint $backup
        $existed = Test-Path -LiteralPath $target -PathType Leaf
        if ($existed) {
            $null = New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($backup)) -Force
            Copy-Item -LiteralPath $target -Destination $backup
        }
        $changed.Add([pscustomobject]@{ Action='Write'; Relative=$relative; Target=$target; Backup=$backup; Existed=$existed })
    }
    foreach ($relative in $obsoleteRelatives) {
        $target = Get-ChildPath $installRoot $relative
        $backup = Get-ChildPath $backupRoot $relative
        Assert-NoReparsePoint $target
        Assert-NoReparsePoint $backup
        $null = New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($backup)) -Force
        Copy-Item -LiteralPath $target -Destination $backup
        $changed.Add([pscustomobject]@{ Action='Delete'; Relative=$relative; Target=$target; Backup=$backup; Existed=$true })
    }
    $copyStarted = $true
    foreach ($file in $changed) {
        if ($file.Action -eq 'Write') {
            Assert-UpdateFileTarget $file.Target
            $null = New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($file.Target)) -Force
            Copy-Item -LiteralPath (Get-ChildPath $packageRoot $file.Relative) -Destination $file.Target -Force
        } elseif ($file.Action -eq 'Delete') {
            Assert-NoReparsePoint $file.Target
            if (Test-Path -LiteralPath $file.Target -PathType Leaf) {
                Remove-Item -LiteralPath $file.Target -Force
            }
        }
    }
    # 仅保留最近 2 次更新备份，避免长期积累占用磁盘空间。
    $backupsParent = Join-Path $installRoot '.desksend-backups'
    if (Test-Path -LiteralPath $backupsParent -PathType Container) {
        Get-ChildItem -LiteralPath $backupsParent -Directory -ErrorAction SilentlyContinue |
            Sort-Object LastWriteTimeUtc, CreationTimeUtc, Name -Descending |
            Select-Object -Skip 2 |
            ForEach-Object {
                try { Remove-UpdateDirectory $_.FullName }
                catch { Write-UpdateLog ('WARN: backup cleanup skipped: ' + $_.Exception.Message) }
            }
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
                Assert-NoReparsePoint $file.Target
                Assert-NoReparsePoint $file.Backup
                if ($file.Existed) {
                    Assert-UpdateFileTarget $file.Target
                    $null = New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($file.Target)) -Force
                    Copy-Item -LiteralPath $file.Backup -Destination $file.Target -Force
                }
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
} finally {
    foreach ($path in @($stageRoot, $(if ($ownsDownload) { $archiveParent }))) {
        if ($path) {
            try { Remove-UpdateDirectory $path }
            catch { Write-UpdateLog ('WARN: temporary cleanup skipped: ' + $_.Exception.Message) }
        }
    }
}
