Third-party sources fetched and built here (not committed):
- dxvk/ : DXVK 3.1.1 (zlib licence), github.com/doitsujin/dxvk, built natively with SDL3
  (meson: native_sdl3=enabled, only d3d9) and installed into dxvk-native/, with
  dxvk-soft-shadows.patch (committed) applied in dxvk/ (it covers its dxbc-spirv subproject too):
  a d3d9.shadowFilter setting that filters shadow-map sampling (option shadow_filter).
- SDL3-3.4.18/ : SDL3 (zlib licence), built as a shared library into sdl3/.
- dr_libs/dr_flac.h : dr_flac 0.12.42 by David Reid (public domain or MIT-0), committed; the
  runtime streams the Xbox soundtrack from FLAC with it (runtime/music.c).
