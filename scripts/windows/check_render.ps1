# Local screenshot regression check for the renderer.
#
# WHAT IT IS: it renders a fixed set of scenarios (fixed config, fixed camera, fixed frame count),
# screenshots each one, and compares it to a reference captured earlier ON THIS MACHINE. A difference
# means the frame CHANGED - not that it got better or worse. An intended visual change is accepted by
# re-running with -Update, which is the explicit step that makes the change visible in review.
#
# WHAT IT CATCHES: logic and wiring regressions, which is what the bugs found so far have all been -
# the deferred path's emissive term silently not being written, and the debug overlay opening a second
# rendering instance that CLEARED the composite it was supposed to sit on top of. Both changed the
# whole image.
# WHAT IT DOES NOT CATCH: a slightly noisier shadow, a 2% colour shift, or anything about performance.
#
# IT IS DELIBERATELY NOT A CI TEST. CI has no GPU, so this cannot run there at all; and the references
# are tied to this machine's GPU + driver, so a shared baseline would be red for everyone else.
#
# TWO PROPERTIES IT DEPENDS ON, both measured rather than assumed:
#  1. [gui] show = false. With the overlay on, the frame is NOT deterministic - it prints a live fps
#     counter, and three consecutive runs of one binary produced three different hashes. With it off,
#     three runs produced one hash. Every scenario therefore forces the overlay off and passes its own
#     config file, so a developer's own config.toml cannot influence it.
#  2. A fixed frame count: TAA accumulates over frames, so frame 10 and frame 40 differ.
# Check mode runs every scenario TWICE and requires the two runs to agree before comparing against the
# reference, so "flaky" is reported as flaky instead of as a regression.
#
#  3. WHICH SCENARIOS RUN: the DEFAULT run is the CORE set - five of the fourteen scenarios defined below,
#     one per pipeline family a wiring change can break - because every scenario is TWO runs (the
#     determinism check below), so the full list costs 28 renders a round and the extra nine mostly
#     answer questions the core five also answer. A core round is 5 x 2 = 10 renders. The nine that
#     are `tier = "extra"` are still checked on demand: `-Full` runs all fourteen, `-Only <name>` runs one.
#     They are worth naming here so the choice to skip them is deliberate: `deferred_taa_fxaa` is the
#     AA stage, `deferred_ssao_off` and `shadow_single` vary one optional stage each,
#     `metal_rough_glossy` / `glossy_motion` are the material sweep (the second one with a MOVING
#     camera), and the four `laevatain_*` scenarios are the REWRITTEN TOON CHAIN's net - the chain on,
#     the same chain at the BODY pose (the only pose whose frame the RS mechanism can move), the same
#     chain on an asset whose per-material sidecar is MISSING (the fallback control), and the OLD
#     chain on the first asset (the byte-identity criterion). Run `-Full` after a driver update or before
#     re-baselining, so no reference goes stale unwatched: -Update only re-baselines the scenarios it ran.
#
#  4. WHICH BUILD IT RUNS: the Release build, and only the Release build. Pointed at a Debug or an
#     ASan+UBSan build, two runs of one binary DIFFER - measured on TWO scenarios, `sponza` and
#     `deferred_ssao_off` - so the flakiness is a property of those builds and not of any pass. The
#     references are Release captures, and "0 changed" is only meaningful against them.
#
#  5. WHICH DRIVER: the references are per-DRIVER values, not merely per-renderer ones. Updating the GPU driver
#     changes what this renderer outputs without a line of code changing: measured on 2026-09-18, moving the
#     NVIDIA driver from 591.59 to 616.92 made 8 of the 9 scenarios differ (`transparent_blend` was the only one
#     that survived), and the PARENT COMMIT - built without the change that was under test - reproduced exactly
#     the same 8 against the same references, which is how the driver was shown to be the cause rather than the
#     code. The lesson is procedural: after a driver update, run the gate on the commit BEFORE your own change
#     to see whether "changed" is yours, and re-baseline (-Update) with the reason written down - archiving the
#     old set first, because it is the only record of what the previous driver looked like.
#
# Usage:
#   pwsh -File scripts/windows/check_render.ps1                 # the CORE set (5 scenarios x 2 runs)
#   pwsh -File scripts/windows/check_render.ps1 -Full           # all FOURTEEN scenarios
#   pwsh -File scripts/windows/check_render.ps1 -Update         # accept the current output as reference
#   pwsh -File scripts/windows/check_render.ps1 -Only deferred_taa_fxaa
#   pwsh -File scripts/windows/check_render.ps1 -List
#
# References live OUTSIDE the repository (they are machine-specific; committing them would be red for
# everyone else and would tie every accepted change to a multi-megabyte commit). Override the location
# with VR_RENDER_BASELINE_DIR. The three `laevatain_*` scenarios are seeded the same way as every other
# one - `-Update` copies the frame it just drew to `<baseDir>\<scenario>.png` - and there is no separate
# seeding path for them; -List prints the directory. NOTE that their ASSETS do not live in the repository
# either (see $charDir), so those three references are reproducible only on a machine that has the
# `chars\` assets under its build directory.

