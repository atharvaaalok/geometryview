// The gallery's PDF in the browser: pages of JPEGs, its cross-reference table pointing at each object.

import { test } from "node:test";
import assert from "node:assert/strict";
import { pdfOfJpegs } from "../../Web/viewer/pdf.js";

const page = (n, points) => ({ jpeg: new Uint8Array([0xff, 0xd8, n, n, 0xff, 0xd9]), width: 4, height: 3, points });

test("a PDF of two pages: its table finds every object, and the trailer finds the table", async () => {
  const blob = pdfOfJpegs([page(1, [1380, 840]), page(2, [1008.5, 665])]);
  assert.equal(blob.type, "application/pdf");
  const bytes = new Uint8Array(await blob.arrayBuffer());
  const text = new TextDecoder("latin1").decode(bytes);
  assert.ok(text.startsWith("%PDF-1.4\n"));
  assert.ok(text.endsWith("%%EOF\n"));
  assert.match(text, /\/Type \/Pages \/Kids \[3 0 R 6 0 R\] \/Count 2/);
  assert.match(text, /\/MediaBox \[0 0 1380 840\]/);
  assert.match(text, /\/MediaBox \[0 0 1008\.5 665\]/);
  // startxref gives the table's offset; each entry, 20 bytes, an object's.
  const table = Number(text.match(/startxref\n(\d+)\n/)[1]);
  assert.ok(text.startsWith("xref\n0 9\n", table));
  const entries = text.slice(table + "xref\n0 9\n".length).split("\n").slice(0, 9);
  assert.equal(entries[0], "0000000000 65535 f ");
  for (let n = 1; n < 9; n++) {
    assert.equal(entries[n].length, 19); // and its newline: 20 bytes
    const at = Number(entries[n].slice(0, 10));
    assert.ok(text.startsWith(`${n} 0 obj\n`, at), `object ${n} at ${at}`);
  }
  // Each picture's stream holds its JPEG whole, as long as it says.
  const length = Number(text.match(/\/DCTDecode \/Length (\d+)/)[1]);
  assert.equal(length, 6);
});
