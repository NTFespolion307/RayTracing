VkGigaTracer bundled UI fonts
=============================

The Material 3 ("Google style") UI prefers Roboto. Drop these two files into
this folder to get the authentic look:

    Roboto-Regular.ttf   (body / UI text)
    Roboto-Medium.ttf    (headers, e.g. the "VkGigaTracer" title)

Where to get them
------------------
Roboto is licensed under the Apache License 2.0 and is free to redistribute.
Download the static TTFs from either:

  * Google Fonts:        https://fonts.google.com/specimen/Roboto  (Download family)
  * google/fonts on GitHub:
        apache/roboto/static/Roboto-Regular.ttf
        apache/roboto/static/Roboto-Medium.ttf

Place the .ttf files directly in this directory (next to this README).

How they are used
-----------------
* CMake copies this whole folder next to the built VkGigaTracer.exe on each
  build (see the POST_BUILD step in CMakeLists.txt).
* At startup theme::loadFonts() looks for fonts/Roboto-*.ttf beside the exe.
* If the files are missing, the UI automatically falls back to Segoe UI
  (a very close geometric match) and, failing that, ImGui's built-in font.
  The application runs identically either way - only the typeface differs.

These fonts were intentionally NOT committed as binaries; add them locally (or
to your release artifacts) to enable Roboto.
