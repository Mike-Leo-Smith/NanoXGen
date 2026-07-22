#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ARCHIVE="${ROOT_DIR}/third_party/downloads/Autodesk_Maya_2026_3_Update_DEVKIT_Linux.tgz"
DEST="${ROOT_DIR}/third_party/maya-devkit-2026.3"
URL='https://autodesk-adn-transfer.s3.us-west-2.amazonaws.com/ADN+Extranet/M%26E/Maya/devkit+2026/Autodesk_Maya_2026_3_Update_DEVKIT_Linux.tgz'
SHA256='d23cc9e788a0114c683983363e28b08f040b900ab728bbf4707baee2dc563c37'

mkdir -p "$(dirname "${ARCHIVE}")" "${DEST}"
if [[ ! -f "${ARCHIVE}" ]]; then
  curl -L --fail --retry 2 -o "${ARCHIVE}" "${URL}"
fi
echo "${SHA256}  ${ARCHIVE}" | sha256sum --check
tar --no-same-owner -xzf "${ARCHIVE}" -C "${DEST}" --strip-components=1
echo "Maya DevKit extracted to ${DEST}"
echo "Note: Autodesk ships XGen headers and libAdskXGen with Maya, not this DevKit."