param(
    [switch]$Update,
    [switch]$List,
    [switch]$Full,
    [string]$Only = "",
    [string]$BuildDir = "",
    [string]$Model = "C:\Users\23530\Desktop\yzk\glTF-Sample-Assets\Models\DamagedHelmet\glTF\DamagedHelmet.gltf",
    [int]$Frames = 40,
    [string]$Camera = "35,20,7,0,-1.6,0",
    [int]$Width = 1080,
    [int]$Height = 960
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $BuildDir) { $BuildDir = Join-Path $repo "build-release-clang64" }
$exe = Join-Path $BuildDir "deren.exe"
if (-not (Test-Path $exe)) { Write-Error "no executable at $exe - build first (-BuildDir to point elsewhere)"; exit 1 }

# CANONICALIZE THE BUILD DIR, and this is a measured defect rather than tidiness: the scenario config carries
# `screenshot_dir = <BuildDir>\render-check` and the app is then launched with that directory as its WORKING
# directory, so a RELATIVE -BuildDir writes a relative path into the config and the two bases no longer agree
# about where the screenshot lands. The symptom is a FALSE FLAKY: with `-BuildDir build-release-clang64`,
# `sponza` reported 1D72F82B946F0338 against 54B854F69AEAA620, neither of them the reference, while the
# same binary with an absolute -BuildDir returned the reference BF180E98ADB29E7E twice. Resolving here makes
# both spellings mean the same run.
$BuildDir = (Resolve-Path $BuildDir).Path

$baseDir = if ($env:DEREN_BASELINE_DIR) { $env:DEREN_BASELINE_DIR } else { Join-Path $env:LOCALAPPDATA "deren\baseline" }
$workDir = Join-Path $BuildDir "render-check"

# ---------------------------------------------------------------------------------------------
# The scenarios. Each is a full config (only the keys that differ from the defaults are listed) plus
# the frame count; the camera, model and extent are shared above so a scenario only varies what it
# means to - a scenario may override `model` / `camera` when it has to (see transparent_blend).
#
# `tier` is what the default run selects: `core` (five) or `extra` (nine, i.e. -Full or -Only). The
# core five are one scenario per pipeline family whose WIRING has broken before: the deferred
# G-buffer and its lighting (deferred), the forward unlit pipeline (unlit), the forward default
# pipeline with a BLEND leaf and a MASK discard (transparent_blend), the heavy scene that adds
# cascaded shadows, clustered lights, IBL and the bulk of the mesh workload (sponza), and the one
# DEFORMING mesh, whose frame is the motion channel itself (deformation). A tier is required of every
# scenario - the check below fails on a missing or unknown one, so adding a scenario means deciding
# whether it earns a place in the default round rather than silently never running.
#
# THE FOUR `laevatain_*` SCENARIOS ARE ALL `extra`, and that is a decision this file owes a reason for:
# they exist to put the REWRITTEN TOON CHAIN inside this gate (its landed steps were accepted by
# hand-run A/B captures, which is not a net) and to hold the chain's two arms apart - the rewritten one on
# the sidecar-carrying asset, the same chain on an asset with NO sidecar (the fallback control), the OLD
# chain on the first asset (the byte-identity criterion), and the rewritten chain ONE POSE FURTHER IN, at
# the body, where alone the two `_GooRSScalars` / `_GooRSTint` materials are drawn (see its own note).
# The first three are one character at ONE pose, so they answer a question the core five do not; the fourth
# exists because that one pose cannot see that mechanism at all. `-Full` is where they run, and
# `-Only laevatain_goo_toon` runs one.
# ---------------------------------------------------------------------------------------------
# WHERE THE CHARACTER SCENARIOS' ASSETS COME FROM, and the reason it is derived rather than absolute: these
# glbs and their `.toon.tsv` sidecars are LOCALLY AUTHORED assets that were never committed (the whole build
# directory is gitignored), so the only portable statement is "beside the build this script is gating".
# `laevatain_goo.glb.toon.tsv` is the file that carries the per-material rows the rewritten toon chain reads;
# `laevatain.glb` is the same character WITHOUT one, and that is what makes it the fallback's control.
$charDir = Join-Path $BuildDir "chars"

