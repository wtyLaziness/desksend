param(
  [string]$DeskflowDir,
  [string]$LocalSendCliExe = "",
  [string]$OutDir,
  [string]$DisableLock = 'false',
  [switch]$CancelDrag,
  [switch]$ReturnBeforeRelease
)
$ErrorActionPreference = 'Continue'
New-Item -ItemType Directory -Force $OutDir | Out-Null
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
Add-Type @"
using System;
using System.Threading;
using System.Runtime.InteropServices;
public class MouseDriver {
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint dx, uint dy, uint d, UIntPtr e);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p);
  [DllImport("user32.dll")] public static extern short GetAsyncKeyState(int key);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr hWnd, uint Msg, IntPtr wParam, IntPtr lParam);
  [DllImport("user32.dll")] public static extern void keybd_event(byte key, byte scan, uint flags, UIntPtr extra);
  public struct POINT { public int X; public int Y; }

  public const uint MOUSEEVENTF_MOVE = 0x0001;
  public const uint MOUSEEVENTF_LEFTDOWN = 0x0002;
  public const uint MOUSEEVENTF_LEFTUP = 0x0004;
  public const uint MOUSEEVENTF_ABSOLUTE = 0x8000;
  public const uint WM_CLOSE = 0x0010;
  public static readonly UIntPtr MAGIC_EXTRA = new UIntPtr(0x44534B53);
  public static string ProxyLog;
  public static volatile bool NativeDragEnded;
  public static bool CancelDrag;
  public static bool ReturnBeforeRelease;
  public static string ReadShared(string path) {
    using (var stream = new System.IO.FileStream(path, System.IO.FileMode.Open, System.IO.FileAccess.Read, System.IO.FileShare.ReadWrite))
    using (var reader = new System.IO.StreamReader(stream)) return reader.ReadToEnd();
  }

  public static void MoveAbs(int x, int y, int sw, int sh) {
    uint dx = (uint)((x * 65535) / (sw - 1));
    uint dy = (uint)((y * 65535) / (sh - 1));
    SetCursorPos(x, y);
    mouse_event(MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE, dx, dy, 0, MAGIC_EXTRA);
  }

  public static void MoveRel(int dx, int dy) {
    mouse_event(MOUSEEVENTF_MOVE, unchecked((uint)dx), unchecked((uint)dy), 0, MAGIC_EXTRA);
  }

  public static void StartDragThread(int startX, int startY, int sw, int sh, string logPath, IntPtr formHwnd) {
    Thread t = new Thread(() => {
      try {
        Thread.Sleep(600);
        MoveAbs(startX, startY, sw, sh);
        Thread.Sleep(500);
        mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, MAGIC_EXTRA);
        System.IO.File.AppendAllText(logPath, "[Thread] LEFTDOWN at (" + startX + "," + startY + ")\r\n");
        Thread.Sleep(150);

        // 1. Move across primary screen into the 36px activation zone and into the 8px sentinel bar (sw - 4)
        for (int x = startX; x <= sw - 4; x += 8) {
          MoveAbs(x, startY, sw, sh);
          Thread.Sleep(15);
        }

        // Pause inside the 8px sentinel bar [sw-8..sw-2] so OLE dispatches IDropTarget::DragEnter
        MoveAbs(sw - 4, startY, sw, sh);
        Thread.Sleep(200);

        // 2. Step into the 1px jump zone (sw - 1) until Server switches and warps cursor to screen center
        POINT p;
        for (int i = 0; i < 20; i++) {
          GetCursorPos(out p);
          if (p.X < sw - 100) {
            System.IO.File.AppendAllText(logPath, "[Thread] Detected screen switch warp to center (" + p.X + "," + p.Y + ") at iter " + i + "\r\n");
            break;
          }
          MoveAbs(sw - 1, startY, sw, sh);
          Thread.Sleep(30);
        }

        // 3. Move slightly on the client screen relative to server center (sw/2, sh/2), then release left mouse button
        for (int i = 0; i < 6; i++) {
          MoveAbs(sw / 2 + 25, sh / 2 + 10, sw, sh);
          Thread.Sleep(30);
        }
        Thread.Sleep(200);
        if (!NativeDragEnded) {
          throw new Exception("Native drag did not cancel before remote button release");
        }
        System.IO.File.AppendAllText(logPath, "[Thread] Native drag ended before remote button release\r\n");
        if (CancelDrag) {
          keybd_event(0x1B, 0, 0, MAGIC_EXTRA);
          Thread.Sleep(80);
          keybd_event(0x1B, 0, 2, MAGIC_EXTRA);
          Thread.Sleep(200);
        }
        if (ReturnBeforeRelease) {
          for (int i = 0; i < 30; i++) {
            MoveRel(-100, 0);
            Thread.Sleep(20);
          }
        }
        GetCursorPos(out p);
        System.IO.File.AppendAllText(logPath, "[Thread] Releasing LEFTUP on client, cursor=(" + p.X + "," + p.Y + ")\r\n");
        mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, MAGIC_EXTRA);
        Thread.Sleep(150);
        if ((GetAsyncKeyState(1) & 0x8000) != 0) {
          throw new Exception("Source OS still considers left mouse button pressed after remote release");
        }
        // 等待真正开始上传，再回切；不点击鼠标，避免掩盖按键状态残留。
        for (int i = 0; !CancelDrag && !ReturnBeforeRelease && i < 100; i++) {
          if (System.IO.File.Exists(ProxyLog) && ReadShared(ProxyLog).Contains("PROXY_UPLOAD_STARTED")) break;
          Thread.Sleep(100);
        }
        System.IO.File.AppendAllText(logPath, "[Thread] Returning to primary during upload\r\n");
        for (int i = 0; i < 30; i++) {
          MoveRel(-100, 0);
          Thread.Sleep(20);
        }
        Thread.Sleep(500);
      } catch (Exception ex) {
        System.IO.File.AppendAllText(logPath, "[Thread] Error: " + ex.Message + "\r\n");
      } finally {
        if (formHwnd != IntPtr.Zero) {
          PostMessage(formHwnd, WM_CLOSE, IntPtr.Zero, IntPtr.Zero);
        }
      }
    });
    t.IsBackground = true;
    t.Start();
  }
}
"@
[MouseDriver]::SetProcessDPIAware() | Out-Null
[MouseDriver]::CancelDrag = $CancelDrag.IsPresent
[MouseDriver]::ReturnBeforeRelease = $ReturnBeforeRelease.IsPresent

