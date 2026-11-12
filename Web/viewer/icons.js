// Line icons for the chrome, on a 16 unit grid, stroked in the current text colour. The Mac app
// draws these same icons: it reads this file, so each icon stays on one line of its own, as
// `name: \`markup\`,` with plain SVG markup and nothing computed. Launcher/test/icons.test.mjs
// reads it the way the app does.

const svg = (markup, size = 16) =>
  `<svg viewBox="0 0 16 16" width="${size}" height="${size}" fill="none" stroke="currentColor" ` +
  `stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">${markup}</svg>`;

export const iconMarkup = {
  eye: `<path d="M1.5 8s2.4-4.5 6.5-4.5S14.5 8 14.5 8 12.1 12.5 8 12.5 1.5 8 1.5 8z"/><circle cx="8" cy="8" r="2"/>`,
  eyeSlash: `<path d="M1.5 8s2.4-4.5 6.5-4.5S14.5 8 14.5 8 12.1 12.5 8 12.5 1.5 8 1.5 8z"/><circle cx="8" cy="8" r="2"/><path d="M2.5 2.5l11 11"/>`,
  ghost: `<circle cx="8" cy="8" r="5.5" stroke-dasharray="2.2 1.8"/>`,
  scope: `<circle cx="8" cy="8" r="4.5"/><path d="M8 1.5v3M8 11.5v3M1.5 8h3M11.5 8h3"/>`,
  undo: `<path d="M5.5 4.5L2.5 7.5l3 3"/><path d="M2.5 7.5H10a3.5 3.5 0 0 1 0 7H8"/>`,
  redo: `<path d="M10.5 4.5l3 3-3 3"/><path d="M13.5 7.5H6a3.5 3.5 0 0 0 0 7h2"/>`,
  seeThrough: `<circle cx="8" cy="8" r="5.5"/><path d="M8 2.5v11a5.5 5.5 0 0 1 0-11z" fill="currentColor"/>`,
  scissors: `<circle cx="4" cy="11.5" r="2"/><circle cx="12" cy="11.5" r="2"/><path d="M5.5 10L12 2.5M10.5 10L4 2.5"/>`,
  palette: `<path d="M8 1.75a6.25 6.25 0 1 0 0 12.5c1 0 1.5-.6 1.5-1.3 0-.9-.8-1.3-.8-2 0-.8.6-1.2 1.4-1.2h1.6c1.6 0 2.55-1.1 2.55-2.6C14.25 4.4 11.6 1.75 8 1.75z"/><circle cx="4.6" cy="7.4" r=".9" fill="currentColor"/><circle cx="6.6" cy="4.6" r=".9" fill="currentColor"/><circle cx="10" cy="4.6" r=".9" fill="currentColor"/>`,
  cube: `<path d="M8 1.75l5.5 3v6.5L8 14.25l-5.5-3V4.75z"/><path d="M2.5 4.75L8 7.75l5.5-3M8 7.75v6.5"/>`,
  fit: `<path d="M2.5 6V2.5H6M10 2.5h3.5V6M13.5 10v3.5H10M6 13.5H2.5V10"/>`,
  chevron: `<path d="M6 4l4 4-4 4"/>`,
  assembly: `<path stroke-width="1.25" d="M9.24 8.1L5.6 10.2L1.96 8.1L1.96 3.9L5.6 1.8L9.24 3.9zM14.04 12.1L10.4 14.2L6.76 12.1L6.76 7.9L10.4 5.8L14.04 7.9z"/>`,
  search: `<circle cx="7" cy="7" r="4.5"/><path d="M10.5 10.5l3.5 3.5"/>`,
  folder: `<path d="M2.5 4.5v8h11V6.5H8L6.5 4.5z"/>`,
  folderPlus: `<path d="M2.5 4.5v8h11V6.5H8L6.5 4.5z"/><path d="M8 7.75v3.5M6.25 9.5h3.5"/>`,
  file: `<path d="M4 1.75h5.25L12.5 5v9.25H4z"/><path d="M9.25 1.75V5h3.25"/>`,
  plus: `<path d="M8 3v10M3 8h10"/>`,
  close: `<path d="M4 4l8 8M12 4l-8 8"/>`,
  parts: `<path d="M3 4h10M3 8h10M3 12h10"/>`,
  more: `<circle cx="3.5" cy="8" r="1.1" fill="currentColor" stroke="none"/><circle cx="8" cy="8" r="1.1" fill="currentColor" stroke="none"/><circle cx="12.5" cy="8" r="1.1" fill="currentColor" stroke="none"/>`,
  check: `<path d="M3.5 8.5l3 3 6-7"/>`,
  plates: `<rect x="1.75" y="2.25" width="5.5" height="5" rx="1"/><rect x="8.75" y="2.25" width="5.5" height="5" rx="1"/><rect x="1.75" y="8.75" width="5.5" height="5" rx="1"/><rect x="8.75" y="8.75" width="5.5" height="5" rx="1"/><path d="M3 6h1M4.75 6h1M10 6h3M3 12.5h3M10 12.5h1"/>`,
  sheet: `<path d="M2 3.5h12 M2 7h12 M2 10.5h12 M2 14h12"/><circle cx="4" cy="5.25" r=".6"/><circle cx="7" cy="5.25" r=".6"/><circle cx="10" cy="5.25" r=".6"/><circle cx="13" cy="5.25" r=".6"/><circle cx="4" cy="12.25" r=".6"/><circle cx="7" cy="12.25" r=".6"/>`,
  ladders: `<path d="M1.5 12.5h13 M2.5 12.5v-1.5 M5.5 12.5v-3 M9 12.5v-5 M13 12.5v-8 M1.5 14.5v-2 M5 14.5v-2 M8.5 14.5v-2 M12 14.5v-2"/>`,
  atlas: `<path d="M1.75 3.5l4-1.5 4.5 1.5 4-1.5v10.5l-4 1.5-4.5-1.5-4 1.5z M5.75 2v10.5 M10.25 3.5V14"/>`,
  poster: `<rect x="2.5" y="1.75" width="11" height="12.5" rx="1"/><path d="M4.5 11h7 M4.5 5l2.5 3 2-2 2.5 3"/>`,
  catalogue: `<path d="M8 3.5c-1.5-1-3.5-1.25-6-1v10c2.5-.25 4.5 0 6 1 1.5-1 3.5-1.25 6-1v-10c-2.5-.25-4.5 0-6 1z M8 3.5v10"/>`,
};

export const icons = Object.fromEntries(
  Object.entries(iconMarkup).map(([name, markup]) => [name, (size) => svg(markup, size)]));

// An icon as an element, parsed once and then cloned, for chrome made often, such as tree rows.
const parsed = new Map();
export function iconNode(name, size) {
  const key = `${name} ${size}`;
  let node = parsed.get(key);
  if (!node) {
    const template = document.createElement("template");
    template.innerHTML = icons[name](size);
    node = template.content.firstChild;
    parsed.set(key, node);
  }
  return node.cloneNode(true);
}