$scenarios = @(
    # `deferred` overrides NOTHING, so its reference IS the compiled defaults - the frame a stock
    # `config.toml` renders. Every other scenario is that frame plus the one difference its name claims,
    # which is why a key equal to its compiled default must never be spelled out here.
    @{ name = "deferred";          desc = "deferred G-buffer + lighting, no AA stage";   tier = "core";  extra = @{} }
    @{ name = "deferred_taa_fxaa"; desc = "deferred + TAA + FXAA (the AA path)";         tier = "extra"; extra = @{ taa = "true"; fxaa = "true" } }
    @{ name = "deferred_ssao_off"; desc = "deferred with SSAO disabled";                tier = "extra"; extra = @{ ssao = "false" } }
    @{ name = "shadow_single";     desc = "one cascade, i.e. the historic shadow path"; tier = "extra"; extra = @{ shadow_cascades = "1" } }
    @{ name = "unlit";             desc = "flat base colour, no shading";               tier = "core";  extra = @{ unlit = "true" } }
    # The one scenario that uses a different model, and it has to: alphaMode BLEND geometry is drawn
    # by a pass of its own, so no model without a BLEND material can exercise it - DamagedHelmet has
    # only OPAQUE/MASK. AlphaBlendModeTest carries one of each alphaMode (OPAQUE / MASK at two cutoffs
    # / BLEND) plus a decal, so this also covers the G-buffer's MASK discard path.
    @{ name = "transparent_blend"; desc = "deferred + an alphaMode BLEND material"; tier = "core"; extra = @{}
       model = "C:\Users\23530\Desktop\yzk\glTF-Sample-Assets\Models\AlphaBlendModeTest\glTF\AlphaBlendModeTest.gltf"
       camera = "0,5,12.4,0,-4.511,0" }
    # The heavy scene, and the only asset here whose interior exercises the cascaded shadows, the
    # clustered lights and the IBL together. The camera is pinned EXPLICITLY rather than left to
    # `camera_fit = "interior"` so the view does not move when the framing rule is tuned - and
    # `camera_fit` is how you get here interactively (the startup log prints the pose it resolves to).
    # taa = false for the same reason: TAA accumulates over frames and amplifies a small difference into
    # a different trail, which is the one kind of noise a visual comparison cannot have (measured the
    # hard way while adding object motion vectors). Sponza is a heavy load - 69 textures, ~150k
    # triangles - so this is the slow scenario.
    @{ name = "sponza";            desc = "Sponza interior, the heavy scene";           tier = "core";  extra = @{ taa = "false" }
       model = "C:\Users\23530\Desktop\yzk\glTF-Sample-Assets\Models\Sponza\glTF\Sponza.gltf"
       camera = "90,0,6.41,0,-18.548,0" }
    # THE MATERIAL SWEEP, the one asset that makes a material response visible: a grid from smooth metal
    # (a mirror) to rough dielectric, so a specular or BRDF change is a shape of the response rather than a
    # scene average. `camera = ""` is deliberate: this scenario lets the scene frame itself (see
    # Invoke-Scenario).
    @{ name = "metal_rough_glossy"; desc = "the material/roughness sweep";              tier = "extra";
       extra = @{ taa = "false"; camera_fit = "'exterior'" }
       model = "C:\Users\23530\Desktop\yzk\glTF-Sample-Assets\Models\MetalRoughSpheres\glTF\MetalRoughSpheres.gltf"
       camera = "" }
    # THE ONLY SCENARIO WHOSE CAMERA MOVES, and it is here because everything else was still: with a fixed
    # camera every reprojection in the renderer is exercised in the trivial case, so TAA's history, the
    # reflection history and the motion-vector target could all be broken without the harness noticing.
    # The sweep is 0.5 degrees of yaw per frame, which over the scenario's 40
    # frames is a 20-degree orbit - enough that a history fetched from the wrong place shows up, and slow
    # enough that the motion-vector path is in its normal range rather than its clamp. Same scene as
    # `metal_rough_glossy` on purpose, so the moving and still frames of one scene can be compared.
    @{ name = "glossy_motion"; desc = "the material sweep, CAMERA MOVING";              tier = "extra";
       extra = @{ taa = "false"; camera_fit = "'exterior'" }
       model = "C:\Users\23530\Desktop\yzk\glTF-Sample-Assets\Models\MetalRoughSpheres\glTF\MetalRoughSpheres.gltf"
       camera = ""
       sweep = "0.5" }
    # THE ONE SCENARIO WHOSE MESH DEFORMS, and it captures the MOTION CHANNEL rather than a shaded frame:
    # with TAA off, a wrong motion vector changes nothing a shaded frame would show, so the regression has
    # to BE the channel-8 image (gbuffer_debug draws the velocity amplified and biased so that "did not
    # move" is one flat value - see scripts/measure/motion_channel.py, which is how the before and after
    # were read). The model is the repository's own fixture: a plane skinned to two joints with its MESH
    # NODE STATIC and one joint pinned, so the rigid half of every motion vector is exactly zero and the
    # deformation is the ONLY motion in the frame. Before deformation-aware motion vectors the whole
    # swinging half reported "did not move": 229,267 px, ONE distinct value.
    @{ name = "deformation"; desc = "a skinned mesh DEFORMING (the motion channel)";    tier = "core";
       extra = @{ taa = "false"; gbuffer_debug = "true"; gbuffer_channel = "8" }
       model = "$repo\tests\fixtures\animated_skin_plane.gltf"
       camera = "0,0,4,0,0,0"
       animation_sweep = "0.02" }
    # THE REWRITTEN TOON CHAIN ON A CHARACTER (`[render] character_forward` + `[render] goo_toon`), and the
    # reason it exists is that the seven landed steps of that rewrite (iris / screen rim / base+skin+cloth+hair
    # diffuse / specular / face - see goo_toon_plan.md) were accepted by a HAND-RUN A/B: capture the arm, capture
    # the other arm, read the difference. That is not a net. This scenario is the same frame the A/B used, so
    # from here on a change to that chain has to move a RECORDED reference or it is not a change.
    #
    # `character_forward = true` IS PART OF THE SCENARIO AND NOT AN OPTION: with it off no material is drawn
    # through the character pipeline at all ([render] character_forward's own note), so `goo_toon` would select
    # nothing and the frame would be the deferred one - a green scenario that measures the wrong thing. The pose
    # is the A/B's own close pose, the one every step 2..7 reproduced exactly.
    #
    # THE ASSET IS THE ONE THAT CARRIES THE DATA: `laevatain_goo.glb` and its `.toon.tsv` sidecar, whose rows are
    # every per-material value the rewritten chain reads (the reference's own Goo node presets). It lives under
    # the BUILD directory because it is a locally-authored asset, not a committed fixture - see the note on
    # $charDir above - so a `-BuildDir` pointing elsewhere needs the same `chars\` beside it, and this scenario
    # then fails loudly ("no screenshot" / exit code) rather than passing green against a frame it never drew.
    @{ name = "laevatain_goo_toon"; desc = "the REWRITTEN toon chain on the goo character"; tier = "extra";
       extra = @{ character_forward = "true"; goo_toon = "true" }
       model = "$charDir\laevatain_goo.glb"
       camera = "-30.3668,-14.3239,0.6,0.0,-0.57,0.0" }
    # THE SAME ARM AT THE BODY POSE, and the reason is a MEASURED blind spot rather than a second opinion: the two
    # materials whose sidecar rows carry `_GooRSScalars` / `_GooRSTint` (mechanism table #14, `RS EFF`) are
    # `M_actor_laevat_cloth_02` and `M_actor_laevat_cloth_05`, and at the close pose above the chain draws NEITHER
    # of them - the step-13 mask arms prove it: `s13fin_white_close_r1` vs `s13fin_black_close_r1` (a full-white
    # `_M` vs a full-black `_M`) differ by 0 px there, so a change to the RS block could not move that reference.
    # At this pose those materials ARE on screen and the chain responds: the same white-vs-black mask A/B moves
    # 75 217 px (7.254726%, bbox x263..826 y549..959), and the shipped asset's real `_M` against a zeroed mask
    # moves 2 686 px (0.259066%, bbox (294,553)-(809,959)). Both numbers are transcribed from
    # `deren-ab/s13_v6_report.txt:14` / `:9` and were RECOMPUTED by the Lead from the on-disk frames
    # `deren-ab/laevat/s13fin_{white,black,on}_body_r1.png` (the first draft of this comment misattributed the
    # 2 686 to the white-vs-black pair; corrected 2026-10-01, see `deren-ab/goo_debt_lead_audit.md` §3-1).
    # That is what this scenario adds to the gate - a RECORDED frame the RS mechanism can actually break.
    @{ name = "laevatain_goo_toon_body"; desc = "the REWRITTEN toon chain at the BODY pose (the RS materials)"; tier = "extra";
       extra = @{ character_forward = "true"; goo_toon = "true" }
       model = "$charDir\laevatain_goo.glb"
       camera = "0,0,2.4,0,-0.9,0" }
    # THE FALLBACK ARM, and it is the negative control the chain's own contract asks for: `laevatain.glb` is the
    # SAME character with NO `.toon.tsv` sidecar, so every per-material value the rewritten chain reads is
    # ABSENT and the chain must come back to the reference's own socket defaults rather than guess (a zeroed or
    # stale lane read as a value). The two assets are the same geometry and the same 11 materials, measured with
    # this repository's own `deren-ab/glb_info.py`: the goo file adds only the extra IMAGES the sidecar's rows
    # resolve, and a copy of it with no sidecar beside it renders THIS scenario's frame exactly (92473A82…), so
    # the sidecar is the whole of the difference and not the textures.
    #
    # THE FALLBACK IS AN IDENTITY ON THIS ASSET, MEASURED: at this pose a sidecar-less `laevatain.glb` renders
    # `12A0A363…` with `goo_toon = true` AND with `goo_toon = false` (×2, 60 frames) - i.e. with nothing to
    # read the rewritten chain draws the old chain's frame, which is what "fall back rather than guess" means
    # for a pixel. That is also what makes this scenario DO something: the goo arm above is NOT that frame, so
    # a chain that went inert on the ASSET THAT HAS DATA is exactly what this pair detects.
    @{ name = "laevatain_no_sidecar"; desc = "the same character WITHOUT a toon sidecar (fallback)"; tier = "extra";
       extra = @{ character_forward = "true"; goo_toon = "true" }
       model = "$charDir\laevatain.glb"
       camera = "-30.3668,-14.3239,0.6,0.0,-0.57,0.0" }
    # THE OLD CHAIN ON THE SAME CHARACTER at the same pose, which is the arm the four byte-identity anchors are
    # taken on: `goo_toon = false` is the chain the rewrite must leave EXACTLY as it found it, so this scenario
    # is the recorded version of that criterion (the anchors are hand-run captures, re-verified at this commit;
    # this one runs in every -Full round). One variable separates it from `laevatain_goo_toon`: the chain.
    # NOTE that it is NOT the identity against the scenario above - measured, the two differ - because the
    # sidecar it reads is present, and that difference IS the rewrite's own output on a real character.
    @{ name = "laevatain_old_chain"; desc = "the OLD toon chain on the same goo character"; tier = "extra";
       extra = @{ character_forward = "true"; goo_toon = "false" }
       model = "$charDir\laevatain_goo.glb"
       camera = "-30.3668,-14.3239,0.6,0.0,-0.57,0.0" }
)