$script:out = (Resolve-Path $OutDir).Path
$testLog = "$script:out\test.log"
function Log($m) {
  $l = "[{0:HH:mm:ss.fff}] {1}" -f (Get-Date), $m
  Write-Host $l
  Add-Content $testLog $l
}

if ($LocalSendCliExe -and (Test-Path $LocalSendCliExe)) {
  Copy-Item $LocalSendCliExe (Join-Path $DeskflowDir 'localsend-cli.exe') -Force
  Log "Copied localsend-cli.exe into $DeskflowDir"
}

$dropDest = "$script:out\drop_dest"
New-Item -ItemType Directory -Force $dropDest | Out-Null

# Only override the drop destination folder for deterministic verification;
# ports (53318/53319) and isolated config directories use DeskSend's zero-config defaults.
$env:DESKSEND_DEFAULT_DROP_DIR = $dropDest
$env:DESKSEND_TARGET_PORT = '53320'
$proxyLog = "$script:out\proxy.log"
[MouseDriver]::ProxyLog = $proxyLog
$proxy = Start-Process python -ArgumentList '-u',"$PSScriptRoot\slow-proxy.py" -PassThru -WindowStyle Hidden -RedirectStandardOutput $proxyLog -RedirectStandardError "$script:out\proxy.err"
for ($i = 0; $i -lt 30; $i++) {
  if ((Test-Path $proxyLog) -and ((Get-Content $proxyLog -Raw) -match 'PROXY_READY')) { break }
  Start-Sleep -Milliseconds 100
}

@"
section: links
	host:
		right = client
	client:
		left = host
end
section: options
end
"@ | Set-Content "$script:out\layout.cfg" -Encoding ASCII

$layout = ("$script:out\layout.cfg") -replace '\\','/'
@"
[core]
computerName=host
port=24800
coreMode=2
interface=127.0.0.1
[security]
tlsEnabled=false
checkPeerFingerprints=false
[server]
externalConfig=true
externalConfigFile=$layout
disableLockToComputer=$DisableLock
enableSwitchDelay=false
[computer_host]
name=host
[computer_client]
name=client
[log]
level=DEBUG
toFile=true
file=$(("$script:out\server.log") -replace '\\','/')
"@ | Set-Content "$script:out\server.conf" -Encoding ASCII

@"
[core]
computerName=client
port=24800
coreMode=1
[client]
remoteHost=127.0.0.1
[security]
tlsEnabled=false
checkPeerFingerprints=false
[log]
level=DEBUG
toFile=true
file=$(("$script:out\client.log") -replace '\\','/')
"@ | Set-Content "$script:out\client.conf" -Encoding ASCII

$core = Join-Path $DeskflowDir 'deskflow-core.exe'
$cli = Start-Process $core -ArgumentList 'client','--new-instance','-s',"$script:out\client.conf" -PassThru -WindowStyle Hidden -RedirectStandardOutput "$script:out\client.stdout" -RedirectStandardError "$script:out\client.stderr"
Start-Sleep 3
if ($cli.HasExited) { throw 'Client exited while waiting for the server' }
$srv = Start-Process $core -ArgumentList 'server','--new-instance','-s',"$script:out\server.conf" -PassThru -WindowStyle Hidden -RedirectStandardOutput "$script:out\server.stdout" -RedirectStandardError "$script:out\server.stderr"

