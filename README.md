# BG3Access Release Manifest Branch

This is an **orphan branch** in the bg3se-SR repo dedicated to update-server
infrastructure. It has no shared history with the code branches
(`main`, `feature/tolk-bindings`, etc.) and is intentionally minimal.

## What lives here

```
Channels/
  Release/
    Manifest.json    -- stable, public-friendly builds
  Beta/
    Manifest.json    -- pre-release builds for testers
```

## What the loader does with this

The BG3Updater loader (the `DWrite.dll` users drop in their BG3 `bin/`
folder) is built with this constant in `BG3Updater/Defines.h`:

```cpp
#define UPDATER_MANIFEST_URL "https://raw.githubusercontent.com/IndomitableHeart/bg3se-SR/release-manifest/Channels/"
```

At launch the loader fetches `{URL}{Channel}/Manifest.json`, parses it,
finds the highest-versioned entry whose `MinGameVersion` matches the
player's BG3 build, downloads the corresponding signed `.zip` package
from its `URL` field, verifies the package signature against the public
key baked into the loader, unpacks the contained `BG3ScriptExtender.dll`
into the user's cache, and loads it.

## Channels

- **Release**: default channel. Users get this unless they ship a
  `ScriptExtenderUpdaterConfig.json` overriding `UpdateChannel`.
- **Beta**: opt-in via the config file override. Used during private
  testing.

## How to add a new release

1. Build `BG3ScriptExtender.dll` in Release config.
2. Zip it as `ScriptExtender-<version>.zip` (contents: just the DLL,
   optionally sibling support files).
3. Sign the zip:
   `UpdateSigner.exe sign D:\BG3Access\bg3access_signing.priv.key ScriptExtender-<version>.zip`
4. Upload the signed zip as an asset on a new GitHub release in this
   repo (`IndomitableHeart/bg3se-SR`).
5. Update the channel's `Manifest.json` by running:
   `UpdateSigner.exe update-manifest <Manifest.json> ScriptExtender <signed-zip-path> <BG3ScriptExtender.dll-path> <MinGameVersion> <MaxGameVersion> <RootURL>`
6. Commit and push this branch.

The loader will see the new entry on the next game launch.

## What does NOT go on this branch

Source code, build outputs, documentation, anything unrelated to
manifests. Keep it deliberately tiny -- the branch is consulted on
every game launch by every user, and any extra payload here adds to
the git fetch cost when CDN cache misses.
