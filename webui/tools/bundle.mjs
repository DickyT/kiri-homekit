// Kiri Bridge — esbuild bundler for the embedded WebUI.
//
// Targets:
//   --target main       (default): emits dist/app.js + dist/app.css.
//                       Loaded chunk-by-chunk by loader.js into the SPA shell.
//   --target installer: emits dist/installer.html, a single self-contained
//                       page (CSS + JS inlined) for the captive-portal flow.
//
// Both targets run terser AFTER esbuild for an extra mangle/dead-code pass,
// then check size guards so future imports cannot accidentally bloat the
// firmware.

import { build } from "esbuild";
import { minify } from "terser";
import { mkdirSync, existsSync, rmSync, writeFileSync, readFileSync, readdirSync } from "node:fs";
import { gzipSync } from "node:zlib";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const __dirname = dirname(fileURLToPath(import.meta.url));
const root = resolve(__dirname, "..");
const distDir = resolve(root, "dist");

const args = process.argv.slice(2);
const target = (() => {
  const i = args.indexOf("--target");
  return i >= 0 ? args[i + 1] : "main";
})();

if (existsSync(distDir)) rmSync(distDir, { recursive: true, force: true });
mkdirSync(distDir, { recursive: true });

const gzSize = (path) => gzipSync(readFileSync(path), { level: 9 }).byteLength;

// Terser settings tuned for *gzipped* size, not raw byte count. Some
// "unsafe" optimizations and aggressive mangling actually inflate the gz
// output by making patterns less compressible. Settings here were picked
// after measuring real numbers; revisit before adding "advanced" flags.
const TERSER_OPTS = {
  compress: { passes: 2, pure_getters: true, unsafe: true, unsafe_arrows: true },
  mangle: true,
  format: { comments: false },
};

const SIZE_LIMITS = {
  main: { jsGz: 33 * 1024, cssGz: 8 * 1024 },
  installer: { htmlGz: 18 * 1024 },
};

if (target === "main") {
  await buildMain();
} else if (target === "installer") {
  await buildInstaller();
} else {
  console.error(`Unknown target: ${target}`);
  process.exit(2);
}

// ---------- main target ----------

async function buildMain() {
  const entry = resolve(root, "src/main.tsx");
  const outJs = resolve(distDir, "app.js");
  const outCss = resolve(distDir, "app.css");

  await build({
    entryPoints: [entry],
    outfile: outJs,
    bundle: true,
    format: "esm",
    platform: "browser",
    target: ["es2020"],
    minify: true,
    sourcemap: false,
    legalComments: "none",
    jsx: "automatic",
    jsxImportSource: "preact",
    treeShaking: true,
    loader: { ".css": "css" },
    drop: ["console", "debugger"],
  });

  // Extra terser pass on the JS bundle.
  const minified = await minify(readFileSync(outJs, "utf8"), { ...TERSER_OPTS, module: true });
  if (minified.code) writeFileSync(outJs, minified.code);

  const jsBytes = readFileSync(outJs).byteLength;
  const cssBytes = existsSync(outCss) ? readFileSync(outCss).byteLength : 0;
  const jsGz = gzSize(outJs);
  const cssGz = cssBytes ? gzSize(outCss) : 0;

  console.log(`[bundle] target=main`);
  console.log(`[bundle]   js  ${jsBytes.toLocaleString()} B  /  gz ${jsGz.toLocaleString()} B`);
  if (cssBytes) console.log(`[bundle]   css ${cssBytes.toLocaleString()} B  /  gz ${cssGz.toLocaleString()} B`);

  enforce(`main js gz`, jsGz, SIZE_LIMITS.main.jsGz);
  enforce(`main css gz`, cssGz, SIZE_LIMITS.main.cssGz);
}

// ---------- installer target ----------

async function buildInstaller() {
  const entry = resolve(root, "src/installer.tsx");
  const outJs = resolve(distDir, "installer.js");
  const outCss = resolve(distDir, "installer.css");
  const outHtml = resolve(distDir, "installer.html");

  await build({
    entryPoints: [entry],
    outfile: outJs,
    bundle: true,
    format: "iife",
    platform: "browser",
    target: ["es2020"],
    minify: true,
    sourcemap: false,
    legalComments: "none",
    jsx: "automatic",
    jsxImportSource: "preact",
    treeShaking: true,
    loader: { ".css": "css" },
    drop: ["console", "debugger"],
  });

  const minified = await minify(readFileSync(outJs, "utf8"), TERSER_OPTS);
  if (minified.code) writeFileSync(outJs, minified.code);

  const css = existsSync(outCss) ? readFileSync(outCss, "utf8") : "";
  const js = readFileSync(outJs, "utf8");

  const html = [
    `<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">`,
    `<title>Installer | Kiri Bridge</title>`,
    `<style>${css}</style>`,
    `</head><body><div id="app"></div><script>${js}</script></body></html>`,
  ].join("");
  writeFileSync(outHtml, html);

  const htmlBytes = Buffer.byteLength(html);
  const htmlGz = gzSize(outHtml);

  console.log(`[bundle] target=installer`);
  console.log(`[bundle]   js   ${readFileSync(outJs).byteLength.toLocaleString()} B`);
  console.log(`[bundle]   css  ${readFileSync(outCss).byteLength.toLocaleString()} B`);
  console.log(`[bundle]   html ${htmlBytes.toLocaleString()} B  /  gz ${htmlGz.toLocaleString()} B`);

  enforce(`installer html gz`, htmlGz, SIZE_LIMITS.installer.htmlGz);
}

function enforce(label, value, limit) {
  if (value > limit) {
    console.error(`[bundle] FAIL: ${label} ${value} exceeds ${limit}`);
    process.exit(1);
  }
}
