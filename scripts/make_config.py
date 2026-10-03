#!/usr/bin/env python3
"""Interactively generate a config.toml for deren.

Walks every startup setting with a per-key prompt: shows the type, the
default (press Enter to keep it), and validates the answer. Questions are
asked one at a time; every key carries a short hint from config.example.toml.
For a plain copy of the defaults, use make_default_config.py instead.

Usage:
    python scripts/make_config.py
    python scripts/make_config.py [output_dir]     # where config.toml is written
"""

from __future__ import annotations

import os
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_MODEL = "gltf_model/DamagedHelmet.gltf"  # relative to the renderer's cwd


# ---- small input helpers -------------------------------------------------

def ask_text(prompt: str, default: str, hint: str = "") -> str:
    suffix = f"  [{hint}]" if hint else ""
    while True:
        raw = input(f"{prompt}{suffix}\n> ").strip()
        if not raw:
            return default
        return raw


def ask_int(prompt: str, default: int, lo: int | None = None, hi: int | None = None, hint: str = "") -> int:
    suffix = f"  [{hint}]" if hint else ""
    while True:
        raw = input(f"{prompt} (int, default {default}){suffix}\n> ").strip()
        if not raw:
            return default
        try:
            value = int(raw)
        except ValueError:
            print(f"  not an integer: {raw!r}")
            continue
        if lo is not None and value < lo:
            print(f"  must be >= {lo}")
            continue
        if hi is not None and value > hi:
            print(f"  must be <= {hi}")
            continue
        return value


def ask_float(prompt: str, default: float, lo: float | None = None, hi: float | None = None, hint: str = "") -> float:
    suffix = f"  [{hint}]" if hint else ""
    while True:
        raw = input(f"{prompt} (float, default {default}){suffix}\n> ").strip()
        if not raw:
            return default
        try:
            value = float(raw)
        except ValueError:
            print(f"  not a number: {raw!r}")
            continue
        if lo is not None and value < lo:
            print(f"  must be >= {lo}")
            continue
        if hi is not None and value > hi:
            print(f"  must be <= {hi}")
            continue
        return value


def ask_bool(prompt: str, default: bool, hint: str = "") -> bool:
    label = "true/false"
    suffix = f"  [{hint}]" if hint else ""
    while True:
        raw = input(f"{prompt} ({label}, default {str(default).lower()}){suffix}\n> ").strip().lower()
        if not raw:
            return default
        if raw in ("t", "true", "y", "yes", "1", "on"):
            return True
        if raw in ("f", "false", "n", "no", "0", "off"):
            return False
        print(f"  expected true/false, got {raw!r}")


def ask_choice(prompt: str, choices: list[str], default: str, hint: str = "") -> str:
    suffix = f"  [{hint}]" if hint else ""
    shown = ", ".join(f"'{c}'" if c else "(empty)" for c in choices)
    while True:
        raw = input(f"{prompt}\n  one of: {shown}  (default '{default}')\n> ").strip()
        if not raw:
            return default
        if raw in choices:
            return raw
        print(f"  expected one of {shown}, got {raw!r}")


def ask_float3(prompt: str, default: tuple[float, float, float]) -> tuple[float, float, float]:
    while True:
        raw = input(f"{prompt} (3 floats, default {default[0]}, {default[1]}, {default[2]})\n> ").strip()
        if not raw:
            return default
        parts = raw.replace(",", " ").split()
        if len(parts) != 3:
            print("  expected three numbers, e.g. 0.02 0.02 0.03")
            continue
        try:
            values = tuple(float(p) for p in parts)
        except ValueError:
            print("  expected three numbers")
            continue
        if any(v < 0.0 or v > 1.0 for v in values):
            print("  channels must be in 0..1")
            continue
        return values  # type: ignore[return-value]