# A tier is REQUIRED, and the failure it prevents is a scenario that silently never runs: the default
# selection is `tier -eq "core"`, so a typo or a missing key would drop the scenario out of every
# default round while -List still showed it.
$tiers = @("core", "extra")
foreach ($s in $scenarios) {
    if ($tiers -notcontains $s.tier) {
        Write-Error "scenario '$($s.name)' has tier '$($s.tier)' - must be one of $($tiers -join ', ')"
        exit 1
    }
}

if ($List) {
    $coreCount = @($scenarios | Where-Object { $_.tier -eq "core" }).Count
    Write-Host "scenarios ($($scenarios.Count), of which $coreCount core - the default run):"
    foreach ($s in $scenarios) { "  {0,-26} {1,-6} {2}" -f $s.name, $s.tier, $s.desc }
    Write-Host "`n  -Full runs all $($scenarios.Count); -Only <name> runs one whatever its tier"
    Write-Host "`nreferences: $baseDir"
    exit 0
}

New-Item -ItemType Directory -Force -Path $workDir, $baseDir | Out-Null

function Write-ScenarioConfig {
    param([hashtable]$Scenario, [string]$Path)
    # Every scenario pins the settings that decide what the frame looks like OR whether it is
    # deterministic at all. [gui] show = false is the determinism prerequisite (see the header), and
    # max_fps/vsync are pinned so two runs of one scenario pace identically.
    #
    # A scenario's `extra` keys must not repeat a default line: TOML forbids a duplicate key and
    # toml++ rejects the whole file (which reads as "the scenario silently fell back to the
    # compiled-in defaults"). Hence the -notcontains filter below.
    $defaults = [ordered]@{
        "window_width"      = "$Width"
        "window_height"     = "$Height"
        "vsync"             = "false"
        "max_fps"           = "240"
        "shadow"            = "true"
        "validation_layers" = "true"
    }
    $scenarioModel = if ($Scenario.ContainsKey('model')) { $Scenario.model } else { $Model }
    # A TOML BASIC string (double quotes) with the backslashes escaped, which is the form make_config.py
    # writes and the form the escape belongs to. The single-quoted literal this used to write kept the
    # doubled backslashes as two literal characters, so every scenario's config said `C:\\Users\\...` and
    # the path only resolved because Win32 collapses a repeated separator (a UNC path would not have).
    $modelLine = "model = `"$($scenarioModel.Replace('\', '\\').Replace('"', '\"'))`""
    $lines = @(
        $modelLine,
        "grid_side = 0",
        "",
        "[paths]",
        "shaders_dir = ''",
        "model_dir = ''",
        "screenshot_dir = '$($workDir.Replace('\','\\'))'",
        "",
        "[render]"
    )
    foreach ($k in $defaults.Keys) {
        if ($Scenario.extra.ContainsKey($k)) { continue } # the scenario's value wins (see above)
        $lines += "$k = $($defaults[$k])"
    }
    foreach ($k in $Scenario.extra.Keys) { $lines += "$k = $($Scenario.extra[$k])" }
    $lines += @(
        "",
        "[gui]",
        "show = false",
        "",
        "[lighting]",
        "env_size = 256",
        "env_mip_count = 5",
        "irr_size = 32",
        "lut_size = 256"
    )
    Set-Content -Path $Path -Value ($lines -join "`n") -NoNewline
}

function Invoke-Scenario {
    param([hashtable]$Scenario)
    $cfg = Join-Path $workDir "$($Scenario.name).toml"
    Write-ScenarioConfig -Scenario $Scenario -Path $cfg
    Get-ChildItem $workDir -Filter "screenshot_*.png" -ErrorAction SilentlyContinue | Remove-Item -Force
    $log = Join-Path $workDir "debug.log"
    Remove-Item $log -Force -ErrorAction SilentlyContinue

    # NOT $args: that is PowerShell's automatic argument array, and assigning to it is the kind of
    # thing that works until it does not.
    #
    # An EMPTY scenario camera means "let the scene frame itself" (camera_fit), and the flag is then
    # omitted rather than passed empty: `--capture-camera=` with no numbers is not a valid pose, and one
    # scenario needs the scene's own framing - the metal/roughness sweep, whose grid a pinned pose would
    # have to be reverse-engineered for. camera_fit is deterministic for a static scene (the metadata
    # rule, not the clock), which is the same reason a scenario may pin one.
    $scenarioCamera = if ($Scenario.ContainsKey('camera')) { $Scenario.camera } else { $Camera }
    $launch = @("--config", $cfg, "--capture-frames", "$Frames")
    if ($scenarioCamera) { $launch += "--capture-camera=$scenarioCamera" }
    # A scenario may SWEEP the camera (`sweep` = degrees of yaw per presented frame). Every other scenario
    # holds it still, and a still camera exercises every reprojection path in the renderer - TAA's history,
    # the GI temporal accumulation, the reflection's history, the velocity target itself - only in the
    # trivial case where a motion vector is zero and the history comes from the pixel it left. A break in
    # any of them passed this harness. The sweep is frame-indexed rather than clock-driven, so a sweeping
    # scenario is as reproducible as a still one (the two-run determinism check below is what proves it).
    if ($Scenario.ContainsKey('sweep')) { $launch += "--capture-sweep=$($Scenario.sweep)" }
    # ... and a scenario may SWEEP THE ANIMATION instead (`animation_sweep` = seconds of animation per
    # presented frame), which is the same argument one level further in: a mesh that DEFORMS is the other
    # half of the reprojection story, and it cannot be reached by pinning the pose - a pinned pose uploads
    # the same skin matrices every frame, so the deformation term of every motion vector is exactly zero
    # and the frame cannot tell a deformation-aware renderer from one that ignores deformation. Also
    # frame-indexed, so it is exactly as reproducible as the camera sweep.
    if ($Scenario.ContainsKey('animation_sweep')) { $launch += "--capture-animation-sweep=$($Scenario.animation_sweep)" }
    # NO SHELL, for the reason `capture.ps1`'s launch gives at length: `Start-Process` is ShellExecuteEx, which is
    # where Windows runs its app-reputation check on an unsigned binary - and this gate launches the renderer TWICE
    # per scenario, so it was the largest single source of those prompts.
    $start = [System.Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $exe
    $start.WorkingDirectory = $workDir
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.WindowStyle = [System.Diagnostics.ProcessWindowStyle]::Hidden
    foreach ($arg in $launch) { $null = $start.ArgumentList.Add($arg) }
    $p = [System.Diagnostics.Process]::Start($start)
    # the capture exits on its own; the timeout is a safety net, not the expected path
    if (-not $p.WaitForExit(180000)) { $p.Kill(); return @{ ok = $false; why = "timed out" } }
    if ($p.ExitCode -ne 0) { return @{ ok = $false; why = "exit code $($p.ExitCode)" } }

    $shot = Get-ChildItem $workDir -Filter "screenshot_*.png" | Select-Object -First 1
    if (-not $shot) { return @{ ok = $false; why = "no screenshot produced" } }

    # Keep this run's image under a name that survives the NEXT run: the app writes a timestamped
    # screenshot_*.png and every scenario deletes those before it starts, so returning $shot.FullName
    # handed the caller a path that the second determinism run had already deleted - which is exactly
    # when the caller wants it (to save the diff of a CHANGED scenario).
    $kept = Join-Path $workDir "$($Scenario.name).last.png"
    Copy-Item $shot.FullName $kept -Force
    $shot = Get-Item $kept

    # the run has to have been VALIDATION clean: a difference in validation output is a finding on its
    # own, and a scenario that silently stopped being validation-clean should not be accepted as a
    # baseline either.
    #
    # [WARNING] is part of the pattern DELIBERATELY, and it was added after this hole hid a real defect:
    # the descriptor pool was missing the storage-image and acceleration-structure types its own set
    # layouts declared, so the layer named them on every single run - and every run was reported clean,
    # because the pattern only looked for errors and VUIDs. A warning from the layer is the layer saying
    # the application did something the specification does not allow; a driver that enforces it returns
    # VK_ERROR_OUT_OF_POOL_MEMORY instead of a frame. If a benign warning ever needs to be tolerated, it
    # belongs here as an explicit exception with its reason, not as a silent gap.
    if (Test-Path $log) {
        $bad = Select-String -Path $log -Pattern "VUID-|Validation Error|\[ERROR\]|\[WARNING\]|panic|recorded out of order"
        if ($bad) { return @{ ok = $false; why = "validation/log problems: $($bad[0].Line.Trim())" } }
    }
    return @{ ok = $true; path = $shot.FullName; hash = (Get-FileHash $shot.FullName -Algorithm SHA256).Hash }
}

# -Only takes a LIST (`-Only deferred,sponza`), and that is a fix rather than a convenience: it used to
# compare one name for equality, so `-Only a,b` matched NOTHING, ran nothing, and printed
# "changed : 0" - a green-looking summary from an empty run, which is how a broken build passed this
# script for several layers. An empty selection is now an error (see the summary below).
#
# An explicit -Only name beats the tier: naming a scenario is the whole point of asking for it.
$onlyNames = @()
if ($Only) { $onlyNames = @($Only -split '[,;\s]+' | Where-Object { $_ }) }
$pass = 0; $fail = 0; $missing = 0; $flaky = 0; $skipped = 0
foreach ($s in $scenarios) {
    if ($onlyNames.Count -gt 0) {
        if ($onlyNames -notcontains $s.name) { $skipped++; continue }
    } elseif (-not $Full -and $s.tier -ne "core") { $skipped++; continue }
    $ref = Join-Path $baseDir "$($s.name).png"
    Write-Host ("`n=== {0} ({1})" -f $s.name, $s.desc) -ForegroundColor Cyan

    $a = Invoke-Scenario -Scenario $s
    if (-not $a.ok) { Write-Host "  FAIL: $($a.why)" -ForegroundColor Red; $fail++; continue }
    if ($Update) {
        Copy-Item $a.path $ref -Force
        Write-Host "  reference updated ($($a.hash.Substring(0,16)))"
        $pass++
        continue
    }
    if (-not (Test-Path $ref)) {
        Write-Host "  NOT SEEDED: no reference at $ref" -ForegroundColor Yellow
        Write-Host "  run with -Update once to accept the current output as the reference"
        $missing++
        continue
    }

    # determinism first: two runs of the SAME binary must agree, or a mismatch below would be noise
    $b = Invoke-Scenario -Scenario $s
    if (-not $b.ok) { Write-Host "  FAIL (second run): $($b.why)" -ForegroundColor Red; $fail++; continue }
    if ($a.hash -ne $b.hash) {
        Write-Host "  FLAKY: two runs of this build differ ($($a.hash.Substring(0,16)) vs $($b.hash.Substring(0,16)))" -ForegroundColor Yellow
        Write-Host "  this scenario cannot be a regression check until it is deterministic"
        $flaky++
        continue
    }

    $refHash = (Get-FileHash $ref -Algorithm SHA256).Hash
    if ($a.hash -eq $refHash) {
        Write-Host "  ok  ($($a.hash.Substring(0,16)))" -ForegroundColor Green
        $pass++
    } else {
        Write-Host "  CHANGED: $($a.hash.Substring(0,16)) vs reference $($refHash.Substring(0,16))" -ForegroundColor Red
        $diff = Join-Path $workDir "$($s.name).actual.png"
        Copy-Item $a.path $diff -Force
        Write-Host "  current output kept at $diff"
        Write-Host "  if the change is intended: re-run with -Update"
        $fail++
    }
}

$ran = $pass + $fail + $flaky + $missing
$set = if ($onlyNames.Count -gt 0) { "-Only $($onlyNames -join ',')" } elseif ($Full) { "full" } else { "core" }
Write-Host "`n---------------- summary ----------------"
Write-Host "  set      : $set   (of $($scenarios.Count) defined)"
Write-Host "  defined  : $($scenarios.Count)   ran: $ran   skipped: $skipped"
Write-Host "  passed   : $pass"
Write-Host "  changed  : $fail"
Write-Host "  flaky    : $flaky"
Write-Host "  unseeded : $missing"
Write-Host "  references: $baseDir"
# AN EMPTY RUN IS A FAILURE, and this guard exists because the opposite was believed for several layers:
# `-Only a,b` matched no scenario, so the run compared nothing and still printed "changed : 0".
if ($ran -eq 0) { Write-Host "  ERROR: no scenario ran, so NOTHING was verified (check -Only / the scenario names)" -ForegroundColor Red; exit 1 }
if ($missing -gt 0) { Write-Host "  ERROR: $missing scenario(s) have no reference - run with -Update once to seed them" -ForegroundColor Red; exit 1 }
# A default (core) round says so, because "changed : 0" over five scenarios is NOT the same statement as
# "changed : 0" over all fourteen - the other nine only ran if -Full asked for them.
if (-not $Full -and $onlyNames.Count -eq 0 -and $skipped -gt 0) {
    Write-Host "  note     : $($skipped) extra scenario(s) NOT run - `-Full runs all $($scenarios.Count)" -ForegroundColor DarkGray
}
if ($fail -gt 0 -or $flaky -gt 0) { exit 1 }
exit 0
