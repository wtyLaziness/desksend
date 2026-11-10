param(
  [string]$DeskflowDir,
  [string]$LocalSendCliExe = "",
  [string]$OutDir,
  [string]$DisableLock = 'false'
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
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr hWnd, uint Msg, IntPtr wParam, IntPtr lParam);
  public struct POINT { public int X; public int Y; }

  public const uint MOUSEEVENTF_MOVE = 0x0001;
  public const uint MOUSEEVENTF_LEFTDOWN = 0x0002;
  public const uint MOUSEEVENTF_LEFTUP = 0x0004;
  public const uint MOUSEEVENTF_ABSOLUTE = 0x8000;
  public const uint WM_CLOSE = 0x0010;
  public static readonly UIntPtr MAGIC_EXTRA = new UIntPtr(0x44534B53);

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
        Thread.Sleep(200);
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

        // 3. Move slightly on the client screen using relative deltas, then release left mouse button
        for (int i = 0; i < 6; i++) {
          MoveRel(12, 4);
          Thread.Sleep(25);
        }
        Thread.Sleep(200);
        GetCursorPos(out p);
        System.IO.File.AppendAllText(logPath, "[Thread] Releasing LEFTUP on client, cursor=(" + p.X + "," + p.Y + ")\r\n");
        mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, MAGIC_EXTRA);
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

$rxCfg = "$script:out\rx_cfg"
$txCfg = "$script:out\tx_cfg"
$dropDest = "$script:out\drop_dest"
New-Item -ItemType Directory -Force $rxCfg, $txCfg, $dropDest | Out-Null

$env:DESKSEND_RECEIVER_PORT = "53318"
$env:DESKSEND_TARGET_PORT = "53318"
$env:DESKSEND_SENDER_PORT = "53319"
$env:DESKSEND_RECEIVER_CONFIG_HOME = $rxCfg
$env:DESKSEND_SENDER_CONFIG_HOME = $txCfg
$env:DESKSEND_DEFAULT_DROP_DIR = $dropDest

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
$srv = Start-Process $core -ArgumentList 'server','--new-instance','-s',"$script:out\server.conf" -PassThru -WindowStyle Hidden -RedirectStandardOutput "$script:out\server.stdout" -RedirectStandardError "$script:out\server.stderr"
Start-Sleep 3
$cli = Start-Process $core -ArgumentList 'client','--new-instance','-s',"$script:out\client.conf" -PassThru -WindowStyle Hidden -RedirectStandardOutput "$script:out\client.stdout" -RedirectStandardError "$script:out\client.stderr"

# Wait for client connection and headless LocalSend receiver readiness
for ($i = 0; $i -lt 30; $i++) {
  if ((Test-Path "$script:out\client.log") -and ((Get-Content "$script:out\client.log" -Raw) -match "LOCALSEND_RECEIVER_READY")) {
    break
  }
  Start-Sleep -Milliseconds 500
}

$sw = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds.Width
$sh = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds.Height
Log "server alive=$(-not $srv.HasExited) client alive=$(-not $cli.HasExited) DisableLock=$DisableLock screen=${sw}x${sh}"

$tmp = Join-Path $env:TEMP 'dd_test'
Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $tmp | Out-Null
'desksend-payload-1' | Set-Content "$tmp\one.txt" -NoNewline
'desksend-payload-2' | Set-Content "$tmp\two.txt" -NoNewline
$script:files = [string[]]@("$tmp\one.txt","$tmp\two.txt")

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

Log "Waiting up to 15s for transferred files to arrive in $dropDest ..."
$transferred = $false
for ($i = 0; $i -lt 30; $i++) {
  if ((Test-Path "$dropDest\one.txt") -and (Test-Path "$dropDest\two.txt")) {
    $transferred = $true
    break
  }
  Start-Sleep -Milliseconds 500
}

Stop-Process $cli,$srv -Force -ErrorAction SilentlyContinue
Get-Process localsend-cli -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep 1

foreach ($n in 'test.log','server.log','client.log','server.stderr','client.stderr') {
  Write-Host "=============== $n ==============="
  if (Test-Path "$script:out\$n") {
    Get-Content "$script:out\$n" -Encoding UTF8 | Select-Object -Last 80
  }
}

Write-Host "=============== Dropped Files in $dropDest ==============="
Get-ChildItem $dropDest -ErrorAction SilentlyContinue | ForEach-Object {
  Write-Host ("{0} ({1} bytes): {2}" -f $_.Name, $_.Length, (Get-Content $_.FullName -Raw))
}

if (-not $transferred) {
  throw "E2E DeskSend file transfer FAILED: files did not arrive in $dropDest"
}
$c1 = Get-Content "$dropDest\one.txt" -Raw
$c2 = Get-Content "$dropDest\two.txt" -Raw
if ($c1 -ne 'desksend-payload-1' -or $c2 -ne 'desksend-payload-2') {
  throw "Transferred file content mismatch: c1='$c1' c2='$c2'"
}
Log "E2E DeskSend cross-screen drag-and-drop file transfer PASSED!"
