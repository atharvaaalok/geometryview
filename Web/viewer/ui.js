// The chrome's controls, after the Mac app's Design folder: sticker cards, pill buttons,
// segmented pills and a slider, as plain elements styled by viewer.css.

// An element: `el("div.card.look", { title: "…", onclick }, [children])`.
export function el(spec, attributes = {}, children = []) {
  const [tag, ...classes] = spec.split(".");
  const node = document.createElement(tag || "div");
  if (classes.length) node.className = classes.join(" ");
  for (const [key, value] of Object.entries(attributes)) {
    if (value === undefined || value === null || value === false) continue;
    if (key.startsWith("on")) node.addEventListener(key.slice(2), value);
    else if (key === "html") node.innerHTML = value;
    else if (key === "text") node.textContent = value;
    else node.setAttribute(key, value === true ? "" : value);
  }
  for (const child of [].concat(children)) {
    if (child !== null && child !== undefined && child !== false) {
      node.append(child instanceof Node ? child : document.createTextNode(String(child)));
    }
  }
  return node;
}

// A rounded button with a title or an icon. `style` is "plain" or "ghostly".
export function pill({ title, icon, help, style = "plain", onPress }) {
  const button = el(`button.pill.${style}${icon && !title ? ".icon" : ""}`, {
    type: "button", title: help, "aria-label": help ?? title, onclick: () => onPress?.(),
  });
  if (icon) button.insertAdjacentHTML("beforeend", icon(15));
  if (title) button.append(el("span", { text: title }));
  return button;
}

// A row of choices, one selected: { element, selected (get/set), enabled (set) }.
export function segmented(titles, { selected = 0, help, tips = [], onChange } = {}) {
  const element = el("div.segmented", { role: "radiogroup", title: help, "aria-label": help });
  let current = selected;
  const buttons = titles.map((title, index) => el("button", {
    type: "button", role: "radio", title: tips[index], text: title,
    onclick: () => {
      if (index === current) return;
      control.selected = index;
      onChange?.(index);
    },
  }));
  element.append(...buttons);
  const control = {
    element,
    get selected() { return current; },
    set selected(index) {
      current = index;
      buttons.forEach((button, i) => {
        button.classList.toggle("on", i === index);
        button.setAttribute("aria-checked", i === index ? "true" : "false");
      });
    },
    set enabled(enabled) { element.classList.toggle("disabled", !enabled); },
  };
  control.selected = selected;
  return control;
}

// A slider from 0 to 1: { element, value (get/set), enabled (set) }.
export function slider({ help, onChange }) {
  const input = el("input.slider", { type: "range", min: 0, max: 1, step: 0.001, title: help,
                                      "aria-label": help });
  input.addEventListener("input", () => onChange?.(Number(input.value)));
  return {
    element: input,
    get value() { return Number(input.value); },
    set value(v) { input.value = String(v); },
    set enabled(enabled) { input.disabled = !enabled; },
  };
}

// A key cap, as the shortcuts sheet and the hold hint show keys.
export const keyCap = (text) => el("kbd.cap", { text });

export const formatNumber = (value) => value.toLocaleString("en-US");

// A scrolling list that makes elements only for the rows in view and a few either side, so a list
// of a million rows scrolls as one of ten. `count()` and `row(index)` give the rows and
// `rowHeight()` their height; `render()` after they change, or `schedule()` to render once at the
// next frame, as scrolling and resizing do.
export class VirtualList {
  #spacer = el("div.spacer");
  #rows;
  #pending = false;
  #count;
  #row;
  #rowHeight;
  element;

  constructor({ role, count, row, rowHeight }) {
    this.#count = count;
    this.#row = row;
    this.#rowHeight = rowHeight;
    this.#rows = el("div.rows", { role });
    this.element = el("div.tree", {}, [this.#spacer, this.#rows]);
    this.element.addEventListener("scroll", () => this.schedule());
    new ResizeObserver(() => this.schedule()).observe(this.element);
  }

  schedule() {
    if (this.#pending) return;
    this.#pending = true;
    requestAnimationFrame(() => {
      this.#pending = false;
      this.render();
    });
  }

  render() {
    const count = this.#count();
    const height = this.#rowHeight();
    this.#spacer.style.height = `${count * height}px`;
    const first = Math.max(0, Math.floor(this.element.scrollTop / height) - 4);
    const last = Math.min(count, first + Math.ceil(this.element.clientHeight / height) + 8);
    this.#rows.style.transform = `translateY(${first * height}px)`;
    const children = [];
    for (let index = first; index < last; ++index) children.push(this.#row(index));
    this.#rows.replaceChildren(...children);
  }

  // Scrolls `index` into view, a third of the way down, if it is out of view.
  scrollToRow(index) {
    const height = this.#rowHeight();
    const top = index * height;
    const view = this.element.clientHeight;
    if (top < this.element.scrollTop || top + height > this.element.scrollTop + view) {
      this.element.scrollTop = Math.max(0, top - view / 3);
    }
  }
}
