# ADR 0011: A lazy library, editable bindings, and a cache with a budget

Status: accepted. M7's exit criteria are met, with one verified only by design; see below.

## Decisions

**The library lists folders, never files.** Connected folders are security-scoped bookmarks in
the user's defaults. They are resolved at launch with security scope, so the sandboxed app keeps
access across relaunches. The tree is an outline of `FileNode`s:
- a folder lists its children only when first opened, with the directory flag prefetched in the
  listing;
- only subfolders and STEP and STL files are shown;
- no file is ever opened to fill the tree.

**FSEvents keeps it current.** One stream watches the connected folders and every open file,
wherever it is. When a folder's contents change, its listing is dropped and read again. When an
open file changes, it is opened again. The cache key includes the size and modification time, so
the edit imports afresh, and the old entry ages out of the cache.

**Bindings are data, edited in Settings.**
- **Keys**: a `KeyMap` of named actions, which the viewport, the shortcut sheet and Settings all
  read. A new key is refused if another action already uses it, naming that action. Command
  shortcuts are refused too, since they belong to the menus, and so are modifiers on a held key,
  whose release would come apart from the key's.
- **Mouse**: Settings gives each of rotate, pan and dolly its own button, so choosing a taken
  button swaps the two. Shift or Space with the rotate button always pans, for trackpads and mice
  without a middle button. The engine's existing mouse table now crosses the C API, with whether
  the wheel zooms toward the pointer or the centre, its direction and its speed.

**The cache has a budget.** It may hold 5% of its volume's free space, at most 20 GB. After a
fresh import, a background task deletes the least recently used entries, each with its
refinements, sparing open files. A cache hit touches its file to record the use, since
modification time is what macOS keeps reliably.

## Exit criteria

| Criterion | Result |
|---|---|
| A 97k-file tree loads on demand, with no file reads | CADBench, 109,204 files: the root lists in 1.9 ms and the largest folder, 3,000 files, in 16 ms. Only folders are listed |
| Access survives relaunch | Security-scoped bookmarks are saved and resolved with scope at launch. **Not verified end to end**: that needs a person choosing a folder in an open panel, which automation cannot do in the sandbox |
| An edited file reloads | Touching an open file imported it again: the trace shows a second "scene ready" |
| All bindings editable | Every key action, including the held hide and ghost keys; the button for each drag gesture; zoom direction, centre and speed |
| Conflicts caught | A key in use, Command shortcuts, and modifiers on held keys are refused, with a message naming the problem |

## Deferred

- **Settings for rendering defaults.** The Look card already holds the look, which is remembered,
  so Settings does not repeat it.
- **A test target for the Swift layer.** The key map's conflict rules have no unit tests. The
  rules are short and were checked by reading.

## Addendum (2026-11-09): Folders and Recent, the same on every platform

The Library was connected folders on the Mac and, in the browser and on the iPhone, the files the
browser had kept. It is now both, the same everywhere: **Folders**, connected folders as trees read
a level at a time when they open, and **Recent**, the twelve files opened last. The first page,
the Library card and the toolbar each have the two ways in, Open files and Connect a folder, and a
dropped folder connects. `Web/viewer/library-rows.js` and `App/Sources/Library/LibraryRows.swift`
decide the rows alike; `library-rows.js` has tests.

- **Mac.** Folders and recent files are security-scoped bookmarks. Folders list off the main
  thread; one whose bookmark fails stays, marked missing, until disconnected. A recent file's
  bookmark is resolved only when it is chosen. The card is the structure tree's own table, rows
  and cells, in place of an outline view with system rows.
- **Browser.** `library-store.js` reaches folders through the File System Access API where there
  is one (handles kept in IndexedDB; a folder needing permission again shows Reconnect), and
  otherwise for the visit, from a folder input or a dropped folder. Recent is kept in IndexedDB. A
  file leaving Recent takes its kept import with it unless it is open, so what the browser keeps is
  bounded by Recent, where before nothing was ever deleted.
- **iPhone.** `ShellLibrary.swift` shows the system's pickers for files and folders, keeps folders
  as bookmarks, and the shell serves a folder's listing and files by a random id, never anything
  outside it. The loopback server now uses the same port each launch (47701, or any if taken): on
  a new port each launch the page's storage, its kept imports among it, was a stranger's.

Measured in headless Chrome (`test=library`): a file from a connected folder imports in 475 ms and
reopens from Recent in 5 ms, from the kept import; with the Library showing, 0 frames are drawn
while nothing happens, with either way of reaching folders.
