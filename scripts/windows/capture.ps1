# One capture: runs the renderer with a config derived from a base file by overriding [render] keys,
# renames the screenshot to <Tag>.png, and reports the mean channels plus whether the run was
# validation clean.
#
# WHY IT EXISTS. `check_render.ps1` answers "did this change any of the reference frames", which is
# the gate. It cannot answer "what does this KNOB do", because every measurement here is an A/B on one
# setting of one frame of one scene, taken dozens of times with different values. This is that
# instrument: one base config, a list of [render] overrides, and a named capture the python tools in
# scripts/measure/ can be pointed at.
#
# The overrides are applied to WHOLE LINES in the [render] table (a key that is not already there is
# appended), and the script asserts that every override actually changed the file - the repository's
# own recorded trap is an edit that silently matched nothing, after which the numbers read as if they
# described a change that was never compiled. It also refuses an override whose value the base file
# already has, for the same reason from the other side: "no change: the overrides matched nothing"
# means the run you are about to read is the run you already have.
#
# Usage:
#   pwsh -File scripts/windows/capture.ps1 -Base my.toml -Tag exp1 -Overrides "ssao_radius=2.0"
#   pwsh -File scripts/windows/capture.ps1 -Base my.toml -Tag wide -Camera "90,0,6.41,0,-18.548,0" -Frames 120
#   pwsh -File scripts/windows/capture.ps1 -Base my.toml -Tag asan -BuildDir build-asan
#
# -Base is resolved relative to the WORK directory (see -WorkDir), which is where the configs and the
# captures live; -Camera "" leaves the pose to the scene's own `camera_fit`, which is deterministic for
# a static scene and is what a scene with no pinned pose resolves to (the run's log prints it).
#
# TWO CAPTURES ARE ONLY COMPARABLE AT THE SAME POSE, which is worth stating because getting it wrong does
# not look like a mistake: comparing a capture that passed -Camera against one that did not reads as a
# whole-frame mean|d| of tens and a difference everywhere, i.e. exactly like a rendering bug. A rendering
# change's signature is small numbers concentrated where the change is. The log's `capture camera:` line is
# the record of which pose a capture was taken at, and two captures are comparable when those lines match.
#
# The captures land beside the build (`<BuildDir>/gi-probe/`), NOT in the repository: they are 4 MB of
# scratch per frame and belong with the binaries they came from. That is also why the instruments they
# are read with now live in the repository (scripts/measure/) while the images do not.

param(
    [Parameter(Mandatory = $true)][string]$Base,
    [Parameter(Mandatory = $true)][string]$Tag,
    [string]$Overrides = "",
    [int]$Frames = 180,
    [string]$Camera = "90,0,6.41,0,-18.548,0",
    [string]$Model = "",
    [string]$BuildDir = "build-release-clang64",
    [string]$WorkDir = "",
    # Degrees of yaw added per presented frame (`--capture-sweep`). 0 = a fixed camera, which is what every
    # other measurement in this repository is; a non-zero value makes the capture exercise the reprojection
    # paths (TAA's history, the GI temporal accumulation, the motion vectors) that a still camera leaves in
    # their trivial case. It is frame-indexed rather than clock-driven, so a sweep is as reproducible as a
    # still capture - the harness's own two-run determinism check is what verifies that.
    [float]$Sweep = 0.0,
    [switch]$KeepLog
)

$ErrorActionPreference = "Stop"
$here = $PSScriptRoot
$repo = Split-Path -Parent (Split-Path -Parent $here)
if (-not $WorkDir) { $WorkDir = Join-Path $repo "$BuildDir\gi-probe" }
New-Item -ItemType Directory -Force -Path $WorkDir | Out-Null
# THE WORK DIRECTORY IS RESOLVED TO AN ABSOLUTE PATH HERE, once, because the config path below is handed to a
# process whose WORKING DIRECTORY is this same directory (see the launch): a relative `--config` would then be
# resolved against itself and never found - measured as `config file '...\s2work\s2_pre_on.toml' not found,
# using defaults` followed by the app's "run the program from the project root" panic. An absolute path makes
# `-WorkDir` usable from anywhere, which is what the parameter promises.
$WorkDir = (New-Item -ItemType Directory -Force -Path $WorkDir).FullName
$exe = Join-Path $repo "$BuildDir\deren.exe"
if (-not (Test-Path $exe)) { Write-Error "no executable at $exe - build it first (-BuildDir to point elsewhere)"; exit 1 }
$mean_tool = Join-Path $repo "scripts\measure\mean.py"

$basePath = Join-Path $WorkDir $Base
if (-not (Test-Path $basePath)) { Write-Error "no base config at $basePath"; exit 1 }
$cfgText = Get-Content $basePath -Raw
$cfg = Join-Path $WorkDir "$Tag.toml"

