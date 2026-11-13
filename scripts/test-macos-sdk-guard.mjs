// macOS CTP SDK guard (scripts/check-macos-sdk.cjs), testable on any OS: the
// guard against fixture SDK trees, its wiring in binding.gyp / package.json, and
// that the real vendored binaries parse (whatever version the frameworks are).
import { spawnSync } from "node:child_process";
import { mkdtempSync, mkdirSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const guard = join(root, "scripts", "check-macos-sdk.cjs");
const run = (dir) => spawnSync(process.execPath, [guard, ...(dir ? [dir] : [])], { encoding: "utf8" });

let pass = 0;
let fail = 0;
const check = (c, m) => (c ? (pass++, console.log("  PASS:", m)) : (fail++, console.error("  FAIL:", m)));

const LINUX = "ctpsdk/linux/thosttraderapi_se.so";
const MD = "ctpsdk/macos/thostmduserapi_se.framework/Versions/A/thostmduserapi_se";
const TD = "ctpsdk/macos/thosttraderapi_se.framework/Versions/A/thosttraderapi_se";
const blob = (stamp) => Buffer.concat([Buffer.from([0x7f, 0x45, 0x00, 0xff]), Buffer.from(`\0${stamp}\0`, "latin1"), Buffer.alloc(64)]);

const tmp = mkdtempSync(join(tmpdir(), "ctp-macos-guard-"));
const fixture = (name, files) => {
  const dir = join(tmp, name);
  for (const [rel, stamp] of Object.entries(files)) {
    mkdirSync(dirname(join(dir, rel)), { recursive: true });
    writeFileSync(join(dir, rel), Buffer.isBuffer(stamp) ? stamp : blob(stamp));
  }
  return dir;
};

try {
  // 1) same x.y.z (different build dates, MacOS-style stamp) -> silent success
  let r = run(fixture("match", {
    [LINUX]: "v6.7.13_20260225 14:16:30.12079",
    [MD]: "v6.7.13_MacOS_20260529 15:38:00",
    [TD]: "v6.7.13_MacOS_20260529 15:38:00",
  }));
  check(r.status === 0 && r.stdout === "" && r.stderr === "", `matching versions pass silently (status ${r.status})`);

  // 2) the shipped situation: v6.7.7 frameworks under v6.7.13 headers -> refuse
  r = run(fixture("mismatch", {
    [LINUX]: "v6.7.13_20260225 14:16:30.12079",
    [MD]: "v6.7.7_MacOS_20240716 15:00:00",
    [TD]: "v6.7.7_MacOS_20240716 15:00:00",
  }));
  check(r.status === 1, `version mismatch fails configure (status ${r.status})`);
  check(r.stdout === "", "nothing on stdout (binding.gyp would consume it)");
  check(/refusing to build on macOS/.test(r.stderr) && /v6\.7\.13/.test(r.stderr) && /v6\.7\.7/.test(r.stderr), "stderr explains with both versions");

  // 3) one framework upgraded, the other not -> still refuse
  r = run(fixture("half", {
    [LINUX]: "v6.7.13_20260225 14:16:30.12079",
    [MD]: "v6.7.13_MacOS_20260529 15:38:00",
    [TD]: "v6.7.7_MacOS_20240716 15:00:00",
  }));
  check(r.status === 1, "a single stale framework fails");

  // 4) missing framework / unstamped binary -> refuse, not crash
  r = run(fixture("missing", { [LINUX]: "v6.7.13_20260225 14:16:30.12079", [MD]: "v6.7.13_MacOS_20260529 15:38:00" }));
  check(r.status === 1 && /cannot read/.test(r.stderr), "missing framework binary fails with 'cannot read'");
  r = run(fixture("unstamped", {
    [LINUX]: Buffer.alloc(256),
    [MD]: "v6.7.13_MacOS_20260529 15:38:00",
    [TD]: "v6.7.13_MacOS_20260529 15:38:00",
  }));
  check(r.status === 1 && /no CTP version stamp/.test(r.stderr), "unstamped reference fails with 'no CTP version stamp'");

  // 5) the real vendored binaries parse: either the guard passes, or it fails on
  //    a genuine version difference (never on an unreadable/unstamped file)
  r = run();
  check(r.status === 0 || (r.status === 1 && !/cannot read|no CTP version stamp|ambiguous/.test(r.stderr)),
    `real ctpsdk binaries parse (guard status ${r.status})`);
  const stampOf = (rel) => [...readFileSync(join(root, rel)).toString("latin1").matchAll(/v(\d+\.\d+\.\d+)_\d{8} /g)].map((m) => m[1]);
  const ref = stampOf(LINUX);
  const others = ["ctpsdk/linux/thostmduserapi_se.so", "ctpsdk/windows/thostmduserapi_se.dll", "ctpsdk/windows/thosttraderapi_se.dll"];
  check(ref.length > 0 && others.every((rel) => stampOf(rel).every((v) => v === ref[0]) && stampOf(rel).length > 0),
    `Windows/Linux libraries are one CTP version (v${ref[0]})`);

  // 6) wiring: only the mac branch of binding.gyp runs the guard, and npm ships it
  const gyp = JSON.parse(readFileSync(join(root, "binding.gyp"), "utf8").replace(/#.*$/gm, ""));
  const branches = Object.fromEntries(gyp.targets[0].conditions.map(([cond, dict]) => [cond, dict]));
  check(branches["OS=='mac'"]?.variables?.ctp_macos_sdk_check === "<!(node scripts/check-macos-sdk.cjs)", "binding.gyp mac branch runs the guard");
  check(!JSON.stringify([branches["OS=='win'"], branches["OS=='linux'"]]).includes("check-macos-sdk"), "Windows/Linux branches do not run it");
  const pkg = JSON.parse(readFileSync(join(root, "package.json"), "utf8"));
  check(pkg.files.includes("scripts/check-macos-sdk.cjs"), "package.json files ships scripts/check-macos-sdk.cjs");
} finally {
  rmSync(tmp, { recursive: true, force: true });
}

console.log(`\nmacos-sdk-guard: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
