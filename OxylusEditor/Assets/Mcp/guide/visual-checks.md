# Visual checks

Look at the scene instead of guessing from component values. The viewport is rendered by the editor, so a screenshot shows exactly what a person sees there.

Screenshot workflow
1. Aim the editor camera: `view_camera_set {position:[0,3,8], look_at:[0,1,0]}` moves it instantly, with no smoothing. `view_focus {entity}` frames one entity the way the F shortcut does. `view_camera_get` reads the pose back.
2. `view_screenshot {max_size:1024}` returns a PNG. The caption says the real size, for example `scene 1, 1024x576, camera editor`. Bigger sizes cost more context, 512 to 1024 is enough for most checks.
3. Point at what you see: `view_pick {pixel:[x,y], image_size:[w,h]}` takes pixel coordinates from that screenshot and returns the entity under them, or nothing. `view_pick {uv:[u,v]}` takes 0 to 1 over the image, (0,0) is the top left.
4. Fix it with `entity_update`, then screenshot again to confirm.

Coordinates and cameras
- Pick coordinates refer to the viewport as it renders now, so pick against the camera of your latest screenshot and do not move the camera in between.
- Pass the size you got in the caption as `image_size`, not the size you asked for.
- A pick sees the scene geometry and sprites, not the grid, gizmos or icons. It does not change the selection.

Looking through scene cameras
- `view_screenshot {camera:"Level/CamA"}` shoots through a camera entity. The viewport looks through it for a few frames so smoothing and temporal effects settle, then returns to the editor camera.
- `view_camera_set` ends looking through a scene camera. While a cinematic is previewing, its panel owns the viewport camera and `camera` fails.

What a screenshot contains
- The rendered scene plus the editor grid and the selection highlight outline. Transform gizmos and component icons are drawn by the UI on top and are not captured, so `view_gizmo` changes nothing you can see in a PNG.
- Deselect with `editor_select` first if the highlight hides what you want to inspect.

Troubleshooting
- A timeout means the viewport is not rendering: hidden, minimized or on another tab. Focus it with `scene_focus`.
- A black or empty image usually means the camera is inside geometry or the scene has no light. Check `view_camera_get`.