# -Model replaces the top-level `model` line (what varies there is the SCENE, and a measurement's
# premise is usually a property of the scene - see the notes on convexity in the furnace captures).
if ($Model) {
    $before = $cfgText
    # A TOML BASIC string (double quotes), because that is the form the escape is FOR: the single-quoted
    # literal this used to write kept the doubled backslashes literally, so the file said
    # `C:\\Users\\...` and the path only resolved because Win32 collapses a repeated separator. A UNC path
    # would not have survived that, and the log line quoted a path the user never typed.
    $escaped = $Model.Replace('\', '\\').Replace('"', '\"')
    # THE `model` LINE IS COUNTED AS WELL AS REPLACED, because "the text did not change" is TWO different
    # situations and only one of them is a mistake: a `-Model` the base file does not have a line for (the
    # typo this guard exists for), and a `-Model` the base file ALREADY names. The second is the normal case
    # on a step whose asset is part of its base config - `deren-ab/laevat/ab_goo.toml` names the goo asset - and
    # it used to abort the run with "no change: -Model matched no line", which is false about the file.
    $model_lines = 0
    $replaced = foreach ($line in ($cfgText -split "`n")) {
        if ($line -match '^\s*model\s*=') {
            $model_lines++
            "model = `"$escaped`""
        } else {
            $line
        }
    }
    $cfgText = $replaced -join "`n"
    if ($model_lines -eq 0) { Write-Error "no change: -Model matched no line in $Base"; exit 1 }
    if ($cfgText -eq $before) { Write-Host "  note: -Model is the model $Base already names (the run is still a valid arm)" }
}

$pairs = @()
if ($Overrides) {
    foreach ($o in $Overrides.Split(";")) {
        if ($o -eq "") { continue }
        $kv = $o.Split("=", 2)
        if ($kv.Count -ne 2) { Write-Error "bad override '$o' (expected key=value)"; exit 1 }
        $pairs += , @($kv[0], $kv[1])
    }
}

# apply each override to the [render] table
$lines = $cfgText -split "`n"
$inRender = $false
$applied = @{}
foreach ($p in $pairs) {
    $key = $p[0].Trim(); $value = $p[1].Trim()
    $found = $false
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $line = $lines[$i]
        if ($line -match '^\s*\[(.+)\]\s*$') { $inRender = ($Matches[1] -eq 'render'); continue }
        if (-not $inRender) { continue }
        if ($line -match "^\s*$([regex]::Escape($key))\s*=") {
            if ($line.Trim() -eq "$key = $value") { Write-Error "override $key = $value is what $Base already has - the run would not be a new arm"; exit 1 }
            $lines[$i] = "$key = $value"
            $found = $true
            break
        }
    }
    if (-not $found) {
        # append inside [render]: after the last line that belongs to it
        $lastIndex = -1; $inRender = $false
        for ($i = 0; $i -lt $lines.Count; $i++) {
            if ($lines[$i] -match '^\s*\[(.+)\]\s*$') { if ($Matches[1] -eq 'render') { $inRender = $true } else { if ($inRender) { break } }; continue }
            if ($inRender -and $lines[$i].Trim() -ne "") { $lastIndex = $i }
        }
        if ($lastIndex -lt 0) { Write-Error "could not find the [render] table in $basePath"; exit 1 }
        $lines = @($lines[0..$lastIndex]) + @("$key = $value") + @($lines[($lastIndex + 1)..($lines.Count - 1)])
    }
    $applied[$key] = $value
}
$newText = $lines -join "`n"
if ($newText -eq $cfgText -and $pairs.Count -gt 0) { Write-Error "no change: the overrides matched nothing"; exit 1 }
Set-Content -Path $cfg -Value $newText -NoNewline

# every override has to be readable back out of the file that will actually be used
foreach ($k in $applied.Keys) {
    $read = Select-String -Path $cfg -Pattern "^\s*$([regex]::Escape($k))\s*=\s*$([regex]::Escape($applied[$k]))\s*$"
    if (-not $read) { Write-Error "override $k = $($applied[$k]) is not in $cfg"; exit 1 }
}

Get-ChildItem $WorkDir -Filter "screenshot_*.png" -ErrorAction SilentlyContinue | Remove-Item -Force
$log = Join-Path $WorkDir "debug.log"
Remove-Item $log -Force -ErrorAction SilentlyContinue

$launch = @("--config", $cfg, "--capture-frames", "$Frames")
# An empty -Camera leaves the pose to the scene's own `camera_fit`, which is deterministic for a static
# scene and is what the model's framing rule resolves to (the log prints it). The flag is omitted
# rather than passed empty, because `--capture-camera=` with no numbers is not a valid pose.
if ($Camera) { $launch += "--capture-camera=$Camera" }
if ($Sweep -ne 0.0) { $launch += "--capture-sweep=$Sweep" }
$sw = [System.Diagnostics.Stopwatch]::StartNew()
# LAUNCHED WITHOUT THE SHELL, AND THAT IS THE POINT RATHER THAN A STYLE CHOICE. `Start-Process` goes through
# ShellExecuteEx, and ShellExecuteEx is the path Windows runs its app-reputation ("unknown publisher" / "untrusted
# program") check on - so every capture raised a security prompt for an unsigned binary that is built locally on
# the machine it runs on. `UseShellExecute = $false` uses CreateProcess, which has no such check. The two things
# the shell was doing for us are done explicitly instead: the working directory, and a hidden window (STARTUPINFO
# rather than a shell verb). Standard output is NOT redirected, deliberately: this app's own `debug.log` is how a
# capture is read, and a redirected pipe would need a concurrent reader or a 60-frame run could deadlock on a full
# buffer.
$start = [System.Diagnostics.ProcessStartInfo]::new()
$start.FileName = $exe
$start.WorkingDirectory = $WorkDir
$start.UseShellExecute = $false
$start.CreateNoWindow = $true
$start.WindowStyle = [System.Diagnostics.ProcessWindowStyle]::Hidden
foreach ($arg in $launch) { $null = $start.ArgumentList.Add($arg) }
$p = [System.Diagnostics.Process]::Start($start)
if (-not $p.WaitForExit(300000)) { $p.Kill(); Write-Error "$Tag timed out"; exit 1 }
$sw.Stop()
if ($p.ExitCode -ne 0) { Write-Error "${Tag}: exit code $($p.ExitCode)"; exit 1 }

$shot = Get-ChildItem $WorkDir -Filter "screenshot_*.png" | Select-Object -First 1
if (-not $shot) { Write-Error "${Tag}: no screenshot produced"; exit 1 }
$out = Join-Path $WorkDir "$Tag.png"
Copy-Item $shot.FullName $out -Force

$bad = @()
if (Test-Path $log) { $bad = Select-String -Path $log -Pattern "VUID-|Validation Error|\[ERROR\]|\[WARNING\]|panic|recorded out of order" }

Write-Host "=== $Tag  ($([math]::Round($sw.Elapsed.TotalSeconds,1)) s)  overrides: $Overrides"
if (Test-Path $log) {
    $scene = Select-String -Path $log -Pattern "shadow mapping enabled: light frustum center .* radius ([\d.]+)"
    if ($scene) { Write-Host "  scene radius $($scene[0].Matches[0].Groups[1].Value)" }
    $modelLine = Select-String -Path $log -Pattern "^rendering '(.+)' with"
    if ($modelLine) { Write-Host "  model: $($modelLine[0].Matches[0].Groups[1].Value)" }
    # The CAPTURE pose, not the "initial camera" one: the app prints both, and on a run that passes
    # --capture-camera they are DIFFERENT poses (the initial line is `camera_fit`'s unresolved framing -
    # on Sponza an exterior fit at distance 51 against the interior pose at 6.41). Printing the wrong one
    # is how a measurement's premise gets quoted wrong, which is exactly what happened the first time the
    # L2.4 cost arms were read. The fallback is for a run that leaves the pose to the scene.
    $cam = Select-String -Path $log -Pattern "capture camera: (.+)"
    if ($cam) {
        Write-Host "  camera: $($cam[0].Matches[0].Groups[1].Value)"
    } else {
        $cam = Select-String -Path $log -Pattern "initial camera: (.+)"
        if ($cam) { Write-Host "  camera (scene-framed, no --capture-camera): $($cam[0].Matches[0].Groups[1].Value)" }
    }
}
if ($bad) {
    Write-Host "  VALIDATION/LOG PROBLEMS:" -ForegroundColor Red
    $bad | Select-Object -First 5 | ForEach-Object { Write-Host "    $($_.Line.Trim())" }
} else {
    Write-Host "  validation clean" -ForegroundColor Green
}
# The instruments in scripts/measure/ need Pillow, and a shell whose `python` is a different one (a
# toolchain's, say) turns that into a traceback under a capture that otherwise succeeded - which reads
# as a failed run. Say what is missing instead.
python -c "import PIL" 2>&1 | Out-Null
if ($LASTEXITCODE -ne 0) {
    Write-Host "  capture OK, but this shell's python has no Pillow: run 'python scripts/measure/mean.py $out' with one that does" -ForegroundColor Yellow
} else {
    python $mean_tool $out
}
if (-not $KeepLog) { Copy-Item $log (Join-Path $WorkDir "$Tag.log") -Force -ErrorAction SilentlyContinue }
