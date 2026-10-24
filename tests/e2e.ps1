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

# ---- window, through MSAA (the API screen readers use for Win32 controls) ----
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
