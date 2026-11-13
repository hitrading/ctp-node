# Releasing

Releases are **fully automatic** via
[semantic-release](https://semantic-release.gitbook.io/). You never bump the
version, tag, write release notes, or run `npm publish` by hand — candidates use
[Conventional Commits](https://www.conventionalcommits.org/) on `next`; verified
stable changes are promoted to `master`.

## How it works

On every push to `master` (stable) or `next` (`rc` prerelease), the CI
[`build & prebuild`](.github/workflows/build.yml) workflow:

1. builds + tests + prebuilds on Windows / Linux, plus a decoupled best-effort
   macOS build;
2. runs `semantic-release`, which reads the commits since the last release and:
   - **decides the next version** from the commit types (see below),
   - updates [`CHANGELOG.md`](CHANGELOG.md) and the `package.json` version,
   - creates a **GitHub Release** with auto-generated notes and the gated
     Windows/Linux prebuild bundle attached,
   - **publishes `@hitrading/ctp-node`** to npm (`--access public`, with npm
     provenance),
   - commits the changelog + version bump back to the current release branch
     (with `[skip ci]` so it doesn't loop).

If there are no release-worthy commits since the last release, it does nothing.
The macOS artifact is attached separately if its non-gating build succeeds; it
is not included in the npm tarball created by the release job.

## Commit message format

```
<type>(<optional scope>): <subject>

<optional body>

<optional footer>
```

| Commit type | Example | Release |
|---|---|---|
| `fix:` | `fix: reject a non-finite limit price in the order gate` | **patch** (0.0.x) |
| `feat:` | `feat: add reqQryDepthMarketData helper` | **minor** (0.x.0) |
| `feat!:` or a `BREAKING CHANGE:` footer | `feat!: rename arm() to armOrder()` | **major** (x.0.0) |
| `docs:` `chore:` `refactor:` `test:` `ci:` `perf:` `style:` | `docs: bilingual FAQ` | none (no release) |

`perf:` is configurable to a patch if you want; by default here it does not
release. Anything not matching a releasing type is bundled into the next release's
notes but doesn't itself trigger one.

## One-time setup (already in the repo, except the secret)

- `.releaserc.json` — the semantic-release plugin pipeline.
- `package.json` → `publishConfig: { access: public, provenance: true }`.
- The workflow's `release` job (`master` stable and `next` rc) with `contents`/`issues`/
  `pull-requests`/`id-token` write permissions.
- **You must add the `NPM_TOKEN` repository secret** — an npm **granular**
  access token with *Read and write* on the `@hitrading` scope and *Bypass 2FA*
  enabled (GitHub → Settings → Secrets and variables → Actions → New repository
  secret). The npm organization `hitrading` must exist
  (https://www.npmjs.com/org/create — free for public packages).

## Current breaking candidate

`v2.0.0` is the latest tagged baseline. The risk and lifecycle redesign is a
breaking `3.0.0` candidate and the worktree package identity is
`3.0.0-rc.1`. Do not publish a dirty build or reuse the `v2.0.0` identity.
Commit the candidate on `next`; semantic-release assigns an `rc` version and
records the exact tag/package integrity. Generate fresh prebuilds and run the
full verification plus SimNow/broker soak against that exact artifact. Promote
the verified commit to `master` only afterwards. A stable `v3.0.0` must not be
inferred merely from local unit tests.

## Dry run (optional, locally)

```sh
npx semantic-release --dry-run --no-ci   # needs GITHUB_TOKEN in the env
```
