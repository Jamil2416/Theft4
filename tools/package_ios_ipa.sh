#!/bin/zsh
set -euo pipefail

usage() {
  print -u2 "Usage: $0 /path/to/Theft4.app [output-directory]"
  print -u2 "Creates a sideload-ready IPA (ad-hoc signed, no developer identity) and SHA-256 checksum."
  print -u2 "Set THEFT4_IPA_INCREASED_MEMORY=0 for a signature-free IPA."
}

if (( $# < 1 || $# > 2 )); then
  usage
  exit 64
fi

app_path="${1:A}"
output_dir="${2:-${PWD}/dist}"
output_dir="${output_dir:A}"

if [[ ! -d "$app_path" || "${app_path:e}" != app ]]; then
  print -u2 "Expected an iOS .app bundle: $app_path"
  exit 66
fi

info_plist="$app_path/Info.plist"
if [[ ! -f "$info_plist" ]]; then
  print -u2 "Missing Info.plist: $info_plist"
  exit 66
fi

plist_value() {
  /usr/libexec/PlistBuddy -c "Print :$1" "$info_plist" 2>/dev/null
}

executable_name="$(plist_value CFBundleExecutable)"
bundle_identifier="$(plist_value CFBundleIdentifier)"
version="$(plist_value CFBundleShortVersionString)"
build_version="$(plist_value CFBundleVersion)"
executable_path="$app_path/$executable_name"

if [[ -z "$executable_name" || ! -f "$executable_path" ]]; then
  print -u2 "Info.plist does not identify an existing executable."
  exit 65
fi

architectures="$(/usr/bin/lipo -archs "$executable_path" 2>/dev/null || true)"
if [[ " $architectures " != *" arm64 "* ]]; then
  print -u2 "The app executable is not an ARM64 device build: ${architectures:-unknown}"
  exit 65
fi
if [[ " $architectures " == *" x86_64 "* || " $architectures " == *" i386 "* ]]; then
  print -u2 "Simulator architecture found in device release: $architectures"
  exit 65
fi

# Public packages must not reveal the builder's account name or checkout path.
# THEFT4_PUBLIC_BUILD applies stable prefix maps; this check catches a build
# made without it as well as similarly leaking prebuilt dependencies.
local_path_sample="$(/usr/bin/strings "$executable_path" | /usr/bin/grep -E -m 1 '^(/Users/|/home/|[A-Za-z]:\\\\Users\\\\)' || true)"
if [[ -n "$local_path_sample" ]]; then
  print -u2 "Refusing to package an executable containing a local source path:"
  print -u2 "  $local_path_sample"
  print -u2 "Reconfigure with -DTHEFT4_PUBLIC_BUILD=ON and rebuild Release."
  exit 65
fi

# The public app imports a lawful user-provided installation at runtime. A
# packaged retail executable, archive, disc image, or title update is a release
# error even when it was copied into the bundle accidentally.
game_payload="$(/usr/bin/find "$app_path" -type f \( \
  -iname '*.xex' -o -iname '*.xexp' -o -iname '*.rpf' -o \
  -iname '*.iso' -o -iname '*.xcp' \) -print -quit)"
if [[ -n "$game_payload" ]]; then
  print -u2 "Refusing to package bundled game data: $game_payload"
  exit 65
fi

safe_version="${version//[^A-Za-z0-9._-]/-}"
safe_build="${build_version//[^A-Za-z0-9._-]/-}"
base_name="Theft4-${safe_version}-${safe_build}-ios-arm64"
ipa_path="$output_dir/$base_name.ipa"
checksum_path="$ipa_path.sha256"

stage_root="$(/usr/bin/mktemp -d "${TMPDIR:-/tmp}/theft4-ipa.XXXXXX")"
trap '/bin/rm -rf -- "$stage_root"' EXIT INT TERM
/bin/mkdir -p "$stage_root/Payload" "$output_dir"
/usr/bin/ditto --norsrc --noextattr --noqtn "$app_path" "$stage_root/Payload/Theft4.app"
staged_app="$stage_root/Payload/Theft4.app"

# A development profile restricts installation to registered devices and also
# publishes team/device metadata. Sideloading tools must supply the user's own
# signature and provisioning profile instead.
if /usr/bin/codesign -dv "$staged_app" >/dev/null 2>&1; then
  /usr/bin/codesign --remove-signature "$staged_app"
fi
/bin/rm -rf -- "$staged_app/_CodeSignature"
/bin/rm -f -- "$staged_app/embedded.mobileprovision"

if [[ -e "$staged_app/_CodeSignature" || -e "$staged_app/embedded.mobileprovision" ]]; then
  print -u2 "Failed to remove development signing material from the staged app."
  exit 70
fi

# Avoid publishing Finder quarantine/resource-fork metadata as __MACOSX
# entries. None of it is part of the iOS application payload. This runs before
# any ad-hoc signing so the signature seals the final file set.
/usr/bin/xattr -cr "$staged_app"
# Imported AppleDouble sidecars are ordinary files, so xattr clearing alone
# cannot remove them. They are metadata, never iOS application resources.
/usr/bin/find "$staged_app" -type f -name '._*' -delete

# Sideloading tools read the entitlements to request from the executable's
# signature, so a signature-free IPA cannot ask for the increased memory limit
# that matters most on 8 GiB devices. An ad-hoc signature carries it without
# team, device or provisioning metadata; the user's tool still re-signs it.
# THEFT4_IPA_INCREASED_MEMORY=0 restores the previous signature-free package.
signing_summary="removed; the user's sideloading tool must re-sign the app"
if [[ "${THEFT4_IPA_INCREASED_MEMORY:-1}" != 0 ]]; then
  entitlements="${0:A:h:h}/ios/Theft4/Theft4Sideload.entitlements"
  if [[ ! -f "$entitlements" ]]; then
    print -u2 "Missing sideload entitlements: $entitlements"
    exit 66
  fi
  # Nested code must be signed before its enclosing bundle; -depth visits the
  # innermost items first. Only the main executable requests entitlements.
  /usr/bin/find "$staged_app" -depth -mindepth 1 \( -name '*.framework' -o \
    -name '*.dylib' -o -name '*.appex' \) -print0 |
    while IFS= read -r -d '' nested; do
      /usr/bin/codesign --force --sign - "$nested"
    done
  /usr/bin/codesign --force --sign - --entitlements "$entitlements" \
    --generate-entitlement-der "$staged_app"
  # Capture before matching: grep -q can close the pipe early, and pipefail
  # would then report codesign's SIGPIPE as a failed check.
  signed_entitlements="$(/usr/bin/codesign -d --entitlements - --xml "$staged_app" 2>/dev/null || true)"
  if [[ "$signed_entitlements" != *com.apple.developer.kernel.increased-memory-limit* ]]; then
    print -u2 "The ad-hoc signature does not carry the increased memory limit request."
    exit 70
  fi
  signature_details="$(/usr/bin/codesign -dv "$staged_app" 2>&1 || true)"
  if [[ "$signature_details" != *$'\n'TeamIdentifier=not\ set* ]]; then
    print -u2 "The staged app signature unexpectedly names a development team."
    exit 70
  fi
  if [[ -e "$staged_app/embedded.mobileprovision" ]]; then
    print -u2 "Ad-hoc signing unexpectedly added a provisioning profile."
    exit 70
  fi
  signing_summary="ad-hoc, requests increased-memory-limit; the user's sideloading tool must re-sign the app"
fi

(
  cd "$stage_root"
  /usr/bin/ditto --norsrc --noextattr --noqtn -c -k --keepParent Payload "$ipa_path"
)
/usr/bin/unzip -tq "$ipa_path" >/dev/null
python3 - "$ipa_path" <<'PY'
import sys, zipfile
with zipfile.ZipFile(sys.argv[1]) as archive:
    names = archive.namelist()
    if any(part == '__MACOSX' or part.startswith('._')
           for name in names for part in name.split('/')):
        raise SystemExit('Refusing to package Finder/AppleDouble metadata')
    if len(names) != len(set(names)):
        raise SystemExit('Refusing to package duplicate archive entries')
PY

checksum="$(/usr/bin/shasum -a 256 "$ipa_path" | /usr/bin/awk '{print $1}')"
print "$checksum  ${ipa_path:t}" > "$checksum_path"

print "Created sideload-ready IPA"
print "  App:        $bundle_identifier $version ($build_version)"
print "  Architect:  $architectures"
print "  IPA:        $ipa_path"
print "  SHA-256:    $checksum"
print "  Signing:    $signing_summary"