# Wait for client connection and headless LocalSend receiver readiness
for ($i = 0; $i -lt 30; $i++) {
  if ((Test-Path "$script:out\client.log") -and ((Get-Content "$script:out\client.log" -Raw) -match "LOCALSEND_RECEIVER_READY")) {
    break
  }
  Start-Sleep -Milliseconds 500
}

$sw = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds.Width
if (!(Test-Path "$script:out\client.log") -or !((Get-Content "$script:out\client.log" -Raw) -match 'LOCALSEND_RECEIVER_READY')) {
  throw 'Receiver did not become ready after starting the server late'
}
$receiverIds = @(Get-Process localsend-cli -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Id)
Stop-Process $srv -Force
Start-Sleep 3
if ($cli.HasExited) { throw 'Client exited after the server stopped' }
$srv = Start-Process $core -ArgumentList 'server','--new-instance','-s',"$script:out\server.conf" -PassThru -WindowStyle Hidden -RedirectStandardOutput "$script:out\server-restart.stdout" -RedirectStandardError "$script:out\server-restart.stderr"
$reconnected = $false
for ($i = 0; $i -lt 30; $i++) {
  if (@(Select-String -Path "$script:out\client.log" -Pattern 'connected to server' -CaseSensitive | Where-Object { $_.Line -notmatch 'disconnected' }).Count -ge 2) {
    $reconnected = $true
    break
  }
  Start-Sleep -Milliseconds 500
}
if (!$reconnected) { throw 'Client did not reconnect automatically after server restart' }
$afterIds = @(Get-Process localsend-cli -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Id)
if (@(Compare-Object $receiverIds $afterIds).Count -ne 0) { throw 'Receiver was duplicated or restarted on reconnect' }
Log 'Client waited for late server, reconnected after restart, and reused the receiver'
$sh = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds.Height
Log "server alive=$(-not $srv.HasExited) client alive=$(-not $cli.HasExited) DisableLock=$DisableLock screen=${sw}x${sh}"

$tmp = Join-Path $env:TEMP 'dd_test'
Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force "$tmp\TestFolder\sub" | Out-Null
'desksend-payload-1' | Set-Content "$tmp\one.txt" -NoNewline
'desksend-payload-2' | Set-Content "$tmp\two.txt" -NoNewline
'desksend-nested-folder-payload' | Set-Content "$tmp\TestFolder\sub\nested.txt" -NoNewline
[IO.File]::WriteAllBytes("$tmp\large.bin", [byte[]]::new(32MB))
$script:files = [string[]]@("$tmp\one.txt","$tmp\two.txt","$tmp\TestFolder","$tmp\large.bin")

$script:f = New-Object System.Windows.Forms.Form
$script:f.StartPosition = 'Manual'
$script:f.Location = New-Object System.Drawing.Point(100, 100)
$script:f.Size = New-Object System.Drawing.Size(300, 200)
$script:f.TopMost = $true
$script:lbl = New-Object System.Windows.Forms.Label
$script:lbl.Dock = 'Fill'
$script:lbl.Text = 'drag source'
$script:f.Controls.Add($script:lbl)

$script:lbl.Add_MouseDown({
  Log "MouseDown received -> calling DoDragDrop"
  $d = New-Object System.Windows.Forms.DataObject
  $d.SetData([System.Windows.Forms.DataFormats]::FileDrop, $script:files)
  $r = $script:lbl.DoDragDrop($d, [System.Windows.Forms.DragDropEffects]::Copy -bor [System.Windows.Forms.DragDropEffects]::Move)
  [MouseDriver]::NativeDragEnded = ($r -eq [System.Windows.Forms.DragDropEffects]::None)
  Log "DoDragDrop returned: $r"
})

$script:safety = New-Object System.Windows.Forms.Timer
$script:safety.Interval = 20000
$script:safety.Add_Tick({
  Log "Safety timeout reached"
  [MouseDriver]::mouse_event([MouseDriver]::MOUSEEVENTF_LEFTUP, 0, 0, 0, [MouseDriver]::MAGIC_EXTRA)
  $script:f.Close()
})

$script:f.Add_Shown({
  $script:safety.Start()
  [MouseDriver]::StartDragThread(200, 200, $sw, $sh, $testLog, $script:f.Handle)
})

[System.Windows.Forms.Application]::Run($script:f)

