# guid-a-tron

Sets the federationGuid on all elements in an iModel throughout its entire history.

A native CLI. It copies an iModel seed or baseline file, sets `federationGuid` in every element that has none, and
then copies all changesets and sets `federationGuid` for all of history.

To *GUIDate* is to add guids to an element or an iModel.

## Prerequisites

A C++20 compiler, CMake 3.24.

Dependencies:

| dependency | where it comes from |
| --- | --- |
| SQLite 3.53.4 | the **public amalgamation**, downloaded and checksummed by `cmake/dependencies.cmake`, compiled here with the flags a `.bim` needs |
| the LZMA SDK | vendored in `third_party/lzma`, copied from the iTwin native source — see the README there for why that copy and not a fresh download |
| nlohmann/json | one header, downloaded and checksummed, for `manifest.json` |

Downloads are cached in `third_party/download`.

## Build

```bash
scripts/build.sh            # Release into out/Release/bin/guid-a-tron
scripts/build.sh --debug    # Debug instead
scripts/build.sh --test     # build, then run the unit tests
```

| variable | default | what it is |
| --- | --- | --- |
| `GUID_A_TRON_DEPS_DIR` | `third_party/download` | the download cache, e.g. a pre-populated one for an offline build |

## Commands

```
# Create a copy of the input file and set all fedGUIDs
guid-a-tron seed     --baseline <file> --out <dir> [--guids random|derived] [--limit <n>]
# Rewrite a single changeset
guid-a-tron rewrite  --fork <dir> --changeset <file> --index <n> [--guid-column <n>] [--description <text>] [--force]
# Rewrite all changesets listed
guid-a-tron rewrite  --fork <dir> --changeset-list <file> [--guid-column <n>] [--description <text>] [--force]
guid-a-tron scan     --changeset <file> | --changeset-list <file> [--guid-column <n>]
guid-a-tron coverage --imodel <file>
guid-a-tron id       --changeset <file> [--parent <changesetId>]
```

| command | what it does |
| --- | --- |
| `seed` | copies the baseline, GUIDates every element with no `FederationGuid`, and creates `guids.db` |
| `rewrite` | copies an original changeset into the fork, writing a guid into every NULL `FederationGuid` field in its bytes |
| `scan` | outputs a summary of the contents of the provided changesets |
| `coverage` | how many elements in any iModel have a guid |
| `id` | computes a changeset's id |

Exit codes: `0` success, `1` a failure (the message says what), `2` bad arguments, `3` a `seed` that
left elements without a guid, `4` a `scan` with a changeset that is unreadable or does not
round-trip to the same id.

guid-a-tron does not apply changesets.  Use iTwin.js or imodulator CLI to build a local checkpoint or upload the fork as a new iModel.

## Output

```
my-fork/
  seed.bim                      the baseline, with a guid on every element
  guids.db                      one row per element id this tool gave a guid to
  changesets/NNNNN-<id>.cs      the rewritten changesets, index-prefixed
  manifest.json                 what produced the fork, and what was done to each changeset
```

## Guid policy

| `--guids` | what it does |
| --- | --- |
| `random` (default) | 16 random bytes per element incarnation, version 4 |
| `derived` | SHA1 over (scope guid, element id, incarnation), shaped as a UUIDv5 |

An *incarnation* is one life of an element id: an id that is deleted and later re-inserted is a
new element and gets a new guid.

Either way the choice is recorded in `guids.db` and in the manifest. `rewrite` uses the policy the
seed recorded; passing a `--guids` that disagrees with it is an error.

## Troubleshooting

| symptom | cause |
| --- | --- |
| `cannot download the SQLite amalgamation` | no network on a first build; fetch the URL in `cmake/dependencies.cmake` by hand into `third_party/download` |
| `… has no bis_Element.FederationGuid column` | the `--baseline` is not an iModel, or not one with a BIS profile |
| `no guid store at … — run guid-a-tron seed first` | `--fork` has no `guids.db`; the seed creates it |
| `changeset … is at or behind the store's position` | the list is out of order, or an earlier run already rewrote that index; resume from the next index |
| a `scan` reporting `DIFFERS` | the input is not a changeset this tool can read losslessly, so `rewrite` would produce one whose id has moved |
| `unresolved` above 0 | the guid store does not cover the history being rewritten — typically a fork begun partway through it |

## Implementation

[`IMPLEMENTATION.md`](IMPLEMENTATION.md) has the internals: SQLite's changeset record format, the
file container and its prefix, the changeset id, the rewriter's state machine and substitution
policy, the guid store's schema, the timestamp trigger, and why nothing is applied.

## Licenses

Uses some minor bits of source code from [imodel-native](https://github.com/iTwin/imodel-native) and
[itwinjs-core](https://github.com/iTwin/itwinjs-core), both MIT-licensed. SQLite and the LZMA SDK
are public domain; nlohmann/json is MIT-licensed.
