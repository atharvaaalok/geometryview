// The gallery's toolbar, in place of the viewer's while the gallery shows: the view switch, the
// view's own controls (Sheet's scale, picture size and order), and the Look pill. The page's
// counterpart of the Mac's gallery toolbar (App/Sources/Gallery/GalleryToolbar.swift).
//
// Browsers keep ⌘1–⌘5 for their tabs, so the views are on ⌥1–⌥5 here.

import { Styles } from "./chrome.js";
import { ViewNames } from "./gallery-core.js";
import { iconNode, icons } from "./icons.js";
import { el, pill, segmented } from "./ui.js";

const kViewIcons = ["plates", "sheet", "ladders", "atlas", "catalogue"];
const label = (text) => el("span.tlabel", { text });

export class GalleryToolbar {
  element;
  #buttons = [];
  #sheet;
  #scale;
  #size;
  #sort;
  #look;
  #poster;
  /** While a poster shows: Save PNG and Done, over its top margin. */
  posterBar;

  /**
   * `on` takes view(index), sheet({ scale, cell, sort }) with what changed, look(), poster() to show
   * or put away the poster, and save() for its PNG.
   */
  constructor(on) {
    const views = el("div.segmented.views", { role: "tablist", "aria-label": "View" });
    ViewNames.forEach((name, view) => {
      const button = el("button", { type: "button", role: "tab", title: `${name} (⌥${view + 1})`,
                                    onclick: () => on.view(view) },
                        [el("span.glyph", {}, [iconNode(kViewIcons[view], 15)]), el("span.nm", { text: name }),
                         el("span.k", { text: `⌥${view + 1}` })]);
      this.#buttons.push(button);
      views.append(button);
    });
    this.#scale = segmented(["Fit", "Folder", "True"], {
      help: "How parts are scaled",
      tips: ["Each part fills its cell", "One scale for each folder", "One scale for everything in view"],
      onChange: (scale) => on.sheet({ scale }),
    });
    this.#size = el("input.slider", { type: "range", min: 72, max: 200, step: 4, title: "Picture size",
                                      "aria-label": "Picture size" });
    this.#size.addEventListener("input", () => on.sheet({ cell: Number(this.#size.value) }));
    this.#sort = segmented(["Name", "Size"], { help: "Order", onChange: (sort) => on.sheet({ sort }) });
    this.#sheet = el("div.group", {}, [el("span.rule"), label("SCALE"), this.#scale.element, label("SIZE"),
                                       this.#size, label("SORT"), this.#sort.element]);
    this.#look = pill({ icon: icons.palette, title: Styles[0].title, help: "Look", onPress: () => on.look() });
    this.#poster = pill({ icon: icons.poster, title: "Poster", help: "Show it off: the view as a poster (⌥P)",
                          onPress: () => on.poster() });
    this.element = el("nav.card.toolbar.gallery-toolbar", { "aria-label": "Gallery", hidden: true },
                      [views, this.#sheet, el("span.rule"), this.#poster, this.#look]);
    this.posterBar = el("div.card.gallery-poster-bar", { hidden: true, "aria-label": "Poster" }, [
      pill({ title: "Save PNG", help: "Save the poster as a picture", onPress: () => on.save() }),
      pill({ title: "Done", help: "Back to the gallery (Esc)", onPress: () => on.poster() }),
    ]);
  }

  /** Shows the view in use among those enabled (a bit each), Sheet's controls, and the style. */
  show({ view, views, sheet, style }) {
    this.#buttons.forEach((button, i) => {
      button.hidden = (views & (1 << i)) === 0;
      button.classList.toggle("on", i === view);
      button.setAttribute("aria-selected", i === view ? "true" : "false");
    });
    this.#sheet.hidden = view !== 1;
    this.#scale.selected = sheet.scale;
    if (Number(this.#size.value) !== sheet.cell) this.#size.value = String(sheet.cell);
    this.#sort.selected = sheet.sort;
    this.#look.querySelector("span").textContent = Styles[style]?.title ?? "";
  }
}
