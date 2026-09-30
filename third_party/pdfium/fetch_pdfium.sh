#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# third_party/pdfium/fetch_pdfium.sh — vendor the BOOTSTRAP PDFium binary.
#
# READ third_party/pdfium/README.md FIRST. This script fetches a PREBUILT
# binary from a third party. That is explicitly NOT what SP-1 recommends for
# production (spikes/SP-1-findings.md §5: build from pinned source with
# upstream's GN toolchain in a hermetic CI container). It exists so that the
# pdf-engine/reader wrapper can be written, compiled and tested now.
#
# WHY A SCRIPT AND NOT FetchContent / file(DOWNLOAD) AT CONFIGURE TIME
# ---------------------------------------------------------------------------
# `cmake -S . -B build` must work with no network. That is the same reason
# tests/support/test_framework.h gives for not pulling in Catch2, and it is a
# stronger rule here: this is an offline-first product. So the network step is
# an explicit, auditable, one-time command a human (or CI) runs, and CMake only
# ever LOOKS at the filesystem. If the fetch has not happened, UAPP_ENABLE_PDF
# defaults OFF and the rest of the build is unaffected.
#
# Usage:
#   third_party/pdfium/fetch_pdfium.sh            # host platform
#   third_party/pdfium/fetch_pdfium.sh mac-arm64  # explicit
#
# Everything it writes lands in third_party/pdfium/<platform>/ and is
# .gitignore'd. The pinned tag and the expected SHA-256 live in VERSION, which
# IS committed — the checksum is the audit trail, not the bytes.

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# --- the pin ---------------------------------------------------------------
# Kept in lockstep with third_party/pdfium/VERSION. A floating "latest" is not
# acceptable: upstream publishes a new branch head every Monday, and a
# dependency that changes underneath us is not a dependency we can audit.
PDFIUM_TAG="chromium/8076"
PDFIUM_VERSION="156.0.8076.0"
BASE_URL="https://github.com/bblanchon/pdfium-binaries/releases/download/${PDFIUM_TAG}"

# platform -> sha256 of the release tarball. Add a row per platform as each is
# vendored; an unlisted platform is refused rather than fetched unverified.
sha_for_platform() {
    case "$1" in
        mac-arm64) echo "0d6781fe08906baff3d82c90953e519fbc4eb253fe76431e5ed53b157763b97c" ;;
        *)         echo "" ;;
    esac
}

platform="${1:-}"
if [[ -z "${platform}" ]]; then
    case "$(uname -s)/$(uname -m)" in
        Darwin/arm64) platform="mac-arm64" ;;
        Darwin/x86_64) platform="mac-x64" ;;
        Linux/aarch64) platform="linux-arm64" ;;
        Linux/x86_64) platform="linux-x64" ;;
        *)
            echo "fetch_pdfium: cannot infer a platform from $(uname -s)/$(uname -m)." >&2
            echo "              Pass one explicitly, e.g. 'mac-arm64'." >&2
            exit 2
            ;;
    esac
fi

expected_sha="$(sha_for_platform "${platform}")"
if [[ -z "${expected_sha}" ]]; then
    echo "fetch_pdfium: no pinned SHA-256 recorded for platform '${platform}'." >&2
    echo "              Only platforms listed in third_party/pdfium/VERSION are" >&2
    echo "              vendored. Fetching an unverified archive is worse than" >&2
    echo "              not building PDF support, so this is a hard stop." >&2
    exit 2
fi

archive_name="pdfium-${platform}.tgz"
url="${BASE_URL}/${archive_name}"
dest="${here}/${platform}"
tmp="$(mktemp -d)"
trap 'rm -rf "${tmp}"' EXIT

echo "fetch_pdfium: tag       ${PDFIUM_TAG} (PDFium ${PDFIUM_VERSION})"
echo "fetch_pdfium: platform  ${platform}"
echo "fetch_pdfium: url       ${url}"

curl -fsSL --retry 3 -o "${tmp}/${archive_name}" "${url}"

if command -v shasum >/dev/null 2>&1; then
    actual_sha="$(shasum -a 256 "${tmp}/${archive_name}" | awk '{print $1}')"
else
    actual_sha="$(sha256sum "${tmp}/${archive_name}" | awk '{print $1}')"
fi

if [[ "${actual_sha}" != "${expected_sha}" ]]; then
    echo "fetch_pdfium: CHECKSUM MISMATCH — refusing to install." >&2
    echo "              expected ${expected_sha}" >&2
    echo "              actual   ${actual_sha}" >&2
    echo "  Either the pin in VERSION is stale, or the artefact changed. Do not" >&2
    echo "  'fix' this by updating the hash without establishing why it moved." >&2
    exit 1
fi
echo "fetch_pdfium: sha256    ${actual_sha}  OK"

rm -rf "${dest}"
mkdir -p "${dest}"
tar xzf "${tmp}/${archive_name}" -C "${dest}"

# The licence texts are the one part of the payload that must be reviewable in
# the repository without running this script (THIRD_PARTY_NOTICES rule 2: a
# directory under third_party/ without a LICENSE file fails CI). Copy them out
# of the extracted, .gitignore'd tree into the committed one.
mkdir -p "${here}/licenses"
# third_party/<name>/LICENSE is what THIRD_PARTY_NOTICES process item 1 and
# rule 2 (NFR-8) require, and it must be the UPSTREAM PROJECT'S licence —
# PDFium's BSD-3 — not the distributor's MIT wrapper, which lands in
# licenses/ alongside everything else.
cp "${dest}/licenses/pdfium.txt" "${here}/LICENSE"
cp "${dest}/LICENSE" "${here}/licenses/LICENSE.pdfium-binaries"
cp "${dest}"/licenses/*.txt "${dest}"/licenses/*.md "${dest}"/licenses/*.ijg \
   "${here}/licenses/" 2>/dev/null || true
cp "${dest}/args.gn" "${here}/args.gn.${platform}"

echo "fetch_pdfium: installed ${dest}"
echo "fetch_pdfium: licences  ${here}/licenses"
echo
echo "Now configure with PDF support:"
echo "    cmake -S . -B build -DUAPP_ENABLE_PDF=ON"
