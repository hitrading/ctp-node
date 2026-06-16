/*
 * test-cpp.mjs - build + run the standalone C++ unit tests for the pure-logic
 * native classes (RiskEngine, ArmRegistry). Links the .cc sources DIRECTLY: no
 * N-API, no addon, no test-only hooks in the shipped library.
 *
 *   npm run test:cpp              # build + run (correctness gate)
 *   COVERAGE=1 npm run test:cpp   # + report per-file coverage
 *
 * Compiler: $CXX (default clang++; CI/Linux uses g++). COVERAGE uses clang's
 * source-based coverage (llvm-cov) or g++'s gcov, whichever matches $CXX.
 */
import { execFileSync } from "node:child_process";
import { mkdirSync } from "node:fs";

const CXX = process.env.CXX || "clang++";
const coverage = process.env.COVERAGE === "1";
const isClang = /clang/.test(CXX);
const exe = process.platform === "win32" ? ".exe" : "";
mkdirSync("build/cpptest", { recursive: true });

const suites = [
  { name: "test_risk", src: "src/native/risk.cc", test: "test/cpp/test_risk.cpp" },
  { name: "test_arm", src: "src/native/arm.cc", test: "test/cpp/test_arm.cpp" },
];

const base = ["-std=c++17", "-Isrc/native"];
const covFlags = !coverage
  ? ["-O0"]
  : isClang
    ? ["-fprofile-instr-generate", "-fcoverage-mapping"] // NB: adding -g corrupts the profraw on Windows clang
    : ["-O0", "--coverage"];

const run = (cmd, args, env) => execFileSync(cmd, args, { stdio: "inherit", env: { ...process.env, ...env } });

let failed = false;
for (const s of suites) {
  const out = `build/cpptest/${s.name}${exe}`;
  try {
    run(CXX, [...base, ...covFlags, s.test, s.src, "-o", out]);
  } catch {
    console.error(`${s.name}: COMPILE FAILED`);
    failed = true;
    continue;
  }
  try {
    console.log(`\n=== ${s.name} ===`);
    run(out, [], coverage && isClang ? { LLVM_PROFILE_FILE: `build/cpptest/${s.name}.profraw` } : {});
  } catch {
    console.error(`${s.name}: TESTS FAILED`);
    failed = true;
  }
}

// Coverage report (best-effort: never fails the run if the tools are absent).
if (coverage && !failed) {
  console.log("\n=== C++ coverage (standalone; no production pollution) ===");
  try {
    if (isClang) {
      for (const s of suites) {
        run("llvm-profdata", ["merge", "-sparse", `build/cpptest/${s.name}.profraw`, "-o", `build/cpptest/${s.name}.profdata`]);
        run("llvm-cov", ["report", `build/cpptest/${s.name}${exe}`, `-instr-profile=build/cpptest/${s.name}.profdata`, s.src]);
      }
    } else {
      run("gcov", ["-n", ...suites.map((s) => s.src)]);
    }
  } catch {
    console.warn("(coverage report tool unavailable - tests still ran and passed)");
  }
}

process.exit(failed ? 1 : 0);
