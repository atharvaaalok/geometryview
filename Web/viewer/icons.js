// Line icons for the chrome, drawn to stand in for the SF Symbols the Mac app uses, on a 16 unit
// grid in the current text colour.

const svg = (body, size = 16) =>
  `<svg viewBox="0 0 16 16" width="${size}" height="${size}" fill="none" stroke="currentColor" ` +
  `stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">${body}</svg>`;

const eyeShape = `<path d="M1.5 8s2.4-4.5 6.5-4.5S14.5 8 14.5 8 12.1 12.5 8 12.5 1.5 8 1.5 8z"/>` +
                 `<circle cx="8" cy="8" r="2"/>`;

export const icons = {
  eye: (size) => svg(eyeShape, size),
  eyeSlash: (size) => svg(`${eyeShape}<path d="M2.5 2.5l11 11"/>`, size),
  ghost: (size) => svg(`<circle cx="8" cy="8" r="5.5" stroke-dasharray="2.2 1.8"/>`, size),
  scope: (size) => svg(`<circle cx="8" cy="8" r="4.5"/><path d="M8 1.5v3M8 11.5v3M1.5 8h3M11.5 8h3"/>`, size),
  undo: (size) => svg(`<path d="M5.5 4.5L2.5 7.5l3 3"/><path d="M2.5 7.5H10a3.5 3.5 0 0 1 0 7H8"/>`, size),
  redo: (size) => svg(`<path d="M10.5 4.5l3 3-3 3"/><path d="M13.5 7.5H6a3.5 3.5 0 0 0 0 7h2"/>`, size),
  seeThrough: (size) => svg(`<circle cx="8" cy="8" r="5.5"/><path d="M8 2.5v11a5.5 5.5 0 0 1 0-11z" fill="currentColor"/>`, size),
  scissors: (size) => svg(`<circle cx="4" cy="11.5" r="2"/><circle cx="12" cy="11.5" r="2"/>` +
                          `<path d="M5.5 10L12 2.5M10.5 10L4 2.5"/>`, size),
  palette: (size) => svg(`<path d="M8 1.75a6.25 6.25 0 1 0 0 12.5c1 0 1.5-.6 1.5-1.3 0-.9-.8-1.3-.8-2 ` +
                         `0-.8.6-1.2 1.4-1.2h1.6c1.6 0 2.55-1.1 2.55-2.6C14.25 4.4 11.6 1.75 8 1.75z"/>` +
                         `<circle cx="4.6" cy="7.4" r=".9" fill="currentColor"/><circle cx="6.6" cy="4.6" r=".9" fill="currentColor"/>` +
                         `<circle cx="10" cy="4.6" r=".9" fill="currentColor"/>`, size),
  cube: (size) => svg(`<path d="M8 1.75l5.5 3v6.5L8 14.25l-5.5-3V4.75z"/><path d="M2.5 4.75L8 7.75l5.5-3M8 7.75v6.5"/>`, size),
  fit: (size) => svg(`<path d="M2.5 6V2.5H6M10 2.5h3.5V6M13.5 10v3.5H10M6 13.5H2.5V10"/>`, size),
  chevron: (size) => svg(`<path d="M6 4l4 4-4 4"/>`, size),
  assembly: (size) => svg(`<circle cx="6" cy="7" r="3.5"/><circle cx="10" cy="9" r="3.5"/>`, size),
  search: (size) => svg(`<circle cx="7" cy="7" r="4.5"/><path d="M10.5 10.5l3.5 3.5"/>`, size),
  open: (size) => svg(`<path d="M2.5 4.5v8h11V6.5H8L6.5 4.5z"/>`, size),
  close: (size) => svg(`<path d="M4 4l8 8M12 4l-8 8"/>`, size),
  parts: (size) => svg(`<path d="M3 4h10M3 8h10M3 12h10"/>`, size),
};
