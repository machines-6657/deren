# README screenshots

`DamagedHelmet.png` and `FlightHelmet.png` are the renderer's default configuration **as it stood when they
were captured** - traced global illumination with shaded hits (`ssgi_ray_tracing` and `ssgi_hit_shading`,
both on by default then), SSAO, TAA, cascaded shadows and the IBL prefilter, at 1080x960. That traced-GI
stage has since been removed (`1162d88`), so the current default configuration would not reproduce these
frames. They are CAPTURES rather than hand-taken screenshots: each one is a fixed config plus a fixed frame
count, so it can be regenerated and compared instead of being replaced by whatever the next session's window
happened to show.

They were produced with the instruments in `scripts/`, from a base config that is the generator's default
output with `[gui] show = false` (a debug overlay in a README image is noise) and `taa = true` (a still
frame that has accumulated its history, which is what the engine looks like in use):

    pwsh -File scripts/windows/capture.ps1 -Base snapshot_base.toml -Tag rc_helmet \
         -Model <repo>/gltf_model/DamagedHelmet.gltf -Camera "" -Frames 120
    pwsh -File scripts/windows/capture.ps1 -Base snapshot_base.toml -Tag rc_flight \
         -Model <assets>/FlightHelmet/glTF/FlightHelmet.gltf -Camera "" -Frames 120

`-Camera ""` is deliberate: it leaves the pose to the scene's own `camera_fit`, which for a static scene is
deterministic (the run prints the pose it resolved to). The captures land in `<BuildDir>/gi-probe/`.

The renderer's own PNG writer stores the frame essentially uncompressed (4.1 MB for a 1080x960 RGBA frame),
so the captures were re-encoded LOSSLESSLY with Pillow before being committed - the pixels are the
renderer's, byte for byte (checked with `ImageChops.difference`), and only the deflate stream differs. That
is a 13x reduction: 4,148,543 -> 322,611 bytes (DamagedHelmet) and -> 290,968 (FlightHelmet).

## CharacterForward.png

`CharacterForward.png` (1197x1494) is the exception to the rule above: a HAND-TAKEN screenshot of the
character-forward pipeline on a posed anime model, not a reproducible capture. The model, the pose and the
camera come from outside this repository, and the session's `toon shading` combo was at `off (plain pbr)`,
so this is the pipeline with the smooth shading response - the banded cel looks are that combo's other
values, and a banded example would have to be captured on purpose.

It was cropped from a 2560x1600 desktop screenshot (ShareX, 2026-10-02). The ImGui debug overlay, the window
title bar and the Windows taskbar are cut away, leaving the render alone; the client-area edges were measured
rather than guessed (`build-release-clang64/docs_review/toon_shot_final.py`, which finds the overlay's right
edge from its text density and the taskbar's top edge from the first row that is dark across the whole
width). Pixels inside the crop are the renderer's own; the crop was re-encoded with Pillow
(`optimize=True`) and NOT resampled.

