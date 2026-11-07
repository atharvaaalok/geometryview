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
