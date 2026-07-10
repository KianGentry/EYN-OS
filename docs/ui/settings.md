# Settings App

The `settings` userland program opens a GUI settings window from shell or Start menu.

## Launch

```sh
settings
```

## Video Section

The Video section controls both hardware mode and compositor workspace profile.

- Hardware mode presets include:
  - `640x480 (4:3)`
  - `800x600 (4:3)`
  - `1024x768 (4:3)`
  - `1280x720 (16:9)`
  - `1280x800 (16:10)`

- Workspace scale: `100%`, `90%`, `75%`, `60%`, `50%`
- Aspect modes:
  - `Native`
  - `4:3`
  - `16:10`
  - `16:9`
  - `21:9`
  - `1:1`

Hardware mode changes the physical framebuffer resolution.
Workspace scale/aspect changes the logical desktop dimensions used by the tiling manager.

Keyboard controls in Video section:

- `Left/Right`: change scale/aspect values
- `Enter` on `Apply now`: apply live for current session
- `Enter` on `Save + apply`: apply and persist to `/config/ui.cfg`
- `A`: apply now
- `S`: save + apply

Mouse controls:

- Click `Video` / `Customization` tabs to switch sections
- Click rows to select and activate actions
- In value rows, click left/right half to decrement/increment option value
- Mouse wheel moves selection

## Customization Section

The Customization section exposes quick UI actions:

- Apply built-in font
- Apply `/fonts/unscii-16.hex`
- Launch `theme`
- Launch `fontpreview /fonts/unscii-16.otf`

## Persisted UI Config Keys

`/config/ui.cfg` includes:

- `workspace_scale=<50..100>`
- `workspace_aspect=<0..5>`
- `display_width=<pixels>`
- `display_height=<pixels>`
- `display_bpp=<16|24|32>`

Where `workspace_aspect` maps to:

- `0`: native
- `1`: 4:3
- `2`: 16:10
- `3`: 16:9
- `4`: 21:9
- `5`: 1:1
