Third-party sources fetched and built here (not committed):
- dxvk/ : DXVK 3.1.1 (zlib licence), github.com/doitsujin/dxvk, built natively with SDL3
  (meson: native_sdl3=enabled, only d3d9) and installed into dxvk-native/.
- SDL3-3.4.18/ : SDL3 (zlib licence), github.com/libsdl-org/SDL, built as a shared library and
  installed into sdl3/.
- UELib/, dotnet/, umodel/ : optional reading tools for the game's packages (DESIGN.md section
  7); the build doesn't need them.
- dr_libs/dr_flac.h : dr_flac 0.12.42 by David Reid (public domain or MIT-0), committed with the
  shared runtime this port started from; nothing in this port uses it yet.
