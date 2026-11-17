// Where the viewer has been, for Back and Forward: folders shown in the gallery and the files open
// in the viewer. Pure, so it can be tested; the Mac's Navigation.swift keeps the same rules, and
// both are checked against Tests/data/navigation.json.
//
// A place is { kind: "folder", root, path } or { kind: "files", files: [...] }; a step is a place
// with what its folder showed when it was left ({ view, scroll, selected, search }), to show again.

export const kNavigationLimit = 50;

// A place as text: equal places, equal text.
export function placeKey(place) {
  if (!place) return "";
  return place.kind === "folder" ? `folder:${place.root}|${place.path}` : `files:${place.files.join(",")}`;
}

export class Navigation {
  steps = [];
  at = -1;

  get current() { return this.steps[this.at] ?? null; }
  get behind() { return this.steps[this.at - 1] ?? null; }
  get ahead() { return this.steps[this.at + 1] ?? null; }
  get canGoBack() { return this.at > 0; }
  get canGoForward() { return this.at + 1 < this.steps.length; }

  // Shows `place`: "same" if it is what shows, "back" or "forward" if it is the step beside, which
  // the history moves to, or else "new", a step that drops the steps ahead.
  visit(place) {
    const key = placeKey(place);
    if (key === placeKey(this.current?.place)) return "same";
    if (this.at > 0 && key === placeKey(this.steps[this.at - 1].place)) {
      this.at -= 1;
      return "back";
    }
    if (key === placeKey(this.steps[this.at + 1]?.place)) {
      this.at += 1;
      return "forward";
    }
    this.steps.splice(this.at + 1);
    this.steps.push({ place, shown: null });
    if (this.steps.length > kNavigationLimit) this.steps.splice(0, this.steps.length - kNavigationLimit);
    this.at = this.steps.length - 1;
    return "new";
  }

  // A file added to the view: the step that holds files now holds `place`; from a folder, a step.
  add(place) {
    if (this.current?.place.kind === "files") {
      this.current.place = place;
      return "same";
    }
    return this.visit(place);
  }

  back() {
    if (!this.canGoBack) return null;
    this.at -= 1;
    return this.current;
  }

  forward() {
    if (!this.canGoForward) return null;
    this.at += 1;
    return this.current;
  }

  // Moves to the step at `index`, as the browser's history does; null if there is none.
  go(index) {
    if (index < 0 || index >= this.steps.length || index === this.at) return null;
    this.at = index;
    return this.current;
  }

  // What the folder shown at `place` showed, kept for when it is shown again.
  keep(place, shown) {
    if (this.current && placeKey(this.current.place) === placeKey(place)) this.current.shown = shown;
  }
}
