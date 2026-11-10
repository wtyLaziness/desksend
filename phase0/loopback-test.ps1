param(
  [string]$DeskflowDir,
  [string]$SentinelExe,
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
  public struct POINT { public int X; public int Y; }

  public const uint MOUSEEVENTF_MOVE = 0x0001;
  public const uint MOUSEEVENTF_LEFTDOWN = 0x0002;
  public const uint MOUSEEVENTF_LEFTUP = 0x0004;
  public const uint MOUSEEVENTF_ABSOLUTE = 0x8000;

  public static void MoveAbs(int x, int y, int sw, int sh) {
    uint dx = (uint)((x * 65535) / (sw - 1));
    uint dy = (uint)((y * 65535) / (sh - 1));
    SetCursorPos(x, y);
    mouse_event(MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE, dx, dy, 0, UIntPtr.Zero);
  }

  public static void StartDragThread(int startX, int startY, int sw, int sh, string logPath) {
    Thread t = new Thread(() => {
      try {
        Thread.Sleep(500);
        MoveAbs(startX, startY, sw, sh);
        Thread.Sleep(200);
        mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, UIntPtr.Zero);
        System.IO.File.AppendAllText(logPath, "[Thread] LEFTDOWN at (" + startX + "," + startY + ")\r\n");
        Thread.Sleep(100);
        for (int x = startX; x <= sw - 1; x += 8) {
          MoveAbs(x, startY, sw, sh);
          Thread.Sleep(15);
        }
        // 在最右边缘停留 500ms
        for (int i = 0; i < 20; i++) {
          MoveAbs(sw - 1, startY, sw, sh);
          Thread.Sleep(25);
        }
        POINT p;
        GetCursorPos(out p);
        System.IO.File.AppendAllText(logPath, "[Thread] At right edge cursor=(" + p.X + "," + p.Y + "), releasing LEFTUP\r\n");
        mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, UIntPtr.Zero);
      } catch (Exception ex) {
        System.IO.File.AppendAllText(logPath, "[Thread] Error: " + ex.Message + "\r\n");
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

@"
section: links
	host:
		right = client
	client:
		left = host
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
Start-Sleep 5
Log "server alive=$(-not $srv.HasExited) client alive=$(-not $cli.HasExited) DisableLock=$DisableLock"

$sentry = Start-Process $SentinelExe -ArgumentList 'right','8' -WorkingDirectory $script:out -PassThru -WindowStyle Hidden
Start-Sleep 2
$sw = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds.Width
$sh = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds.Height
Log "screen=${sw}x${sh} sentinel alive=$(-not $sentry.HasExited)"

$tmp = Join-Path $env:TEMP 'dd_test'
New-Item -ItemType Directory -Force $tmp | Out-Null
'hello1' | Set-Content "$tmp\one.txt"
'hello2' | Set-Content "$tmp\two.txt"
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
  $script:f.BeginInvoke([Action]{ $script:f.Close() }) | Out-Null
})

$script:safety = New-Object System.Windows.Forms.Timer
$script:safety.Interval = 15000
$script:safety.Add_Tick({
  Log "Safety timeout reached"
  [MouseDriver]::mouse_event([MouseDriver]::MOUSEEVENTF_LEFTUP, 0, 0, 0, [UIntPtr]::Zero)
  $script:f.Close()
})

$script:f.Add_Shown({
  $script:safety.Start()
  [MouseDriver]::StartDragThread(200, 200, $sw, $sh, $testLog)
})

[System.Windows.Forms.Application]::Run($script:f)

Start-Sleep 2
Stop-Process $sentry,$cli,$srv -Force -ErrorAction SilentlyContinue
Start-Sleep 1

foreach ($n in 'test.log','sentinel.log','server.log','client.log','server.stderr','client.stderr') {
  Write-Host "=============== $n ==============="
  if (Test-Path "$script:out\$n") {
    Get-Content "$script:out\$n" -Encoding UTF8 | Select-Object -Last 60
  }
}