def ask_vec3(prompt: str, default: tuple[float, float, float], hint: str = "") -> tuple[float, float, float]:
    """Three unconstrained floats: positions and aim points are in metres, so unlike ask_float3 (colour
    channels) there is no 0..1 rule - the area light sits 15 m away and mostly above the scene."""
    suffix = f"  [{hint}]" if hint else ""
    while True:
        raw = input(f"{prompt} (3 floats, default {default[0]}, {default[1]}, {default[2]}){suffix}\n> ").strip()
        if not raw:
            return default
        parts = raw.replace(",", " ").split()
        if len(parts) != 3:
            print("  expected three numbers, e.g. -3 15 4")
            continue
        try:
            values = tuple(float(p) for p in parts)
        except ValueError:
            print("  expected three numbers")
            continue
        return values  # type: ignore[return-value]


def fmt_toml_string(s: str) -> str:
    # TOML basic strings: escape backslashes so Windows paths survive
    return s.replace("\\", "\\\\").replace('"', '\\"')


def write_toml(path: str, cfg: dict) -> None:
    lines: list[str] = [
        "# ============================================================",
        "# deren startup configuration (generated by",
        "#   scripts/make_config.py)",
        "# ============================================================",
        "",
        "# ---- top level: model to load ----",
        f"model = \"{fmt_toml_string(cfg['model'])}\"",
        "",
        f"grid_side = {cfg['grid_side']}",
        f"sun_intensity = {cfg['sun_intensity']}",
        "",
        "# ---- [paths] resource directories (empty = auto-locate) ----",
        "[paths]",
        f"shaders_dir = \"{fmt_toml_string(cfg['shaders_dir'])}\"",
        f"model_dir = \"{fmt_toml_string(cfg['model_dir'])}\"",
        "",
        "# ---- [render] window / presentation ----",
        "[render]",
        f"window_width = {cfg['window_width']}",
        f"window_height = {cfg['window_height']}",
        f"window_title = \"{fmt_toml_string(cfg['window_title'])}\"",
        f"vsync = {str(cfg['vsync']).lower()}",
        f"max_fps = {cfg['max_fps']}",
        "clear_color = [{0}, {1}, {2}]".format(*cfg["clear_color"]),
        f"camera_fit = \"{cfg['camera_fit']}\"",
        f"shadow = {str(cfg['shadow']).lower()}",
        f"validation_layers = {str(cfg['validation_layers']).lower()}",
        "",
        "# ---- [render] shadow mapping ----",
        f"shadow_cascades = {cfg['shadow_cascades']}",
        f"shadow_map_size = {cfg['shadow_map_size']}",
        f"shadow_cascade_blend = {cfg['shadow_cascade_blend']}",
        f"toon_shadow_softness = {cfg['toon_shadow_softness']}",
        "",
        "# ---- [render] shading + post-processing features ----",
        f"unlit = {str(cfg['unlit']).lower()}",
        f"taa = {str(cfg['taa']).lower()}",
        f"taa_blend_static = {cfg['taa_blend_static']}",
        f"taa_blend_min = {cfg['taa_blend_min']}",
        f"fxaa = {str(cfg['fxaa']).lower()}",
        f"render_scale = {cfg['render_scale']}",
        f"upscale = \"{cfg['upscale']}\"",
        f"gbuffer_debug = {str(cfg['gbuffer_debug']).lower()}",
        f"gbuffer_channel = {cfg['gbuffer_channel']}",
        f"gpu_timings = {str(cfg['gpu_timings']).lower()}",
        "",
        "# ---- [render] punctual lights + screen-space AO ----",
        f"clustered_lights = {str(cfg['clustered_lights']).lower()}",
        f"ssao = {str(cfg['ssao']).lower()}",
        f"ssao_radius = {cfg['ssao_radius']}",
        f"ssao_intensity = {cfg['ssao_intensity']}",
        f"ssao_samples = {cfg['ssao_samples']}",
        "",
        "# camera_pose = [yaw_deg, pitch_deg, distance, tx, ty, tz]   # a PINNED initial pose; absent\n"
        "#   leaves camera_fit in charge. F12 and exit print one (`camera pose: ...`) to paste back.\n"
        "# ---- [render] ray-traced effects ----",
        f"rt_shadows = {str(cfg['rt_shadows']).lower()}",
        f"rt_mask_bake = {str(cfg['rt_mask_bake']).lower()}",
        f"rt_skin_bake = {str(cfg['rt_skin_bake']).lower()}",
        f"animation_time = {cfg['animation_time']}",
        f"furnace = {str(cfg['furnace']).lower()}",
        "",
        "# ---- [gui] debug overlay ----",
        "[gui]",
        f"show = {str(cfg['gui_show']).lower()}",
        f"panel_width = {cfg['panel_width']}",
        f"panel_height = {cfg['panel_height']}",
        "",
        "# ---- [lighting] IBL precompute resolutions ----",
        "[lighting]",
        f"env_size = {cfg['env_size']}",
        f"env_mip_count = {cfg['env_mip_count']}",
        f"irr_size = {cfg['irr_size']}",
        f"lut_size = {cfg['lut_size']}",
        # QUOTED like `model` above: `fmt_toml_string` escapes the contents but the CALLER supplies the quotes,
        # and without them the empty default wrote `environment_hdr = ` - a line no TOML parser accepts, i.e.
        # the generator's own default output was an unreadable config (found while adding area_light_*).
        f"environment_hdr = \"{fmt_toml_string(cfg['environment_hdr'])}\"",
        f"environment_intensity = {cfg['environment_intensity']}",
        f"area_light_size = {cfg['area_light_size']}",
        f"area_light_power = {cfg['area_light_power']}",
        f"area_light_position = [{cfg['area_light_position'][0]}, {cfg['area_light_position'][1]}, {cfg['area_light_position'][2]}]",
        f"area_light_target = [{cfg['area_light_target'][0]}, {cfg['area_light_target'][1]}, {cfg['area_light_target'][2]}]",
        f"area_light_intensity = {cfg['area_light_intensity']}",
        f"area_light_irradiance = {str(cfg['area_light_irradiance']).lower()}",
        f"area_light_shadow = {str(cfg['area_light_shadow']).lower()}",
        f"area_light_softness = {cfg['area_light_softness']}",
        "",
    ]
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(lines))


