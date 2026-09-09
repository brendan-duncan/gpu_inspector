# Releasing

GPU Inspector ships as a Windows installer (NSIS), a Debian package, and a macOS disk image,
built by `.github/workflows/release.yml` whenever a version tag is pushed. Installed builds
update themselves from the GitHub releases of this repository.

## Cutting a release

The version lives in one place, `app/package.json`; the tag must match it. `CHANGELOG.md` has
a section per version, the upcoming one at the top; check it is complete before tagging:

```
(cd app && npm version 0.2.0 --no-git-tag-version)
git commit -am "v0.2.0"
git tag v0.2.0
git push origin main v0.2.0
```

The workflow then, on a Windows, an Ubuntu 22.04 and a macOS 14 runner:

1. checks out the repository with the `Vulkan-Headers` submodule,
2. builds the layer with CMake (`-DVKINSP_BUILD_TESTS=OFF`, so no Vulkan SDK is needed) —
   skipped on macOS, which packages the UI alone,
3. checks that the tag matches the `version` in `app/package.json` (`v0.2.0` needs `0.2.0`)
   and fails with instructions if it does not,
4. runs `npm run dist:win` / `dist:linux` / `dist:mac` and uploads the installers, their block
   maps and the update manifests (`latest.yml` / `latest-linux.yml` / `latest-mac.yml`) as
   workflow artifacts,
5. once every platform has built, creates the GitHub release for the tag with all of those
   files attached and download links in the notes, and marks it as the latest release.

A build failure on any platform therefore leaves no release behind; fix, delete the tag
(`git push --delete origin vX.Y.Z; git tag -d vX.Y.Z`) and tag again. Running the workflow by
hand (workflow_dispatch) only builds; the installers are attached to the workflow run.

Release assets:

| Platform | File |
|---|---|
| Windows | `GPU-Inspector-Setup-<version>.exe` (+ `.blockmap`, `latest.yml`) |
| Linux | `gpu-inspector_<version>_amd64.deb` (+ `latest-linux.yml`) |
| macOS | `GPU-Inspector-<version>-{arm64,x64}.dmg` and `.zip` (+ `.blockmap`, `latest-mac.yml`) |

The Windows installer is not code-signed, so SmartScreen warns on first run; the updater still
verifies downloads against the SHA-512 in the manifest. The macOS build is signed and notarized
(below). The mac `.zip` is not just a second download format — it is the archive
`electron-updater` installs from, so it must be published alongside the `.dmg` for self-update
to work.

## macOS signing and notarization

Without this a downloaded build does not open at all: macOS quarantines it and reports it as
damaged or from an unidentified developer. Five repository secrets drive it, read from the
environment by electron-builder in the macOS build step of the workflow:

| Secret | What it is |
|---|---|
| `MAC_CERTS` | the *Developer ID Application* certificate and its private key, exported from Keychain Access as a `.p12` and base64-encoded (`base64 -i cert.p12 \| pbcopy`) |
| `MAC_CERTS_PASSWORD` | the password given when exporting the `.p12` |
| `APPLE_ID` | the Apple ID of the developer account |
| `APPLE_APP_SPECIFIC_PASSWORD` | an app-specific password for that Apple ID, from https://account.apple.com (*not* the account password) |
| `APPLE_TEAM_ID` | the ten-character team identifier, `W2FKLA6G2Y` |

GitHub secrets are per-repository, so these have to exist on this repository even if the same
values are already set on another.

`app/electron-builder.yml` leaves `mac.identity` unset on purpose: electron-builder then finds
the certificate itself, from the keychain on a developer's Mac and from `CSC_LINK` /
`CSC_KEY_PASSWORD` in CI. `hardenedRuntime: true` is what notarization requires, and
electron-builder's default entitlements (`allow-jit`, `allow-unsigned-executable-memory`,
`disable-library-validation`) are all Electron needs — the app is not sandboxed, so there is no
entitlements file of our own to maintain.

Notarization runs after signing, once per architecture, and staples Apple's ticket to the bundle
before the `.dmg` and `.zip` are built from it, so both carry it. It adds a few minutes per
architecture. Neither step is mandatory for a build to succeed: with no certificate
electron-builder logs `skipped macOS code signing`, with no Apple credentials it logs `skipped
macOS notarization`, and `app/tools/adhoc_sign.cjs` leaves the bundle ad-hoc signed so a
contributor without an Apple Developer account can still build and run it.

To check a finished build, `spctl -a -vv -t exec "GPU Inspector.app"` should say `accepted`
with `source=Notarized Developer ID`. `source=Unnotarized Developer ID` means signing worked but
notarization did not run.

## How the installer is put together

`app/electron-builder.yml` is the configuration. Beyond the app bundle it ships:

- `resources/layer/`: the layer library and its manifest, copied from `build/bin[/Release]`
  into `app/dist/layer` by `npm run stage:layer` (`app/tools/stage_layer.mjs`;
  `INSPECTOR_LAYER_DIR` overrides the source). The app finds them there through
  `findLayerDir` in `app/src/main/main.ts`, the same lookup that finds a development build.
  On macOS there is no layer to stage, so the directory holds only the Android layer if it was
  built, and nothing at all otherwise.
- `resources/assets/`: the window icons.
- `resources/app-update.yml`: the update feed (generated by electron-builder from `publish`).

The `.deb` installs to `/opt/GPU Inspector` with a `gpu-inspector` launcher in `/usr/bin` and a
`gpu-inspector.desktop` entry whose `StartupWMClass` matches the `desktopName` in
`app/package.json`, so GNOME associates the running window with its icon. It depends on
`libvulkan1` in addition to Electron's usual libraries.

## Self-update

`electron-updater` runs in the main process (`main.ts`, "Self-update"). Three seconds after
startup an installed build checks the feed; failures are silent (offline, no release yet). The
version label at the right of the launch bar checks again on click and reports the result.
An available update is only downloaded when the user clicks **Download**; a downloaded update is
installed when the app closes, or right away with **Restart and Install** (which stops the
inspected applications first, like quitting does).

On Windows the NSIS installer runs silently over the existing installation. On Linux the
updater downloads the new `.deb` and installs it with `dpkg` through `pkexec`, which asks for
the user's password. On macOS it downloads the `.zip` for the running architecture and swaps
the app bundle in place.

`npm start` never checks for updates (`app.isPackaged` is false); the version label says so.

## Building an installer locally

```
cmake --build build --config Release     # the layer
cd app
npm run pack                             # unpacked app in app/release/<platform>-unpacked
npm run dist:win                         # app/release/GPU-Inspector-Setup-<version>.exe
npm run dist:linux                       # app/release/gpu-inspector_<version>_amd64.deb
npm run dist:mac                         # app/release/GPU-Inspector-<version>-{arm64,x64}.dmg
```

`dist:mac` needs no layer build and builds both Mac architectures from either one. It signs with
whatever Developer ID is in the keychain; to notarize locally as well, set the same three
variables the workflow uses:

```
export APPLE_ID=... APPLE_APP_SPECIFIC_PASSWORD=... APPLE_TEAM_ID=W2FKLA6G2Y
npm run dist:mac
```

`npm run dist` only builds; `gh release create vX.Y.Z app/release/*` (or `-- --publish always`
with `GH_TOKEN` set) publishes from a machine instead of the workflow.
