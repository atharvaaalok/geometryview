// A PDF of pictures: each page one JPEG, at its size in points. No fonts and no dependencies: the
// gallery's posters and its catalogue, drawn as they show, for the browser to save. (The Mac's are
// drawn from its layers, their text and lines kept as text and lines.)

/**
 * @param {{ jpeg: Uint8Array, width: number, height: number, points: [number, number] }[]} pages
 *   each page's JPEG, its size in pixels, and the page's size in points
 * @returns {Blob}
 */
export function pdfOfJpegs(pages) {
  const encoder = new TextEncoder();
  const chunks = [];
  const offsets = [];
  let length = 0;
  const push = (data) => {
    const bytes = typeof data === "string" ? encoder.encode(data) : data;
    chunks.push(bytes);
    length += bytes.length;
  };
  const object = (n, body) => {
    offsets[n] = length;
    push(`${n} 0 obj\n`);
    body();
    push("\nendobj\n");
  };
  const number = (v) => (Math.round(v * 100) / 100).toString();
  // The header, and a comment of high bytes so tools take the file as binary.
  push("%PDF-1.4\n");
  push(new Uint8Array([0x25, 0xe2, 0xe3, 0xcf, 0xd3, 0x0a]));
  // 1 the catalogue, 2 the page tree, then for each page: the page, its content, its picture.
  const count = 3 + pages.length * 3;
  object(1, () => push("<< /Type /Catalog /Pages 2 0 R >>"));
  object(2, () => push(`<< /Type /Pages /Kids [${pages.map((_, i) => `${3 + i * 3} 0 R`).join(" ")}] /Count ${pages.length} >>`));
  pages.forEach((p, i) => {
    const page = 3 + i * 3, content = page + 1, picture = page + 2;
    const [w, h] = p.points.map(number);
    object(page, () => push(`<< /Type /Page /Parent 2 0 R /MediaBox [0 0 ${w} ${h}] ` +
                            `/Resources << /XObject << /P ${picture} 0 R >> >> /Contents ${content} 0 R >>`));
    const draw = `q ${w} 0 0 ${h} 0 0 cm /P Do Q`;
    object(content, () => push(`<< /Length ${draw.length} >>\nstream\n${draw}\nendstream`));
    object(picture, () => {
      push(`<< /Type /XObject /Subtype /Image /Width ${p.width} /Height ${p.height} /ColorSpace /DeviceRGB ` +
           `/BitsPerComponent 8 /Filter /DCTDecode /Length ${p.jpeg.length} >>\nstream\n`);
      push(p.jpeg);
      push("\nendstream");
    });
  });
  // The cross-reference table: each entry exactly 20 bytes.
  const table = length;
  push(`xref\n0 ${count}\n0000000000 65535 f \n`);
  for (let n = 1; n < count; n++) push(`${String(offsets[n]).padStart(10, "0")} 00000 n \n`);
  push(`trailer\n<< /Size ${count} /Root 1 0 R >>\nstartxref\n${table}\n%%EOF\n`);
  return new Blob(chunks, { type: "application/pdf" });
}
