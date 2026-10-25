<#
.SYNOPSIS
  End-to-end checks against the real FlashLauncher.exe: command line, launching
  with exact arguments, the built-in relay, and the window driven through UI
  Automation (as a screen reader would). Uses a fake player (launcher_tests.exe
  --fake-player) and a fake game folder in %TEMP%; never touches real games.
#>
param(
  [Parameter(Mandatory)] [string]$Launcher,
  [Parameter(Mandatory)] [string]$Root
)
$ErrorActionPreference = 'Stop'
$failures = 0
function Check([bool]$ok, [string]$what) {
  if ($ok) { Write-Host "[ OK ] $what" } else { Write-Host "[FAIL] $what"; $script:failures++ }
}
function Run([string[]]$argList) {
  $p = Start-Process -FilePath $Launcher -ArgumentList $argList -Wait -PassThru -NoNewWindow `
         -RedirectStandardOutput "$work\out.txt" -RedirectStandardError "$work\err.txt"
  return @{ Code = $p.ExitCode; Out = (Get-Content "$work\out.txt" -Raw) + (Get-Content "$work\err.txt" -Raw) }
}
function PortOpen([int]$port) {
  $c = New-Object System.Net.Sockets.TcpClient
  try { $c.Connect('127.0.0.1', $port); return $true } catch { return $false } finally { $c.Close() }
}
function WaitFor([scriptblock]$cond, [int]$ms = 5000) {
  $sw = [Diagnostics.Stopwatch]::StartNew()
  while ($sw.ElapsedMilliseconds -lt $ms) { if (& $cond) { return $true }; Start-Sleep -Milliseconds 100 }
  return $false
}

# MSAA access to the window's controls (the API screen readers use).
Add-Type -ReferencedAssemblies Accessibility -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
using Accessibility;

public class AccControl { public IntPtr Hwnd; public string Name; public string Class; }

public static class Acc {
    delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] static extern bool EnumChildWindows(IntPtr p, EnumProc f, IntPtr l);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetClassName(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, string l);
    [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("oleacc.dll")] static extern int AccessibleObjectFromWindow(IntPtr h, uint id, ref Guid iid,
        [MarshalAs(UnmanagedType.Interface)] out IAccessible acc);
    static Guid IID = new Guid("618736E0-3C3D-11CF-810C-00AA00389B71");

    public static List<AccControl> Controls(IntPtr top) {
        var list = new List<AccControl>();
        EnumChildWindows(top, (h, l) => {
            if (!IsWindowVisible(h)) return true;
            IAccessible a;
            if (AccessibleObjectFromWindow(h, 0xFFFFFFFC, ref IID, out a) != 0 || a == null) return true;
            var c = new AccControl { Hwnd = h };
            try { c.Name = a.get_accName(0) ?? ""; } catch { c.Name = ""; }
            var sb = new StringBuilder(64); GetClassName(h, sb, 64); c.Class = sb.ToString();
            list.Add(c);
            return true;
        }, IntPtr.Zero);
        return list;
    }
    public static void Do(IntPtr h) {
        IAccessible a;
        AccessibleObjectFromWindow(h, 0xFFFFFFFC, ref IID, out a);
        a.accDoDefaultAction(0);
    }
}
'@

$fake = Join-Path (Split-Path $Launcher) 'launcher_tests.exe'
$work = Join-Path $env:TEMP "fl-e2e-$PID"
Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force "$work\games", "$work\my games" | Out-Null
Set-Content "$work\games\demo.swf" 'not a real swf'
Set-Content "$work\my games\online demo.swf" 'not a real swf'
$port = 47300 + ($PID % 500)
$config = @"
{
  "paths": { "games": "`${LAUNCHER_DIR}\\games", "players": "`${LAUNCHER_DIR}\\players",
             "data": "`${LAUNCHER_DIR}\\data", "flashpoint": [] },
  "players": [ { "id": "fake", "name": "Fake Player", "paths": ["$($fake -replace '\\','\\')"], "args": ["--fake-player"] } ],
  "categories": [ { "id": "online", "name": "Online multiplayer" } ],
  "games": [
    { "id": "demo", "name": "Demo Game", "players": "1-2 players", "categories": ["online"],
      "actions": [
        { "id": "play", "label": "Play", "swf": "`${GAMES_DIR}\\demo.swf" },
        { "id": "host", "label": "Host online", "swf": "`${LAUNCHER_DIR}\\my games\\online demo.swf",
          "args": ["--tcp-connections", "allow"], "relay": { "port": $port } }
      ] },
    { "id": "missing", "name": "Missing Game", "actions": [ { "id": "play", "label": "Play", "swf": "`${GAMES_DIR}\\nope.swf" } ] }
  ]
}
"@
Set-Content "$work\launcher.json" $config -Encoding UTF8
$env:FL_FAKE_OUT = "$work\args.txt"

