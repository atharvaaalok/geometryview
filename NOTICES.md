# Third-party notices

GeometryView is MIT licensed (see [LICENSE](LICENSE)). It builds on the work below, each under its
own license. Versions are pinned in [ThirdParty/manifest.toml](ThirdParty/manifest.toml).

The showcase models ([`Tools/showcase`](Tools/showcase), and the folder and sample files made from
them) and the README's pictures are GeometryView's own, under the same MIT license.

## Shipped in the app, the browser viewer and the launcher

| Project | License | How it is used |
|---|---|---|
| [Open CASCADE Technology](https://github.com/Open-Cascade-SAS/OCCT) (OCCT) 8.0.1 | LGPL-2.1 with the OCCT exception 1.0 | Reads and meshes STEP files: in the Mac app's import helper and in the browser viewer's importers. It is not changed. `ThirdParty/build_occt.sh` and `ThirdParty/build_occt_wasm.sh` rebuild it from its source, so you can relink GeometryView with your own build of it. |
| [meshoptimizer](https://github.com/zeux/meshoptimizer) 1.3 | MIT | Meshlets and their bounds. Vendored, with its [license](ThirdParty/meshoptimizer/LICENSE.md). |
| [xxHash](https://github.com/Cyan4973/xxHash) 0.8.3 | BSD-2-Clause | Content hashes, to tell files and repeated shapes apart. Vendored, with its [license](ThirdParty/xxhash/LICENSE). |
| [IBM Plex Mono](https://github.com/IBM/plex) 2.5.0 | SIL Open Font License 1.1 | The interface's typeface. Vendored, with its [license](ThirdParty/ibm-plex-mono/LICENSE.txt). |
| [Dawn](https://dawn.googlesource.com/dawn)'s emdawnwebgpu | BSD-3-Clause | The WebGPU bindings compiled into the browser viewer's engine. |
| [Emscripten](https://emscripten.org) runtime and libraries | MIT / University of Illinois NCSA; libc++ under Apache-2.0 with LLVM exceptions; musl under MIT | Compiled into the browser viewer's WebAssembly. |

The figure palette and the see-through look follow [Colorfigures](https://april-zhu.com/colorfigures)
by April Zhu, under the MIT license:

```
Copyright (c) 2026 April Zhu (Colorfigures: colour, hand-drawn style, 3D props, template, examples)
Copyright (c) 2026 April Zhu (iso-glow, on which Colorfigures is based)
Copyright (c) 2026 Tolga Cohce (iso-figure, on which iso-glow is based)

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## Used to build and test only

| Project | License |
|---|---|
| [doctest](https://github.com/doctest/doctest) 2.4.12 | MIT; vendored with its [license](ThirdParty/doctest/LICENSE.txt) |
| [Dawn](https://dawn.googlesource.com/dawn) (native) | BSD-3-Clause; the WebGPU test harness on the Mac |
| [build123d](https://github.com/gumyr/build123d) 0.13 and [OCP](https://github.com/CadQuery/OCP) 8.0.1 | Apache-2.0; they write the showcase models' STEP files |
