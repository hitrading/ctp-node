/*
 * native-binding.ts - load the addon via node-gyp-build (prebuilt binary if
 * available, else the local build/), and make the CTP shared libraries findable
 * by the dynamic loader.
 */
import { createRequire } from "node:module";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";
import process from "node:process";

const require = createRequire(import.meta.url);
const pkgRoot = join(dirname(fileURLToPath(import.meta.url)), "..");

// On Windows the CTP .dll must be on the search path; prepend the bundled dir
// so it is found regardless of where the .node lives (build/ or prebuilds/).
// Linux/macOS resolve their .so/.framework via the rpath baked at build time.
if (process.platform === "win32") {
  process.env.PATH = `${join(pkgRoot, "ctpapi", "windows")};${process.env.PATH ?? ""}`;
}

// eslint-disable-next-line @typescript-eslint/no-explicit-any
const loadAddon = require("node-gyp-build") as (root: string) => any;

// eslint-disable-next-line @typescript-eslint/no-explicit-any
export const native = loadAddon(pkgRoot) as any;
