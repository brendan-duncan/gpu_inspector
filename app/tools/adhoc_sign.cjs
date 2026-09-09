// electron-builder afterPack hook: ad-hoc code signing for macOS, as a fallback.
//
// Released builds are signed with the project's Developer ID and notarized, and this hook is
// irrelevant to them: afterPack runs before electron-builder signs, so the real signature simply
// replaces the ad-hoc one. It exists for builds made without a certificate — a contributor's
// local `npm run dist:mac`, or a fork's CI without the secrets.
//
// What electron-builder leaves behind in that case is not an unsigned bundle but a broken one:
// the executable keeps the linker-signed ad-hoc signature that Electron's prebuilt binary came
// with, while the resources it seals have been replaced wholesale. The app still launches, but
// `codesign --verify` fails with "code has no resources but signature indicates they must be
// present", and that is also what Gatekeeper would see in a copy of it — an invalid signature
// rather than a missing one, which is the "the application is damaged and can't be opened" path
// rather than the ordinary unidentified-developer one.
//
// Re-signing the whole bundle ad-hoc (`codesign --sign -`) needs no certificate and re-seals the
// resources, so the signature is valid again. Removing the signature altogether is not an option:
// Apple Silicon will not execute a bundle whose main executable carries no signature at all.
const { execFileSync } = require("node:child_process");
const path = require("node:path");

exports.default = async function adhocSign(context) {
  if (context.electronPlatformName !== "darwin") return;
  const app = path.join(context.appOutDir, `${context.packager.appInfo.productFilename}.app`);
  // --deep so the nested helpers and the Electron Framework are re-sealed too: they carry the
  // same linker signature. No hardened runtime, which only goes with notarization and without
  // entitlements would just take capabilities away from an app that cannot be notarized.
  execFileSync("codesign", ["--force", "--deep", "--sign", "-", app], { stdio: "inherit" });
  execFileSync("codesign", ["--verify", "--deep", app], { stdio: "inherit" });
  console.log(`  • ad-hoc signed  file=${app}`);
};
