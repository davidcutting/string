String engine - Windows x64
===========================

Run string_demo.exe. Everything it needs is in this folder.

REQUIREMENTS
  Windows 10/11 x64, and up-to-date GPU drivers (the engine uses Vulkan; the
  driver provides vulkan-1.dll). Nothing to install.

VIEWING YOUR OWN SCENE
  Put a .gltf/.glb (with its .bin and textures) in    assets\
  then launch and pick it from the Scene menu. The engine cooks it on first
  use - that takes a while the first time and is instant afterwards.
  The scene list being empty just means assets\ has no models yet.

CONTROLS
  WASD + mouse   fly the camera
  Esc            release the mouse for the UI
  `              console

TROUBLESHOOTING
  Fails to start          -> update your GPU drivers.
  "cannot open ...ttf"    -> the folder is incomplete; re-extract the whole zip.
