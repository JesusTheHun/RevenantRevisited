# Window and HUD scaling

The game HUD is authored in 640x480 coordinates. Its artwork scales
proportionally with the window height; wide windows add horizontal game-view
space. Narrow windows fit the width so the HUD remains accessible.

`Display.Width()` and `Height()` are game-canvas coordinates. Swapchain size
(`sapp_width()` and `sapp_height()`) is the physical presentation size. Mouse
input passes through the inverse of the canvas-to-window transform, so drawn
controls and their hit regions agree after a resize. The editor uses physical
window coordinates.

Aspect changes recreate only render targets and the 2D backbuffer, preserving
loaded assets, shader pipelines and held input. Pending GPU picks from an old
target generation are drained and discarded.
