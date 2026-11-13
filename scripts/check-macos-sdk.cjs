#!/usr/bin/env node
/*
 * check-macos-sdk.cjs - build-time guard, run by binding.gyp on macOS only.
 *
 * The addon compiles against the CTP headers in ctpsdk/*.h, which belong to the
 * Windows/Linux libraries in ctpsdk/windows and ctpsdk/linux. A macOS framework
 * from another CTP version still compiles and links, because node-gyp links
 * macOS addons with `-undefined dynamic_lookup`, but the result crashes at
 * runtime: the factory signatures and the API/SPI vtables change between CTP
 * versions (a v6.7.7 framework under v6.7.13 headers segfaults on the first
 * CreateFtdcMdApi call). So refuse to configure unless every vendored framework
 * binary reports the same CTP version (x.y.z) as the reference Linux library.
 *
 * Usage: node scripts/check-macos-sdk.cjs [package-root]
 * Prints nothing and exits 0 when the versions match (binding.gyp ignores the
 * output); otherwise explains on stderr and exits 1, which fails node-gyp.
 */
"use strict";

const { readFileSync } = require("node:fs");
const { join, resolve } = require("node:path");

const root = resolve(process.argv[2] ?? join(__dirname, ".."));

const REFERENCE = "ctpsdk/linux/thosttraderapi_se.so";
const FRAMEWORKS = [
  "ctpsdk/macos/thostmduserapi_se.framework/Versions/A/thostmduserapi_se",
  "ctpsdk/macos/thosttraderapi_se.framework/Versions/A/thosttraderapi_se",
];

// The vendor libraries embed a build stamp such as
// "v6.7.13_20260225 14:16:30.12079" or "v6.7.7_MacOS_20240716 15:00:00".
const STAMP = /v(\d+\.\d+\.\d+)_(?:MacOS_)?\d{8} \d{2}:\d{2}:\d{2}/g;

function readStamp(rel) {
  let bytes;
  try {
    bytes = readFileSync(join(root, rel));
  } catch (err) {
    return { rel, error: `cannot read (${err.code ?? err.message})` };
  }
  const stamps = new Set();
  const versions = new Set();
  for (const m of bytes.toString("latin1").matchAll(STAMP)) {
    stamps.add(m[0]);
    versions.add(m[1]);
  }
  if (versions.size !== 1) {
    return { rel, error: versions.size ? `ambiguous CTP version (${[...stamps].join(", ")})` : "no CTP version stamp found" };
  }
  return { rel, version: [...versions][0], stamp: [...stamps][0] };
}

const ref = readStamp(REFERENCE);
const frameworks = FRAMEWORKS.map(readStamp);
const ok = !ref.error && frameworks.every((f) => !f.error && f.version === ref.version);

if (!ok) {
  const line = (f) => `  ${f.rel}: ${f.error ?? `v${f.version} (${f.stamp})`}`;
  process.stderr.write(
    [
      "",
      "ctp-node: refusing to build on macOS - CTP SDK version mismatch.",
      "  The addon is compiled against the CTP headers in ctpsdk/*.h, which match:",
      line(ref),
      "  but the bundled macOS frameworks are:",
      ...frameworks.map(line),
      "  An addon built from these links (node-gyp uses -undefined dynamic_lookup) but",
      "  crashes at runtime, because the factory functions and the API/SPI vtables differ",
      "  between CTP versions. macOS is not supported by this release; use Windows or",
      "  Linux x64 (prebuilt). See docs/troubleshooting.md, \"Build & install\".",
      "  Maintainers: replace ctpsdk/macos/*.framework with the SFIT MacOS build of the",
      "  same CTP version as ctpsdk/linux, then rebuild.",
      "ctp-node：macOS 框架的 CTP 版本与头文件（及 Windows/Linux 库）不一致，这样编出的插件",
      "运行时会崩溃，构建已中止。当前版本不支持 macOS，请使用 Windows 或 Linux x64。",
      "",
    ].join("\n"),
  );
  process.exit(1);
}
