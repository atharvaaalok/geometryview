# Building and developing GeometryView

Everything here builds from a clone of this repository, on a Mac. The [README](../README.md) covers
using GeometryView; this covers building each part, testing it and releasing it.

## Requirements

- macOS 26 or later, on Apple silicon
- Xcode 27, with the Metal Toolchain component (Xcode ▸ Settings ▸ Components)
- CMake, Ninja and LLVM: `brew install cmake ninja llvm`
- Node.js 20 or later, for the launcher and the AI tools
- For the browser viewer: Emscripten 7.0 (`brew install emscripten`)
- For the iPhone shell: XcodeGen (`brew install xcodegen`)

**OCCT**, which reads and meshes STEP files, is built from source once at the pinned version:
`ThirdParty/build_occt.sh` for the Mac (about five minutes) and `ThirdParty/build_occt_wasm.sh` for
the browser. Without it everything else builds, and STEP files don't open.

## The Mac app

```sh
cmake --preset release
cmake --build --preset release
cmake --build --preset release --target install-app    # puts GeometryView in /Applications
```

`install-app` copies the app to `/Applications` and registers it, so it opens from Spotlight,
Launchpad or the Dock, and double-clicking a STEP or STL file opens it (unless you chose another
app for those files). Run it again after a build to update the installed copy. From a terminal,
`open -a GeometryView part.step` opens a file in it.

Every preset builds its own copy of the app, and Spotlight lists each one beside the installed app.
Spotlight skips folders whose names end in `.noindex` (it ignores `.metadata_never_index`), so keep
the build tree in one and link to it:

```sh
mv build build.noindex && ln -s build.noindex build
mv iOS/Shell/build iOS/Shell/build.noindex && ln -s build.noindex iOS/Shell/build
```

Every path stays the same. A copy you have opened is also offered under Open With; `lsregister -u`
(in `/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support`)
with its path takes it off.

## The browser viewer

```sh
cmake --build --preset wasm-webgpu     # the engine
cmake --build --preset wasm-release    # the STEP importers
cmake --build --preset wasm-threads    # the threaded importer
cd Launcher && npm link && cd ..       # once: puts the geometryview command on your PATH
geometryview                           # serves the viewer on 127.0.0.1 and opens your browser
geometryview part.step                 # the same, with a file open
geometryview --no-open [part.step]     # serves it and prints its address, for any browser
```

The viewer is always at http://127.0.0.1:47700, so the browser keeps its library and look from one
run to the next. Running `geometryview` while it already runs just opens the browser there; Ctrl-C
stops it. `--port N` chooses another port. While it runs, the browser can install the page as an
app window (Chrome's Install button, or Safari's File ▸ Add to Dock).

The hosted copy is the same files with sample models: `node Launcher/scripts/site.mjs` assembles it
in `Launcher/dist/site` and zips it. The samples are the showcase models, which
`Tools/showcase/make.py` writes into `build/showcase` (see "The README's pictures").

## The AI tools, from a checkout

The launcher is also the tool server (MCP). From a checkout, point an AI app at it:

```sh
claude mcp add geometryview -- node "$PWD/Launcher/bin/geometryview.mjs" --mcp
```

Apps started from the Dock don't see your shell's PATH, so give them the full path to `node`
(`which node` prints it). With the release preset built, a Mac uses the native engine (`gcli`) for
pictures and the GeometryView app to show models; anywhere else the tools open the viewer in the
browser and work through it, so the person sees what the AI looks at.
`GEOMETRYVIEW_ENGINE=page` makes a Mac use the browser's viewer too.

`node Launcher/scripts/pack.mjs` packs `Launcher/dist/geometryview.mcpb` for Claude Desktop, with
this Mac's native engine; `--portable` leaves the engine out, for a bundle that runs anywhere.

## The iPhone and iPad shell

The shell is the browser viewer's files in a WKWebView, working offline, opening files from Files
and the share sheet. Build the three WebAssembly presets above, then:

```sh
cd iOS/Shell
xcodegen                         # makes GeometryViewShell.xcodeproj, which is not committed
open GeometryViewShell.xcodeproj
```

In Xcode, choose your team under Signing & Capabilities, change the bundle identifier to one of
your own, and run it on your device.

## Tests

```sh
cmake --preset debug && cmake --build --preset debug
ctest --preset debug                      # unit, GPU, golden-image and layering tests
cd Launcher && npm test                   # the launcher, and the tools driven as an AI app drives them
GEOMETRYVIEW_E2E=1 node --test test/page.test.mjs   # the viewer in Chrome, end to end
```

The `asan` and `tsan` presets run the same tests under sanitizers, and the `wasm-release` and
`wasm-threads` presets have their own `ctest` suites. Tests that need a model look in `test-models/`
(or `GEOMETRYVIEW_FIXTURES`), and skip when it is missing.

