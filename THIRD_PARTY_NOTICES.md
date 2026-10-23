# Third-party notices

Ebook Reader is licensed under the AGPL-3.0 and includes the following components.

| Component | Version | License | Source |
|---|---|---|---|
| MuPDF | 1.28.5 | AGPL-3.0 | https://mupdf.com · `third_party/mupdf` |
| MuPDF bundled libraries (freetype, harfbuzz, gumbo, jbig2dec, lcms2, libjpeg, openjpeg, zlib, brotli, …) | as pinned by MuPDF | various permissive / FTL / MIT / BSD | `third_party/mupdf/thirdparty` |
| libmobi | 0.12 | LGPL-3.0-or-later | https://github.com/bfabiszewski/libmobi · `third_party/libmobi` |
| miniz (inside libmobi) | as pinned | MIT | `third_party/libmobi/src/miniz.c` |
| SQLite | 3.54.1 | Public domain | https://sqlite.org · `third_party/sqlite` |
| doctest (tests only, not shipped) | 2.5.3 | MIT | https://github.com/doctest/doctest · `third_party/doctest` |

Fonts, Piper/onnxruntime/espeak-ng and voice models will be listed here when they are added.

The full license texts are shipped in the `licenses\` folder of release packages. Because the complete source code
of Ebook Reader (including the exact library versions above) is published in this repository, the source-availability
requirements of the AGPL-3.0 and LGPL-3.0 are met.
