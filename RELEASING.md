# Releasing

Releases are **fully automatic** via
[semantic-release](https://semantic-release.gitbook.io/). You never bump the
version, tag, write release notes, or run `npm publish` by hand — you just merge
[Conventional Commits](https://www.conventionalcommits.org/) into `master`.

## How it works

On every push to `master`, the CI [`build & prebuild`](.github/workflows/build.yml)
workflow:

1. builds + tests + prebuilds on Windows / Linux / macOS (macOS is best-effort);
2. runs `semantic-release`, which reads the commits since the last release and:
   - **decides the next version** from the commit types (see below),
   - updates [`CHANGELOG.md`](CHANGELOG.md) and the `package.json` version,
   - creates a **GitHub Release** with auto-generated notes and the all-platform
     prebuilds attached,
   - **publishes `@hitrading/ctp-node`** to npm (`--access public`, with npm
     provenance),
   - commits the changelog + version bump back to `master` (with `[skip ci]` so it
     doesn't loop).

If there are no release-worthy commits since the last release, it does nothing.

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
- The workflow's `release` job (master only) with `contents`/`issues`/
  `pull-requests`/`id-token` write permissions.
- **You must add the `NPM_TOKEN` repository secret** — an npm **granular**
  access token with *Read and write* on the `@hitrading` scope and *Bypass 2FA*
  enabled (GitHub → Settings → Secrets and variables → Actions → New repository
  secret). The npm organization `hitrading` must exist
  (https://www.npmjs.com/org/create — free for public packages).

## First release

semantic-release's first automatic release defaults to **`1.0.0`**. If you'd
rather start at `0.x`, create a starting tag once before the first releasing
commit, e.g.:

```sh
git tag v0.1.0 && git push origin v0.1.0
```

then your next `feat:`/`fix:` commit releases `0.1.1` / `0.2.0` from there.

## Dry run (optional, locally)

```sh
npx semantic-release --dry-run --no-ci   # needs GITHUB_TOKEN in the env
```
