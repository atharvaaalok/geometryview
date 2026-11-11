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
  #ladders;
  #laddersSort;
  #laddersScale;
  #atlas;
  #atlasLevels;
  #catalogue;
  #pages;
  #contents;
  #index;
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
    // Ladders: the order (shared with Sheet) and the scale, each strip's own or one for all.
    this.#laddersSort = segmented(["Size", "Name"], { help: "Order", onChange: (i) => on.sheet({ sort: i === 0 ? 1 : 0 }) });
    this.#laddersScale = segmented(["Each", "Shared"], {
      help: "Scale", tips: ["Each strip at its own scale", "Every strip at one scale"],
      onChange: (i) => on.ladders?.({ shared: i === 1 }),
    });
    this.#ladders = el("div.group.desktop-only", {}, [el("span.rule"), label("SORT"), this.#laddersSort.element,
                                                       label("SCALE"), this.#laddersScale.element]);
    // Atlas: out a level, the level, in a level.
    this.#atlasLevels = segmented(["Regions", "Districts", "Parts"], { help: "Level", onChange: (level) => on.atlas?.({ level }) });
    this.#atlas = el("div.group", {}, [el("span.rule"),
      pill({ title: "\u2212", help: "Zoom out a level (\u2212)", onPress: () => on.atlas?.({ step: -1 }) }),
      this.#atlasLevels.element,
      pill({ title: "+", help: "Zoom in a level (+)", onPress: () => on.atlas?.({ step: 1 }) })]);
    // Catalogue: turn the book, the pages in view, the contents and the index.
    this.#pages = el("span.tlabel.pages", { text: "" });
    this.#catalogue = el("div.group", {}, [el("span.rule"),
      pill({ title: "\u2039", help: "Turn back (\u2190)", onPress: () => on.catalogue?.({ step: -1 }) }),
      this.#pages,
      pill({ title: "\u203a", help: "Turn on (\u2192)", onPress: () => on.catalogue?.({ step: 1 }) }),
      this.#contents = pill({ title: "Contents", help: "The contents", onPress: () => on.catalogue?.({ spread: 1 }) }),
      this.#index = pill({ title: "Index", help: "The index", onPress: () => on.catalogue?.({ index: true }) })]);
    // Short of room, the view switch drops its keys, then Poster and Look their titles (.fold).
    addEventListener("resize", () => this.#fit());
    this.#look = pill({ icon: icons.palette, title: Styles[0].title, help: "Look", onPress: () => on.look() });
    this.#poster = pill({ icon: icons.poster, title: "Poster", help: "Show it off: the view as a poster (⌥P)",
                          onPress: () => on.poster() });
    this.#poster.classList.add("desktop-only", "fold");
    this.#look.classList.add("fold");
    this.#sheet.classList.add("desktop-only");
    // On a phone: the Library's sheet, raised from here.
    const library = pill({ icon: icons.parts, help: "Library", onPress: () => on.library?.() });
    library.classList.add("phone-only");
    this.element = el("nav.card.toolbar.gallery-toolbar", { "aria-label": "Gallery", hidden: true },
                      [library, views, this.#sheet, this.#ladders, this.#atlas, this.#catalogue, el("span.rule.desktop-only"),
                       this.#poster, this.#look]);
    this.posterBar = el("div.card.gallery-poster-bar", { hidden: true, "aria-label": "Poster" }, [
      pill({ title: "Save PNG", help: "Save the poster as a picture", onPress: () => on.save() }),
      pill({ title: "Save PDF", help: "Save the poster, or in Catalogue the whole book, as a PDF",
             onPress: () => on.savePDF() }),
      pill({ title: "Done", help: "Back to the gallery (Esc)", onPress: () => on.poster() }),
    ]);
  }

  /** Shows the view in use among those enabled (a bit each), Sheet's controls, and the style. */
  show({ view, views, sheet, style, ladders = { shared: false }, atlasLevel = 0,
          catalogue = { spread: 0, spreads: 1, index: 0 } }) {
    this.#catalogue.classList.toggle("off", view !== 4);
    this.#contents.classList.toggle("on", catalogue.spread === 1);
    this.#index.classList.toggle("on", catalogue.spread > 0 && catalogue.spread >= catalogue.index);
    const pages = catalogue.spreads * 2;
    this.#pages.textContent = catalogue.spread === 0 ? `Cover \u00b7 ${pages - 2} pages`
      : `Pages ${catalogue.spread * 2}\u2013${catalogue.spread * 2 + 1} of ${pages - 1}`;
    this.#atlas.classList.toggle("off", view !== 3);
    this.#atlasLevels.selected = atlasLevel;
    this.#ladders.classList.toggle("off", view !== 2);
    this.#laddersSort.selected = sheet.sort === 1 ? 0 : 1;
    this.#laddersScale.selected = ladders.shared ? 1 : 0;
    this.#buttons.forEach((button, i) => {
      button.hidden = (views & (1 << i)) === 0;
      button.classList.toggle("on", i === view);
      button.setAttribute("aria-selected", i === view ? "true" : "false");
    });
    this.#sheet.classList.toggle("off", view !== 1);
    this.#scale.selected = sheet.scale;
    if (Number(this.#size.value) !== sheet.cell) this.#size.value = String(sheet.cell);
    this.#sort.selected = sheet.sort;
    this.#look.querySelector("span").textContent = Styles[style]?.title ?? "";
    this.#fit();
  }

  /** Fits the toolbar into its room, as the Mac's does: keys go first, then two titles. */
  #fit() {
    const bar = this.element;
    bar.classList.remove("tight", "tighter");
    for (const step of ["tight", "tighter"]) {
      if (bar.scrollWidth <= bar.clientWidth) break;
      bar.classList.add(step);
    }
  }
}
