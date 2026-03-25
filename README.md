# Vulkan Renerer

This is a starting point for a larger project that I am embarking upon to create
a high quality 3D engine and game for personal use.

## Known Issues

### Defaulting to llvmpipe
- Occurs on some Linux distributions / windowing environments (e.g., Ubuntu + Gnome)
- Vulkan fails to identify graphics card and defaults to software rendering
- Fix by updating configs to enable "DRI 3" (e.g., in Xorg settings)

### Wayland Flickering
- Occurs on some wayland based compositors (e.g., SwayWM)
- Screen flickers and tears when put in fullscreen mode
- Avoid by manually resizing window to fill screen, instead of fullscreening it 
- Avoid by using a different compositor / windowing environment

## Todo
- [ ] Get build working on Windows