# ---- command line ---------------------------------------------------------
$r = Run @('--help'); Check ($r.Code -eq 0 -and $r.Out -match '--launch') '--help prints usage'
$r = Run @('--version'); Check ($r.Code -eq 0 -and $r.Out -match 'Flash Launcher \d+\.\d+\.\d+') "--version prints the version ($($r.Out.Trim()))"
$r = Run @('--bogus'); Check ($r.Code -eq 3) 'unknown option exits 3'
$r = Run @('--validate', '--config', "`"$Root\launcher\launcher.json`""); Check ($r.Code -eq 0 -and $r.Out -match 'OK') 'shipped launcher.json validates'
$r = Run @('--validate', '--config', "`"$work\launcher.json`""); Check ($r.Code -eq 0) 'test config validates'
$r = Run @('--list', '--config', "`"$work\launcher.json`"")
Check ($r.Out -match 'play \(Play\): ready' -and $r.Out -match 'game file missing') '--list shows ready and missing games'

$env:FL_FAKE_MS = '0'
Remove-Item "$work\args.txt" -ErrorAction SilentlyContinue
$r = Run @('--launch', 'demo', '--config', "`"$work\launcher.json`"")
Check ($r.Code -eq 0) '--launch demo starts the player'
Check (WaitFor { Test-Path "$work\args.txt" }) 'player ran'
$args1 = @(Get-Content "$work\args.txt")
Check ($args1.Count -eq 1 -and $args1[0] -like '*\games\demo.swf') "player got the SWF path ($($args1 -join ' | '))"

$r = Run @('--launch', 'missing', '--config', "`"$work\launcher.json`"")
Check ($r.Code -eq 4 -and $r.Out -match 'missing') 'missing game file exits 4 with a reason'

# Host: relay must be up while the game runs, and gone after it exits.
$env:FL_FAKE_MS = '3000'
Remove-Item "$work\args.txt" -ErrorAction SilentlyContinue
$host1 = Start-Process -FilePath $Launcher -ArgumentList @('--launch', 'demo:host', '--config', "`"$work\launcher.json`"") -PassThru
Check (WaitFor { PortOpen $port }) "--launch demo:host opens the relay on $port"
$args2 = @()
if (WaitFor { Test-Path "$work\args.txt" }) { $args2 = @(Get-Content "$work\args.txt") }
Check ($args2.Count -eq 3 -and $args2[0] -eq '--tcp-connections' -and $args2[2] -like '*\my games\online demo.swf') "host passes args and a path with spaces ($($args2 -join ' | '))"
$host1.WaitForExit(15000) | Out-Null
Check ($host1.HasExited -and -not (PortOpen $port)) 'relay stops when the hosted game exits'

$relay = Start-Process -FilePath $Launcher -ArgumentList @('--relay', ($port + 1)) -PassThru
Check (WaitFor { PortOpen ($port + 1) }) '--relay runs a bare relay'
Stop-Process -Id $relay.Id -Force

# ---- installing a missing player (served from 127.0.0.1, no internet) -----
$pkg = Join-Path $work 'pkg\FakePlayer'
New-Item -ItemType Directory -Force $pkg | Out-Null
Copy-Item $fake (Join-Path $pkg 'fakeplay.exe')
Compress-Archive -Path $pkg -DestinationPath "$work\fake.zip"  # the archive wraps a top folder
$sha = (Get-FileHash "$work\fake.zip" -Algorithm SHA256).Hash.ToLower()
$httpPort = $port + 2
# A tiny web server: GET /<path> answers with the file www\<path>.
$www = Join-Path $work 'www'
New-Item -ItemType Directory -Force $www | Out-Null
Copy-Item "$work\fake.zip" $www
$server = Start-Job -ArgumentList $httpPort, $www -ScriptBlock {
  param($p, $root)
  $l = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, $p)
  $l.Start()
  for ($i = 0; $i -lt 40; $i++) {
    $c = $l.AcceptTcpClient(); $s = $c.GetStream()
    $buf = New-Object byte[] 8192; $n = $s.Read($buf, 0, $buf.Length)
    $path = ([Text.Encoding]::ASCII.GetString($buf, 0, $n) -split ' ')[1].TrimStart('/') -replace '/', '\'
    $file = Join-Path $root $path
    if (Test-Path $file -PathType Leaf) {
      $bytes = [IO.File]::ReadAllBytes($file)
      $h = [Text.Encoding]::ASCII.GetBytes("HTTP/1.1 200 OK`r`nContent-Length: $($bytes.Length)`r`nConnection: close`r`n`r`n")
      $s.Write($h, 0, $h.Length); $s.Write($bytes, 0, $bytes.Length)
    } else {
      $h = [Text.Encoding]::ASCII.GetBytes("HTTP/1.1 404 Not Found`r`nContent-Length: 0`r`nConnection: close`r`n`r`n")
      $s.Write($h, 0, $h.Length)
    }
    $s.Flush(); $c.Close()
  }
  $l.Stop()
}
Start-Sleep -Milliseconds 800
function InstallConfig([string]$hash) {
@"
{
  "paths": { "games": "`${LAUNCHER_DIR}\\games", "players": "`${LAUNCHER_DIR}\\players", "data": "`${LAUNCHER_DIR}\\data", "flashpoint": [] },
  "players": [ { "id": "fake", "name": "Fake Player", "paths": ["`${PLAYERS_DIR}\\fake\\fakeplay.exe"],
                 "install": { "url": "http://127.0.0.1:$httpPort/fake.zip", "sha256": "$hash",
                              "dir": "`${PLAYERS_DIR}\\fake", "exe": "fakeplay.exe" } } ],
  "games": [ { "id": "demo", "name": "Demo Game", "actions": [ { "id": "play", "label": "Play", "swf": "`${GAMES_DIR}\\demo.swf" } ] } ]
}
"@
}
Set-Content "$work\install.json" (InstallConfig $sha) -Encoding UTF8
$r = Run @('--list', '--config', "`"$work\install.json`"")
Check ($r.Out -match 'Fake Player: not found') 'player starts out missing'
$r = Run @('--install-player', 'fake', '--config', "`"$work\install.json`"")
Check ($r.Code -eq 0 -and (Test-Path "$work\players\fake\fakeplay.exe")) "--install-player downloads, checks and unpacks ($($r.Out.Trim() -replace '\s+', ' '))"
$r = Run @('--list', '--config', "`"$work\install.json`"")
Check ($r.Out -match 'play \(Play\): ready, Fake Player') 'the installed player is used right away'
Check (-not (Test-Path "$work\players\fake.installing") -and -not (Test-Path "$work\players\fake.old")) 'no leftovers after installing'

Set-Content "$work\install.json" (InstallConfig ('0' * 64)) -Encoding UTF8
$r = Run @('--install-player', 'fake', '--config', "`"$work\install.json`"")
Check ($r.Code -eq 1 -and $r.Out -match 'checksum') 'a download with the wrong checksum is refused'
Check (Test-Path "$work\players\fake\fakeplay.exe") 'a failed reinstall keeps the working player'
$r = Run @('--install-player', 'nope', '--config', "`"$work\install.json`"")
Check ($r.Code -eq 3) 'unknown player id exits 3'

# First run with no player: the window offers the install; accept it.
Set-Content "$work\install.json" (InstallConfig $sha) -Encoding UTF8
Remove-Item -Recurse -Force "$work\players\fake"
Add-Type -TypeDefinition @'
using System; using System.Runtime.InteropServices; using System.Text;
public static class Top {
    delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc f, IntPtr l);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetClassName(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    public static IntPtr Find(uint pid, string cls) {
        IntPtr found = IntPtr.Zero;
        EnumWindows((h, l) => { uint p; GetWindowThreadProcessId(h, out p); var sb = new StringBuilder(64); GetClassName(h, sb, 64);
            if (p == pid && sb.ToString() == cls) { found = h; return false; } return true; }, IntPtr.Zero);
        return found;
    }
}
'@
$first = Start-Process -FilePath $Launcher -ArgumentList @('--config', "`"$work\install.json`"") -PassThru
$dlg = [IntPtr]::Zero
WaitFor { $script:dlg = [Top]::Find([uint32]$first.Id, '#32770'); $script:dlg -ne [IntPtr]::Zero } 10000 | Out-Null
Check ($dlg -ne [IntPtr]::Zero) 'first run with no player asks to install one'
if ($dlg -ne [IntPtr]::Zero) {
  [Top]::SendMessage($dlg, 0x0466, [IntPtr]6, [IntPtr]::Zero) | Out-Null  # TDM_CLICK_BUTTON, IDYES
  Check (WaitFor { Test-Path "$work\players\fake\fakeplay.exe" } 15000) 'accepting the prompt installs the player'
}
Stop-Process -Id $first.Id -Force -ErrorAction SilentlyContinue
# ---- downloading missing game files from the catalog -----------------------
New-Item -ItemType Directory -Force "$www\games\demo2" | Out-Null
Set-Content "$www\games\demo2\dl.swf" 'pretend game file' -NoNewline
$gsha = (Get-FileHash "$www\games\demo2\dl.swf" -Algorithm SHA256).Hash.ToLower()
$gsize = (Get-Item "$www\games\demo2\dl.swf").Length
function CatalogJson([string]$hash) {
  "{ `"schemaVersion`": 1, `"files`": [ { `"name`": `"dl.swf`", `"game`": `"demo2`", `"path`": `"games/demo2/dl.swf`", `"size`": $gsize, `"sha256`": `"$hash`" } ] }"
}
Set-Content "$www\catalog.json" (CatalogJson $gsha) -Encoding ASCII
@"
{
  "paths": { "games": "`${LAUNCHER_DIR}\\dlgames", "players": "`${LAUNCHER_DIR}\\players", "data": "`${LAUNCHER_DIR}\\data", "flashpoint": [] },
  "downloads": { "catalog": "http://127.0.0.1:$httpPort/catalog.json" },
  "players": [ { "id": "fake", "name": "Fake Player", "paths": ["$($fake -replace '\\','\\')"] } ],
  "games": [ { "id": "demo2", "name": "Downloadable Game", "actions": [ { "id": "play", "label": "Play", "swf": "`${GAMES_DIR}\\dl.swf" } ] } ]
}
"@ | Set-Content "$work\dl.json" -Encoding UTF8
$r = Run @('--list', '--config', "`"$work\dl.json`"")
Check ($r.Out -match 'game file missing') 'downloadable game starts out missing'
$r = Run @('--download-game', 'demo2', '--config', "`"$work\dl.json`"")
Check ($r.Code -eq 0 -and (Test-Path "$work\dlgames\dl.swf")) '--download-game saves the file from the catalog'
Check ((Get-FileHash "$work\dlgames\dl.swf" -Algorithm SHA256).Hash.ToLower() -eq $gsha) 'downloaded file matches the catalog checksum'
$r = Run @('--list', '--config', "`"$work\dl.json`"")
Check ($r.Out -match 'play \(Play\): ready') 'downloaded game is ready to play'
Remove-Item "$work\dlgames\dl.swf"
Set-Content "$www\catalog.json" (CatalogJson ('0' * 64)) -Encoding ASCII
$r = Run @('--download-game', 'demo2', '--config', "`"$work\dl.json`"")
Check ($r.Code -eq 1 -and -not (Test-Path "$work\dlgames\dl.swf") -and -not (Test-Path "$work\dlgames\dl.swf.part")) 'a file with the wrong checksum is not saved'
$r = Run @('--download-game', 'nosuchgame', '--config', "`"$work\dl.json`"")
Check ($r.Code -eq 3) 'unknown game id exits 3'

# The window: the catalog arrives in the background, then Download game files,
# accept the prompt, and the file lands in the games folder.
Set-Content "$www\catalog.json" (CatalogJson $gsha) -Encoding ASCII
$dlwin = Start-Process -FilePath $Launcher -ArgumentList @('--config', "`"$work\dl.json`"") -PassThru
$dlHwnd = [IntPtr]::Zero
WaitFor { $dlwin.Refresh(); $dlwin.MainWindowHandle -ne 0 } 10000 | Out-Null
$btn = $null
if ($dlwin.MainWindowHandle -ne 0) {
  WaitFor { $script:btn = [Acc]::Controls($dlwin.MainWindowHandle) | Where-Object { $_.Name -like 'Download game files*' } | Select-Object -First 1; $null -ne $script:btn } 10000 | Out-Null
}
Check ($null -ne $btn) "window offers Download game files ($($btn.Name))"
if ($btn) {
  [Acc]::Do($btn.Hwnd)
  $dlg = [IntPtr]::Zero
  WaitFor { $script:dlg = [Top]::Find([uint32]$dlwin.Id, '#32770'); $script:dlg -ne [IntPtr]::Zero } 10000 | Out-Null
  if ($dlg -ne [IntPtr]::Zero) { [Top]::SendMessage($dlg, 0x0466, [IntPtr]6, [IntPtr]::Zero) | Out-Null }
  Check (WaitFor { Test-Path "$work\dlgames\dl.swf" } 15000) 'accepting the prompt downloads the game file'
}
Stop-Process -Id $dlwin.Id -Force -ErrorAction SilentlyContinue
# ---- open rooms, game updates and Settings --------------------------------
$centralPort = $port + 3
$central = Start-Job -ArgumentList $centralPort -ScriptBlock {
  param($p)
  $l = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, $p)
  $l.Start()
  for ($i = 0; $i -lt 30; $i++) {
    $c = $l.AcceptTcpClient(); $s = $c.GetStream()
    $buf = New-Object byte[] 1024; $null = $s.Read($buf, 0, $buf.Length)
    $bytes = [Text.Encoding]::UTF8.GetBytes("LIST|ABCDE,1,4,Test room;FULL1,4,4,Full room" + [char]0)
    $s.Write($bytes, 0, $bytes.Length); $s.Flush(); $c.Close()
  }
  $l.Stop()
}
Start-Sleep -Milliseconds 800
# The catalog now claims a different build of dl.swf, which is already downloaded.
Set-Content "$www\catalog.json" (CatalogJson ('1' * 64)) -Encoding ASCII
Remove-Item -Recurse -Force "$work\rdata" -ErrorAction SilentlyContinue
@"
{
  "paths": { "games": "`${LAUNCHER_DIR}\\dlgames", "players": "`${LAUNCHER_DIR}\\players", "data": "`${LAUNCHER_DIR}\\rdata", "flashpoint": [] },
  "downloads": { "catalog": "http://127.0.0.1:$httpPort/catalog.json" },
  "rooms": { "server": "127.0.0.1:$centralPort", "refreshSeconds": 10 },
  "players": [ { "id": "fake", "name": "Fake Player", "paths": ["$($fake -replace '\\','\\')"], "args": ["--fake-player"], "flashvarArg": "-P`${KEY}=`${VALUE}" } ],
  "games": [ { "id": "demo2", "name": "Downloadable Game", "online": { "id": "demo2", "join": "play" },
               "actions": [ { "id": "play", "label": "Play", "swf": "`${GAMES_DIR}\\dl.swf" } ] } ]
}
"@ | Set-Content "$work\rooms.json" -Encoding UTF8
$r = Run @('--rooms', 'demo2', '--config', "`"$work\rooms.json`"")
Check ($r.Code -eq 0 -and $r.Out -match '2 open' -and $r.Out -match 'ABCDE\s+1/4\s+Test room') "--rooms lists the central server's rooms"

Add-Type -TypeDefinition @'
using System; using System.Runtime.InteropServices;
public static class Win2 {
    [DllImport("user32.dll")] public static extern bool IsWindowEnabled(IntPtr h);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
}
'@
$env:FL_FAKE_MS = '500'
Remove-Item "$work\args.txt" -ErrorAction SilentlyContinue
$rw = Start-Process -FilePath $Launcher -ArgumentList @('--config', "`"$work\rooms.json`"") -PassThru
try {
  WaitFor { $rw.Refresh(); $rw.MainWindowHandle -ne 0 } 10000 | Out-Null
  $h = $rw.MainWindowHandle
  $join = $null; $full = $null
  WaitFor { $script:join = [Acc]::Controls($h) | Where-Object { $_.Name -eq 'Join room ABCDE' } | Select-Object -First 1; $null -ne $script:join } 10000 | Out-Null
  $full = [Acc]::Controls($h) | Where-Object { $_.Name -eq 'Join room FULL1' } | Select-Object -First 1
  Check ($null -ne $join) 'the details panel lists open rooms with Join buttons'
  Check ($null -ne $full -and -not [Win2]::IsWindowEnabled($full.Hwnd)) "a full room's Join button is disabled"
  $upd = [Acc]::Controls($h) | Where-Object { $_.Name -like 'Update game files*' } | Select-Object -First 1
  Check ($null -ne $upd) 'a newer build in the catalog shows Update game files'
  if ($join) {
    [Acc]::Do($join.Hwnd)
    Check (WaitFor { (Test-Path "$work\args.txt") -and ((Get-Content "$work\args.txt") -contains '-Pjoin=ABCDE') }) 'Join passes the room code to the player (-Pjoin=ABCDE)'
    $args3 = @(Get-Content "$work\args.txt" -ErrorAction SilentlyContinue)
    Check ($args3.Count -ge 2 -and $args3[-1] -like '*dl.swf') "the game file stays last ($($args3 -join ' | '))"
    Check ((Get-Clipboard) -eq 'ABCDE') 'Join copies the room code'
  }
  $setBtn = [Acc]::Controls($h) | Where-Object { $_.Name -eq 'Settings' } | Select-Object -First 1
  if ($setBtn) {
    [Acc]::Do($setBtn.Hwnd)
    $setWnd = [IntPtr]::Zero
    WaitFor { $script:setWnd = [Top]::Find([uint32]$rw.Id, 'MultiplayerFlashLauncherSettings'); $script:setWnd -ne [IntPtr]::Zero } 5000 | Out-Null
    Check ($setWnd -ne [IntPtr]::Zero) 'Settings opens'
    if ($setWnd -ne [IntPtr]::Zero) {
      # The window exists a moment before its controls do.
      $min = $null
      WaitFor { $script:min = [Acc]::Controls($setWnd) | Where-Object { $_.Name -eq 'Minimize the launcher while a game runs' } | Select-Object -First 1; $null -ne $script:min } 5000 | Out-Null
      if ($min) { [Acc]::Do($min.Hwnd) }
      Check (WaitFor { (Get-Content "$work\rdata\settings.json" -Raw -ErrorAction SilentlyContinue) -match '"minimize": 1' }) 'a Settings checkbox applies and saves at once'
      [Win2]::PostMessage($setWnd, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null  # WM_CLOSE
      Check (WaitFor { [Top]::Find([uint32]$rw.Id, 'MultiplayerFlashLauncherSettings') -eq [IntPtr]::Zero } 5000) 'Settings closes'
      Check ([Win2]::IsWindowEnabled($h)) 'the library is usable again after Settings closes'
    }
  } else { Check $false 'Settings button found' }
} finally {
  Stop-Process -Id $rw.Id -Force -ErrorAction SilentlyContinue
  Stop-Job $central -ErrorAction SilentlyContinue; Remove-Job $central -Force -ErrorAction SilentlyContinue
}
Stop-Job $server -ErrorAction SilentlyContinue; Remove-Job $server -Force -ErrorAction SilentlyContinue

# ---- window, through MSAA (the API screen readers use for Win32 controls) ----
function Ctl([IntPtr]$top, [string]$name) { [Acc]::Controls($top) | Where-Object { $_.Name -eq $name } | Select-Object -First 1 }
function OfClass([IntPtr]$top, [string]$cls) { [Acc]::Controls($top) | Where-Object { $_.Class -eq $cls } | Select-Object -First 1 }
function GridCount([IntPtr]$top) {
  $g = [Acc]::Controls($top) | Where-Object { $_.Class -eq 'SysListView32' } | Select-Object -First 1
  if (-not $g) { return 0 }  # hidden when nothing matches
  return [int][Acc]::SendMessage($g.Hwnd, 0x1004, [IntPtr]::Zero, [IntPtr]::Zero)  # LVM_GETITEMCOUNT
}

$env:FL_FAKE_MS = '1500'
Remove-Item "$work\args.txt" -ErrorAction SilentlyContinue
$gui = Start-Process -FilePath $Launcher -ArgumentList @('--config', "`"$work\launcher.json`"") -PassThru
try {
$win = [IntPtr]::Zero
WaitFor { $gui.Refresh(); $gui.MainWindowHandle -ne 0 } 10000 | Out-Null
$win = $gui.MainWindowHandle
Check ($win -ne [IntPtr]::Zero) 'window opens'
if ($win -ne [IntPtr]::Zero) {
  $play = Ctl $win 'Play'
  Check ($null -ne $play) 'Play button has an accessible name'
  if ($play) {
    [Acc]::Do($play.Hwnd)
    Check (WaitFor { Test-Path "$work\args.txt" }) 'Play (default action) starts the player'
  }
  $hostBtn = Ctl $win 'Host online'
  if ($hostBtn) {
    [Acc]::Do($hostBtn.Hwnd)
    Check (WaitFor { PortOpen $port }) 'Host online starts the relay'
    $stop = $null
    WaitFor { $script:stop = Ctl $win 'Stop hosting'; $null -ne $script:stop } | Out-Null
    Check ($null -ne $stop) 'hosting box shows Stop hosting'
    if ($stop) {
      [Acc]::Do($stop.Hwnd)
      Check (WaitFor { -not (PortOpen $port) }) 'Stop hosting closes the relay'
    }
  } else { Check $false 'Host online button found' }

  Check ((GridCount $win) -eq 2) 'grid lists both games'
  $search = OfClass $win 'Edit'
  if ($search) {
    [Acc]::SendMessage($search.Hwnd, 0x000C, [IntPtr]::Zero, 'missing') | Out-Null  # WM_SETTEXT
    Check (WaitFor { (GridCount $win) -eq 1 }) 'search filters the grid'
    [Acc]::SendMessage($search.Hwnd, 0x000C, [IntPtr]::Zero, 'zzzz') | Out-Null
    Check (WaitFor { (GridCount $win) -eq 0 }) 'search with no match empties the grid'
    [Acc]::SendMessage($search.Hwnd, 0x000C, [IntPtr]::Zero, '') | Out-Null
    Check (WaitFor { (GridCount $win) -eq 2 }) 'clearing the search shows everything again'
  } else { Check $false 'search box found' }
  [Acc]::PostMessage($win, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null  # WM_CLOSE
  $gui.WaitForExit(5000) | Out-Null
}
Check ($gui.HasExited) 'window closes'
$settings = Get-Content "$work\data\settings.json" -Raw -ErrorAction SilentlyContinue
Check ($settings -match '"plays": [1-9]') 'play count is saved'
} catch {
  Check $false "window automation: $($_.Exception.Message)"
} finally {
  if (-not $gui.HasExited) { Stop-Process -Id $gui.Id -Force }
}

Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
if ($failures) { Write-Host "$failures check(s) failed"; exit 1 }
Write-Host 'All end-to-end checks passed'
