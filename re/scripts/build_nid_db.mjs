// Builds a PSP NID -> name table from PPSSPP's HLE sources.
// Usage: node build_nid_db.mjs <ppsspp Core/HLE dir> <out.json>
// Each entry records which PPSSPP module table lists it and whether PPSSPP
// provides an implementation (a null function pointer means "known but unimplemented").
import { readdirSync, readFileSync, writeFileSync } from "node:fs";
import { join } from "node:path";

const [dir, out] = process.argv.slice(2);
const db = {};
const tables = {}; // C++ table identifier -> PSP module name
for (const file of readdirSync(dir).filter((f) => f.endsWith(".cpp"))) {
  const src = readFileSync(join(dir, file), "utf8");
  for (const m of src.matchAll(/RegisterHLEModule\(\s*"([^"]+)"\s*,\s*ARRAY_SIZE\((\w+)\)/g)) tables[m[2]] = m[1];
  for (const m of src.matchAll(/\{\s*"([^"]+)"\s*,\s*ARRAY_SIZE\((\w+)\)\s*,\s*\2\s*\}/g)) tables[m[2]] = m[1];
  for (const t of src.matchAll(/HLEFunction\s+(\w+)\s*\[\]\s*=\s*\{([\s\S]*?)\n\};/g)) {
    for (const e of t[2].matchAll(/\{\s*0[xX]([0-9A-Fa-f]{8})\s*,\s*([^,]+?)\s*,\s*"([^"]*)"/g)) {
      const nid = e[1].toUpperCase();
      const impl = !/^(nullptr|0|NULL)$/.test(e[2].trim());
      (db[nid] ??= []).push({ name: e[3], table: t[1], file, implemented: impl });
    }
  }
}
for (const entries of Object.values(db)) for (const e of entries) e.module = tables[e.table] ?? null;
writeFileSync(out, JSON.stringify(db));
const all = Object.values(db).flat();
console.log(`${Object.keys(db).length} NIDs, ${all.filter((e) => e.implemented).length} implemented entries, ${Object.keys(tables).length} module tables`);