Log "Waiting up to 90s for throttled transfer to arrive in $dropDest ..."
$transferred = $false
for ($i = 0; !$CancelDrag -and !$ReturnBeforeRelease -and $i -lt 180; $i++) {
  if ((Test-Path "$dropDest\one.txt") -and (Test-Path "$dropDest\two.txt") -and (Test-Path "$dropDest\TestFolder\sub\nested.txt") -and (Test-Path "$dropDest\large.bin")) {
    $transferred = $true
    break
  }
  Start-Sleep -Milliseconds 500
}

# CI 不具备本机截图技能脚本，使用 Windows 截图 API 保存窗口验证证据。
if ($transferred) {
  Start-Sleep -Milliseconds 500
  $bounds = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
  $bitmap = [System.Drawing.Bitmap]::new($bounds.Width, $bounds.Height)
  $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
  try {
    $graphics.CopyFromScreen($bounds.Location, [System.Drawing.Point]::Empty, $bounds.Size)
    $capture = Join-Path $env:TEMP 'desksend-transfer-status.png'
    $bitmap.Save($capture, [System.Drawing.Imaging.ImageFormat]::Png)
    Copy-Item $capture "$script:out\transfer-status.png" -Force
  } finally {
    $graphics.Dispose()
    $bitmap.Dispose()
  }
}
Stop-Process $cli,$srv -Force -ErrorAction SilentlyContinue
Stop-Process $proxy -Force -ErrorAction SilentlyContinue
Get-Process localsend-cli -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep 1

foreach ($n in 'test.log','server.log','client.log','server.stderr','client.stderr') {
  Write-Host "=============== $n ==============="
  if (Test-Path "$script:out\$n") {
    Get-Content "$script:out\$n" -Encoding UTF8 | Select-Object -Last 80
  }
}

Write-Host "=============== Dropped Files in $dropDest ==============="
Get-ChildItem $dropDest -Recurse -File -ErrorAction SilentlyContinue | ForEach-Object {
  Write-Host ("{0} ({1} bytes)" -f $_.FullName, $_.Length)
}

if (-not $transferred) {
  if ($CancelDrag -or $ReturnBeforeRelease) {
    $serverText = Get-Content "$script:out\server.log" -Raw
    if ($serverText -match 'spawning .*localsend-cli|queued send job' -or @(Get-ChildItem $dropDest -Recurse -File).Count -gt 0) {
      throw 'Cancelled drag unexpectedly sent files'
    }
    $cancelMessage = if ($CancelDrag) {'Escape pressed; cancelling file drag'} else {'cursor returned to primary before release; cancelling file drag'}
    if ($serverText -notmatch $cancelMessage -or $serverText -notmatch 'switch from "client" to "host"') {
      throw 'Cancelled drag did not cancel and return to primary'
    }
    if (!((Get-Content $testLog -Raw) -match 'Native drag ended before remote button release')) {
      throw 'Native source drag did not end before release'
    }
    Log 'Remote drag cancelled without sending files; cursor returned to primary'
    exit 0
  }
  throw "E2E DeskSend file/folder transfer FAILED: items did not arrive in $dropDest"
}
$serverText = Get-Content "$script:out\server.log" -Raw
if (@([regex]::Matches($serverText, 'FileDropBridge: spawning ')).Count -ne 1 -or $serverText -match 'queued send job') {
  throw 'One drag produced duplicate send jobs'
}
if (!((Get-Content $testLog -Raw) -match 'Native drag ended before remote button release')) {
  throw 'Native source drag did not end before release'
}
if (!(Test-Path "$tmp\large.bin") -or !(Test-Path "$tmp\one.txt") -or !(Test-Path "$tmp\TestFolder\sub\nested.txt")) {
  throw 'Source files were moved or removed'
}
$returnIndex = $serverText.IndexOf('switch from "client" to "host"')
$successIndex = $serverText.IndexOf('LocalSend transfer to')
if ($returnIndex -lt 0 -or $successIndex -lt 0 -or $returnIndex -ge $successIndex) {
  throw 'Cursor did not return to primary before transfer completed'
}
if ((Get-FileHash "$tmp\large.bin").Hash -ne (Get-FileHash "$dropDest\large.bin").Hash) { throw 'Large file hash mismatch' }
Log 'Returned to primary during upload; transfer continued and large file hash matched'
$c1 = Get-Content "$dropDest\one.txt" -Raw
$c2 = Get-Content "$dropDest\two.txt" -Raw
$c3 = Get-Content "$dropDest\TestFolder\sub\nested.txt" -Raw
if ($c1 -ne 'desksend-payload-1' -or $c2 -ne 'desksend-payload-2' -or $c3 -ne 'desksend-nested-folder-payload') {
  throw "Transferred content mismatch: c1='$c1' c2='$c2' c3='$c3'"
}
Log "E2E DeskSend cross-screen drag-and-drop file & folder transfer PASSED!"
