#!/usr/bin/env bash
# 本地 Release：编译 → adhoc 签名 → DMG + ZIP（产物在 .release/，已 gitignore）
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

VERSION="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' Sources/MacBoardApp/Info.plist)"
BUILD="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' Sources/MacBoardApp/Info.plist)"
OUT="${ROOT}/.release"
DERIVED="${OUT}/DerivedData"
APP_NAME="MacBoard"
SCHEME="MacBoard"

echo "==> MacBoard ${VERSION} (${BUILD})"

rm -rf "${OUT}/stage" "${OUT}/dmg"
mkdir -p "${OUT}"

xcodebuild \
  -project MacBoard.xcodeproj \
  -scheme "${SCHEME}" \
  -configuration Release \
  -derivedDataPath "${DERIVED}" \
  CODE_SIGN_IDENTITY="-" \
  CODE_SIGNING_ALLOWED=YES \
  build

APP="${DERIVED}/Build/Products/Release/${APP_NAME}.app"
test -d "${APP}"

STAGE="${OUT}/stage/${APP_NAME}.app"
rm -rf "${OUT}/stage"
mkdir -p "${OUT}/stage"
ditto "${APP}" "${STAGE}"

codesign --force --deep --options runtime --sign - "${STAGE}"

DMG_ROOT="${OUT}/dmg"
rm -rf "${DMG_ROOT}"
mkdir -p "${DMG_ROOT}/Applications"
ditto "${STAGE}" "${DMG_ROOT}/Applications/${APP_NAME}.app"
ln -s /Applications "${DMG_ROOT}/Applications Link"

DMG="${OUT}/${APP_NAME}-v${VERSION}.dmg"
ZIP="${OUT}/${APP_NAME}-v${VERSION}-macOS.zip"
rm -f "${DMG}" "${ZIP}"

hdiutil create -volname "${APP_NAME} ${VERSION}" -srcfolder "${DMG_ROOT}" -ov -format UDZO "${DMG}"
ditto -c -k --keepParent "${STAGE}" "${ZIP}"

echo "==> ${DMG}"
echo "==> ${ZIP}"
