/*
 * bundle-cjs.mjs - produce the CommonJS entry so the package works under BOTH
 * `import` (ESM) and `require` (CommonJS), regardless of the consumer's module
 * setting (TS->ESM, TS->CJS, JS-ESM, JS-CJS).
 *
 * Strategy: tsc has already emitted the ESM build into dist/ (unchanged - ESM
 * consumers and the test suite keep using dist/index.js + dist/*.js subpaths).
 * Here we bundle the compiled dist/index.js into a single CommonJS file
 * dist/index.cjs. esbuild rewrites `import.meta.url` (used by native-binding to
 * locate pkgRoot for the addon + the bundled CTP DLLs) into a __filename-based
 * equivalent, so native resolution still works from dist/index.cjs (../  ==
 * pkgRoot, same as dist/index.js). node-gyp-build stays EXTERNAL so its own
 * __dirname-relative prebuild lookup is preserved (inlining it would break it).
 */
import { build } from "esbuild";
import { copyFileSync, existsSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const dist = join(root, "dist");

if (!existsSync(join(dist, "index.js"))) {
  console.error("dist/index.js not found - run `tsc` first (npm run build:ts runs both).");
  process.exit(1);
}

await build({
  entryPoints: [join(dist, "index.js")],
  outfile: join(dist, "index.cjs"),
  bundle: true,
  platform: "node",
  format: "cjs",
  target: "node18",
  // node-gyp-build does its own __dirname-relative prebuild resolution; keep it
  // external so it resolves against its installed package, not the bundle.
  external: ["node-gyp-build"],
  // esbuild leaves `import.meta.url` EMPTY in CJS output (it only works in ESM).
  // native-binding uses it to locate pkgRoot, so rewrite it to a __filename-based
  // file URL computed once in a banner. __filename in the emitted dist/index.cjs
  // IS that file, so dirname(..)/.. == pkgRoot, identical to the ESM build.
  define: { "import.meta.url": "import_meta_url_shim" },
  banner: {
    js: "const import_meta_url_shim = require('node:url').pathToFileURL(__filename).href;",
  },
  logLevel: "warning",
});

// CJS consumers resolve the entry's types from index.d.cts; the per-module .d.ts
// files in dist/ are shared (a .d.cts may reference them), so only the entry
// needs a .cts twin.
copyFileSync(join(dist, "index.d.ts"), join(dist, "index.d.cts"));

console.log("bundle-cjs: wrote dist/index.cjs + dist/index.d.cts");
