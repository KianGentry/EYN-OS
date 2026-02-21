# Second Reality Demo (Userland)

This is a userland demo inspired by Second Reality. It focuses on real-time visuals rendered through the GUI syscall API and is designed to run as a .uelf program.

## Status

Work in progress. The demo is implemented in incremental steps so each stage is runnable and visually verifies new features.

## Step 1: Raster + Wireframe

Features implemented:
- Real-time 2D raster plasma effect using scanline rectangles
- 3D rotating wireframe cube with perspective projection

### Build

From the repository root:

```bash
./devtools/build_user_c.sh testdir/demo.c testdir/demo.uelf
```

### Run

From the EYN-OS shell:

```bash
run /testdir/demo.uelf
```

### Notes

- Rendering uses a RGB565 userland framebuffer with `gui_blit_rgb565`, plus GUI text overlay.
- Animation enables continuous redraw (`gui_set_continuous_redraw`) and uses a cooperative sleep (`usleep`) to yield for UI updates.

### Controls

- `Q`: Quit
- Space: Pause/Resume
- Left Arrow: Previous scene
- Right Arrow: Next scene

## Step 2: Bars + Palette Cycling + Starfield

Features implemented:
- Palette-cycled raster bars
- Starfield depth motion
- Filled cube faces with backface culling

## Step 3: Scene Timeline + Mesh Transition

Features implemented:
- Palette switches every 60 frames
- Bars hidden after 600 frames to reveal starfield
- Mesh transition from cube to pyramid

## Step 4: Textured Cube Scene

Features implemented:
- REI image texture (loaded from /icons16/file_rei.rei with fallback to /eynos.rei)
- Perspective-correct texture mapping on cube faces
- Starfield backdrop during textured scene

## Step 5: Filtering + Z Buffer + Rotozoom

Features implemented:
- Per-pixel z-buffer for textured triangle rendering (stabilizes face ordering and reduces artifacts)
- Bilinear texture sampling for the textured cube (reduces shimmer/jitter)
- Proper near-plane clipping for textured faces (prevents perspective warping from per-vertex z clamping)
- New rotozoom scene (classic demoscene texture warp)

## Planned Next Steps

- 3D scene transitions (multiple meshes, camera paths)
- 2D raster bars and palette cycling
- Sprite-based elements and starfield layers
- Optional precomputed meshes and spline movement for smoother motion