`Tests/golden` holds an image of every style and colour mode. After a deliberate change to how
things look, `GEOMETRYVIEW_UPDATE_GOLDENS=1 build/debug/Tests/geo_tests -tc="*golden*"` rewrites
them, to be checked by eye before committing.

`Tools/perf_gate.py build/release` imports the fixtures and fails if any got more than 5% slower
than `Tools/perf_baseline.json`; `--update` records a new baseline. `Tools/fuzz.sh 600` fuzzes the
STEP tokenizer, the STL reader and the cache reader for ten minutes each.
`Tools/step_equivalence.py build/release files.txt --jobs 6` compares STEP imports with OCCT
reading each file whole, on face count, area, volume and bounds.

## Tools

`gcli` imports, checks and renders files without the app:

```sh
build/release/Tools/gcli import part.step part.geoblob build/release/Import/geometryview-occt-worker
build/release/Tools/gcli stats part.geoblob
build/release/Tools/gcli render build/release/shaders/default.metallib part.geoblob part.png \
    1600 1000 35 25 ortho 1 0 x0.5 plain   # size, view, projection, style, colour, section
```

A section is `x0.5` or `x0.6,y-0.4` (a minus removes the low side), and `plain` leaves out the view
cube and scale bar.

The app has switches for measuring and checking it:

- `GEOMETRYVIEW_TRACE=1` prints stage timings, memory use and GPU time per frame.
- `GEOMETRYVIEW_LATENCY=1` measures input-to-present latency.
- `GEOMETRYVIEW_ALLOCS=1` counts per-frame heap allocations.
- `--exercise` runs a scripted drag, hover and click on the opened file, then checks that changing
  only the interface draws no 3D frame.
- `--chrome-check` clicks every segment and card label with real events, and checks none reaches
  the 3D view; then that the tree's buttons work, Esc closes cards and the masthead's button closes
  the file. Give it `--card structure`, so the tree is showing, and launch it through `open`, which
  lets the sandboxed app read the file: `open -n -W --stdout out.txt -a
  build/release/App/GeometryView.app part.step --args --chrome-check --card structure`.
- `--snapshot` writes the window to `chrome.png` in the app's temporary directory; `--window WxH`
  sizes the window, `--card structure` shows the opened file's structure, and `--plain` leaves out
  the view cube and scale bar. In the browser viewer, `?plain` does the same.

`python3 Tools/media/app_icon.py` draws the app icon's master, `App/Resources/AppIcon.png`, from the
showcase's nesting dolls; the Mac app's icon is made from it as the app builds, and
`Tools/make_icons.sh --web` and `--ios` remake the web viewer's and the iPhone shell's.

## The README's pictures

Every picture in the README is drawn by GeometryView from its own showcase models, which
`Tools/showcase/make.py` writes (its header says how; they need build123d). Then:

```sh
python3 Tools/media/readme_media.py capture-gallery capture-phone capture-mac   # the screens
python3 Tools/media/readme_media.py --out Docs/media                            # the pictures
```

The captures need a release build and the browser engine (`wasm-webgpu`); `capture-mac` exports
the app's settings first, imports them after, and takes the build off Launch Services. Without
`--out` the pictures go to `build/media/readme`. Pictures come in light and dark twins of one
size, at twice the README's 880 px column.

`node Tools/media/readme_board.mjs <folder>` shows README drafts as GitHub renders them (through its
Markdown API, with github-markdown-css), in light and dark, at a desktop's width and a phone's, with
the npm page and each draft's weight. Its header says what the folder holds.

## Releasing

1. `node Launcher/scripts/pack.mjs --portable`, then `node Launcher/scripts/site.mjs` (packing
   empties `Launcher/dist`), for `geometryview.mcpb` and `geometryview-web.zip`; and
   `Tools/showcase/make.py release` for `things-that-come-in-sizes.zip`, the folder the README
   offers.
2. A GitHub release with those three files as assets. Publishing it runs the Pages workflow
   (`.github/workflows/pages.yml`), which deploys the zip as the hosted viewer. The
   `github-pages` environment allows the `main` branch and `v*` tags, the release's.
3. `cd Launcher && npm publish`, after bumping the version in `package.json` and
   `mcpb/manifest.json` and running `node Launcher/scripts/readme.mjs`, which pins the npm page's
   pictures to the new tag. Publishing first checks that every link on that page answers, so the
   tag must be pushed. Run it in a terminal of your own: npm asks for two-factor approval there.
4. With a Developer ID: `Tools/release/mac_release.sh` signs, notarizes and zips the Mac app and a
   native `.mcpb`, for the same release, as `GeometryView-mac.zip` and `geometryview-mac.mcpb`
   (the README links to the latest release's by name). Its header says how to set it up; `--check`
   tries everything but notarizing. The first signing asks for the login keychain's password.