def ask_all(output_dir: str) -> dict:
    print("== deren: interactive config ==")
    print("(press Enter on any question to keep its default)\n")

    print("-- model + grid --")
    sun_intensity = ask_float(
        "render.sun_intensity",
        1.0,
        0.0,
        3.0,
        hint="a scale on the sun's radiance; 1.0 leaves the shading path's constant unchanged",
    )
    model = ask_text(
        "model: glTF/GLB file to load",
        DEFAULT_MODEL,
        hint="empty = auto-locate DamagedHelmet.gltf; use forward slashes on Windows",
    )
    grid_side = ask_int("grid_side: instancing stress grid side", 0, 0, 90, hint=">1 draws a grid; 0 = off")

    print("\n-- paths (empty = auto-locate) --")
    shaders_dir = ask_text(
        "paths.shaders_dir: shader SPIR-V directory", "", hint="empty = find 'shaders/' upward from cwd"
    )
    model_dir = ask_text(
        "paths.model_dir: default model directory (for empty model)", "", hint="empty = find 'gltf_model/' upward"
    )

    print("\n-- render (window / presentation) --")
    window_width = ask_int("render.window_width", 1080, 1, hint="pixels; resizable anyway")
    window_height = ask_int("render.window_height", 960, 1, hint="pixels")
    window_title = ask_text("render.window_title", "deren")
    vsync = ask_bool("render.vsync", False, hint="false = Mailbox (uncapped), true = FIFO")
    max_fps = ask_int("render.max_fps", 0, 0, hint="0 = uncapped; e.g. 240 to match a 240 Hz panel")
    clear_color = ask_float3("render.clear_color (RGB 0..1)", (0.02, 0.02, 0.03))
    camera_fit = ask_choice(
        "render.camera_fit: initial framing", ["exterior", "interior"], "exterior",
        hint="exterior = fit the whole model from outside; interior = stand inside and look down the longest axis"
    )
    shadow = ask_bool("render.shadow", True, hint="record the directional shadow pass")
    validation_layers = ask_bool(
        "render.validation_layers", True, hint="Debug defaults on; Release off - override here if needed"
    )

    print("\n-- render (shadow mapping) --")
    shadow_cascades = ask_int(
        "render.shadow_cascades", 3, 1, 4, hint="1 = the single-map baseline; 2..4 = cascaded"
    )
    shadow_map_size = ask_int(
        "render.shadow_map_size", 2048, 256, 8192, hint="edge length in texels; per cascade layer"
    )
    shadow_cascade_blend = ask_float(
        "render.shadow_cascade_blend", 0.1, 0.0, 0.5, hint="fraction of a cascade's range blended into the next"
    )
    toon_shadow_softness = ask_float(
        "render.toon_shadow_softness",
        0.0,
        0.0,
        4.0,
        hint="toon chain shadow softness: 0 = shipped 3x3 PCF, 1..4 = wider kernels (softer edge, more light leak)",
    )

    print("\n-- render (shading + post-processing; every one of these is also a live overlay toggle) --")
    unlit = ask_bool("render.unlit", False, hint="flat base color instead of PBR (a shading-free reference)")
    taa = ask_bool(
        "render.taa", False, hint="temporal AA; the engine's anti-aliasing (a G-buffer cannot be multisampled)"
    )
    taa_blend_static = ask_float("render.taa_blend_static", 0.9, 0.0, 1.0, hint="history weight for a still pixel")
    taa_blend_min = ask_float("render.taa_blend_min", 0.5, 0.0, 1.0, hint="history weight floor under motion")
    fxaa = ask_bool("render.fxaa", False, hint="final anti-aliasing pass; costs nothing when off")
    render_scale = ask_float(
        "render.render_scale", 1.0, 0.1, 1.0, hint="fraction of the window the render chain runs at (1.0 = no scaling)"
    )
    upscale = ask_choice(
        "render.upscale: which filter resolves a scaled render chain",
        ["easu", "linear"],
        "easu",
        hint="easu = FSR 1's upscaler; linear = the bilinear reference it is measured against",
    )
    gbuffer_debug = ask_bool("render.gbuffer_debug", False, hint="show a stored G-buffer channel")
    gbuffer_channel = ask_int(
        "render.gbuffer_channel", 1, 0, 8, hint="0 albedo, 1 normal, 2 roughness, 3 metallic, 4 ao, 5 id, 6 depth, 7 flags, 8 motion"
    )
    gpu_timings = ask_bool("render.gpu_timings", True, hint="per-pass GPU timestamps (a no-op if the device cannot)")

    print("\n-- render (punctual lights + screen-space AO) --")
    clustered_lights = ask_bool(
        "render.clustered_lights", True, hint="false = the brute-force reference the clustered path is checked against"
    )
    ssao = ask_bool("render.ssao", True, hint="screen-space ambient occlusion")
    ssao_radius = ask_float("render.ssao_radius", 0.5, 0.0, 100.0, hint="world-space sample radius")
    ssao_intensity = ask_float("render.ssao_intensity", 1.0, 0.0, 1.0, hint="1 = full occlusion, 0 = off")
    ssao_samples = ask_int("render.ssao_samples", 8, 1, 16, hint="samples per pixel")

    print("\n-- render (ray-traced effects) --")
    furnace = ask_bool(
        "render.furnace",
        False,
        hint="VERIFICATION: sun off + a constant environment, so the correct frame is computable by hand",
    )
    rt_shadows = ask_bool(
        "render.rt_shadows",
        False,
        hint="ray-traced sun shadows (needs a device with ray queries; ignored elsewhere)",
    )
    animation_time = ask_float(
        "render.animation_time",
        -1.0,
        hint="pin a keyframe animation at N seconds (-1 = play it; playback is wall-clock driven, so only a "
             "pinned pose makes a capture of an animated scene reproducible)",
    )
    rt_mask_bake = ask_bool(
        "render.rt_mask_bake",
        False,
        hint="bake alphaMode MASK into the acceleration structures (an instrument: its per-triangle rule "
             "measured worse than the raster path, so it is off by default)",
    )

    rt_skin_bake = ask_bool(
        "render.rt_skin_bake",
        False,
        hint="re-skin animated casters and refit their acceleration structures every frame, so a traced "
             "shadow follows the animation instead of the bind pose (off by default: it is the A/B knob)",
    )

    print("\n-- gui (debug overlay) --")
    gui_show = ask_bool("gui.show", True, hint="Dear ImGui debug overlay on by default")
    panel_width = ask_int("gui.panel_width", 380, 0, hint="0 = ImGui auto-size")
    panel_height = ask_int("gui.panel_height", 140, 0, hint="0 = ImGui auto-size")

    print("\n-- lighting (IBL precompute resolutions; lower = faster startup) --")
    # Bounds mirror app_config::load_settings' clamps exactly: a generator that could emit a value the
    # loader then rejects would be a trap, and these four feed the CPU bake that runs before anything
    # validates them (env_size = 0 indexes an empty source buffer, a negative one dies on the size
    # arithmetic, env_mip_count < 2 wraps the sampler's level math).
    env_size = ask_int("lighting.env_size", 256, 16, 4096, hint="environment cubemap size")
    env_mip_count = ask_int("lighting.env_mip_count", 5, 2, 12, hint="prefiltered env mip chain length")
    irr_size = ask_int("lighting.irr_size", 32, 1, 1024, hint="irradiance cubemap size")
    lut_size = ask_int("lighting.lut_size", 256, 1, 1024, hint="BRDF LUT size")
    # environment_hdr: an equirectangular .hdr used as the IBL instead of the procedural sky. It is a
    # PATH, so it must survive the round trip verbatim - write_toml quotes it - and a missing file is a
    # hard startup error rather than a fallback, which is why the default is the empty string (the
    # procedural sky) rather than a path that may not exist on this machine.
    environment_hdr = ask_text("lighting.environment_hdr", "", hint="equirect .hdr path ('' = the procedural sky)")
    environment_intensity = ask_float(
        "lighting.environment_intensity", 0.35, 0.0, hint="multiplier on the loaded HDR (the reference's world_strength)"
    )
    # area_light_*: the reference package's 30 m soft box, expressed through the frame's ONE directional light
    # (see config.example.toml and app_config::derive_area_light). The DEFAULT IS SIZE 0 = off, and the
    # generator must be able to write that off state, so 0 is a legal answer to the first question. The
    # manifest's own numbers are size 30 / power 4000 / position [-3, 15, 4] (Blender's [-3, -4, 15] turned
    # Y-up) aiming at [0, 0, 0], and position/target are measured from the AUTHOR'S ORIGIN - the character's
    # feet on the ground - not from the scene's bounding-box centre; `irradiance` decides whether that emitter
    # IS the main light (its direction and radiance drive the sun) or contributes only its penumbra, `shadow`
    # is the second A/B switch and `softness` 0 = the automatic penumbra. None of these bounds is enforced here
    # beyond "not negative": the loader is the one place that sanitizes (an infinity or a NaN written here is
    # caught there, like every other key).
    area_light_size = ask_float("lighting.area_light_size", 0.0, 0.0, hint="square emitter side in metres (0 = no area light)")
    area_light_power = ask_float("lighting.area_light_power", 0.0, 0.0, hint="total power in watts")
    area_light_position = ask_vec3(
        "lighting.area_light_position", (0.0, 0.0, 0.0), hint="emitter centre, RELATIVE TO THE AUTHOR ORIGIN (the feet)"
    )
    area_light_target = ask_vec3(
        "lighting.area_light_target", (0.0, 0.0, 0.0), hint="aim point, same frame as the position"
    )
    area_light_intensity = ask_float(
        "lighting.area_light_intensity", 1.0, 0.0, hint="multiplier on the emitter's radiance (clipped at 3.0 by sun_intensity)"
    )
    area_light_irradiance = ask_bool(
        "lighting.area_light_irradiance",
        True,
        hint="true = the emitter takes over the main light (direction + radiance); false = penumbra only",
    )
    area_light_shadow = ask_bool(
        "lighting.area_light_shadow", True, hint="give the shadow a penumbra (false = the emitter does not touch it)"
    )
    area_light_softness = ask_float(
        "lighting.area_light_softness", 0.0, 0.0, hint="penumbra world radius in metres (0 = automatic)"
    )

    print(f"\nwriting config.toml to: {output_dir}")
    return {
        "model": model,
        "sun_intensity": sun_intensity,
        "grid_side": grid_side,
        "shaders_dir": shaders_dir,
        "model_dir": model_dir,
        "window_width": window_width,
        "window_height": window_height,
        "window_title": window_title,
        "vsync": vsync,
        "max_fps": max_fps,
        "clear_color": clear_color,
        "camera_fit": camera_fit,
        "shadow": shadow,
        "validation_layers": validation_layers,
        "shadow_cascades": shadow_cascades,
        "shadow_map_size": shadow_map_size,
        "shadow_cascade_blend": shadow_cascade_blend,
        "toon_shadow_softness": toon_shadow_softness,
        "unlit": unlit,
        "taa": taa,
        "taa_blend_static": taa_blend_static,
        "taa_blend_min": taa_blend_min,
        "fxaa": fxaa,
        "render_scale": render_scale,
        "upscale": upscale,
        "gbuffer_debug": gbuffer_debug,
        "gbuffer_channel": gbuffer_channel,
        "gpu_timings": gpu_timings,
        "clustered_lights": clustered_lights,
        "ssao": ssao,
        "ssao_radius": ssao_radius,
        "ssao_intensity": ssao_intensity,
        "ssao_samples": ssao_samples,
        "rt_shadows": rt_shadows,
        "rt_mask_bake": rt_mask_bake,
        "rt_skin_bake": rt_skin_bake,
        "animation_time": animation_time,
        "furnace": furnace,
        "gui_show": gui_show,
        "panel_width": panel_width,
        "panel_height": panel_height,
        "env_size": env_size,
        "env_mip_count": env_mip_count,
        "irr_size": irr_size,
        "lut_size": lut_size,
        "environment_hdr": environment_hdr,
        "environment_intensity": environment_intensity,
        "area_light_size": area_light_size,
        "area_light_power": area_light_power,
        "area_light_position": area_light_position,
        "area_light_target": area_light_target,
        "area_light_intensity": area_light_intensity,
        "area_light_irradiance": area_light_irradiance,
        "area_light_shadow": area_light_shadow,
        "area_light_softness": area_light_softness,
    }


def main() -> int:
    if len(sys.argv) > 1:
        output_dir = os.path.abspath(sys.argv[1])
    else:
        output_dir = REPO_ROOT  # the renderer's default cwd

    if not os.path.isdir(output_dir):
        try:
            os.makedirs(output_dir, exist_ok=True)
            print(f"(created directory {output_dir})")
        except OSError as exc:
            print(f"cannot create {output_dir}: {exc}", file=sys.stderr)
            return 1

    dest = os.path.join(output_dir, "config.toml")
    if os.path.exists(dest):
        answer = input(f"{dest} already exists - overwrite? [y/N] ").strip().lower()
        if answer not in ("y", "yes"):
            print("aborted - nothing written.")
            return 1

    cfg = ask_all(output_dir)
    write_toml(dest, cfg)
    print(f"wrote {dest}")
    print("run the renderer from that directory, or point at it with --config config.toml")
    return 0


if __name__ == "__main__":
    sys.exit(main())
