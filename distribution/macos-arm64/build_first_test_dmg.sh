#!/bin/bash

set -euo pipefail

script_dir="$(cd "$(dirname "$0")" && pwd)"
project_root="$(cd "${script_dir}/../.." && pwd)"
build_root="${project_root}/build-distribution-arm64"
release_build="${project_root}/build-release-arm64"
runtime_build="${build_root}/runtime"
stage_root="${build_root}/stage"
app_stage="${stage_root}/Synthetic Obsidian.app"
dmg_stage="${build_root}/dmg-root"
output_dir="${project_root}/dist"
output_dmg="${output_dir}/Synthetic-Obsidian-1.0-Apple-Silicon.dmg"
identity="${SYNTHETIC_OBSIDIAN_CODESIGN_IDENTITY:-Developer ID Application: Aleksandr  Kolesov (3WKK5AGY66)}"
notary_profile="${SYNTHETIC_OBSIDIAN_NOTARY_PROFILE:-SMC-notarize}"

legacy_root="${HOME}/.synthetic_obsidian"
soulx_source="${legacy_root}/runtime/soulx-singer/source"
soulx_svc_model="${legacy_root}/models/SoulX-Singer/model-svc.pt"
backing_models="${legacy_root}/models/backing_vocals"
boundary_model="${project_root}/data/user_boundary_alignment/sasha_manual_boundary_tcn.pt"
non_vocal_model="${project_root}/data/gtsinger_alignment/multilingual_mps_full_strict_pause_silence_head_thr090_tcn.pt"
wav2vec_model="${HOME}/.cache/torch/hub/checkpoints/wav2vec2_fairseq_base_ls960_asr_ls960.pth"

require_file() {
    if [[ ! -f "$1" ]]; then
        echo "Missing required file: $1" >&2
        exit 1
    fi
}

require_dir() {
    if [[ ! -d "$1" ]]; then
        echo "Missing required directory: $1" >&2
        exit 1
    fi
}

require_command() {
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "Missing required command: $1" >&2
        exit 1
    fi
}

require_command cmake
require_command codesign
require_command hdiutil
require_command npm

require_dir "${runtime_build}/core"
require_dir "${runtime_build}/backing-notes"
require_dir "${runtime_build}/voice-engine"
require_dir "${soulx_source}"
require_dir "${backing_models}/backing_transformer_v1"
require_file "${soulx_svc_model}"
require_file "${boundary_model}"
require_file "${non_vocal_model}"
require_file "${wav2vec_model}"
require_file "${script_dir}/THIRD_PARTY_NOTICES.md"
require_file "${soulx_source}/LICENSE"

if ! security find-identity -v -p codesigning | grep -F "${identity}" >/dev/null; then
    echo "Developer ID signing identity is not available: ${identity}" >&2
    exit 1
fi

echo "Building the embedded interface"
(
    cd "${project_root}/frontend"
    npm run build
)

echo "Building the Apple Silicon application"
cmake -S "${project_root}" -B "${release_build}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 \
    -DSYNTHETIC_OBSIDIAN_BUILD_TOOLS=ON
cmake --build "${release_build}" --config Release --target VocalAnnotationTool --parallel

built_app="${release_build}/vocal_annotation_tool_build/VocalAnnotationTool_artefacts/Release/Synthetic Obsidian.app"
require_dir "${built_app}"

echo "Assembling the self-contained application"
rm -rf "${stage_root}" "${dmg_stage}"
mkdir -p "${stage_root}" "${dmg_stage}" "${output_dir}"
/usr/bin/ditto "${built_app}" "${app_stage}"

resources="${app_stage}/Contents/Resources"
runtime_resources="${resources}/Runtime"
licenses="${resources}/Licenses"
info_plist="${app_stage}/Contents/Info.plist"
/usr/libexec/PlistBuddy -c "Delete :LSMinimumSystemVersion" "${info_plist}" \
    >/dev/null 2>&1 || true
/usr/libexec/PlistBuddy -c "Add :LSMinimumSystemVersion string 15.0" "${info_plist}"
mkdir -p "${runtime_resources}" "${licenses}"

/usr/bin/ditto "${runtime_build}/core" "${runtime_resources}/core"
/usr/bin/ditto "${runtime_build}/backing-notes" "${runtime_resources}/backing-notes"
/usr/bin/ditto "${runtime_build}/voice-engine" "${runtime_resources}/voice-engine"

mkdir -p \
    "${runtime_resources}/core/models/torch/hub/checkpoints" \
    "${runtime_resources}/backing-notes/models" \
    "${runtime_resources}/voice-engine/models"

/usr/bin/ditto "${boundary_model}" \
    "${runtime_resources}/core/models/$(basename "${boundary_model}")"
/usr/bin/ditto "${non_vocal_model}" \
    "${runtime_resources}/core/models/$(basename "${non_vocal_model}")"
/usr/bin/ditto "${wav2vec_model}" \
    "${runtime_resources}/core/models/torch/hub/checkpoints/$(basename "${wav2vec_model}")"
/usr/bin/ditto "${backing_models}/backing_transformer_v1" \
    "${runtime_resources}/backing-notes/models/backing_transformer_v1"

/usr/bin/ditto "${soulx_svc_model}" \
    "${runtime_resources}/voice-engine/models/model-svc.pt"
/usr/bin/ditto "${soulx_source}" \
    "${runtime_resources}/voice-engine/source"

/usr/bin/ditto "${script_dir}/THIRD_PARTY_NOTICES.md" \
    "${licenses}/THIRD_PARTY_NOTICES.md"
/usr/bin/ditto "${soulx_source}/LICENSE" \
    "${licenses}/SoulX-Singer-LICENSE.txt"

echo "Signing the application"
codesign --force \
    --options runtime \
    --timestamp \
    --sign "${identity}" \
    "${app_stage}"
codesign --verify --deep --strict --verbose=2 "${app_stage}"

echo "Creating the disk image"
/usr/bin/ditto "${app_stage}" "${dmg_stage}/Synthetic Obsidian.app"
ln -s /Applications "${dmg_stage}/Applications"
rm -f "${output_dmg}"
hdiutil create \
    -quiet \
    -format ULFO \
    -fs HFS+ \
    -volname "Synthetic Obsidian" \
    -srcfolder "${dmg_stage}" \
    "${output_dmg}"

codesign --force --timestamp --sign "${identity}" "${output_dmg}"
codesign --verify --verbose=2 "${output_dmg}"

if [[ "${SYNTHETIC_OBSIDIAN_SKIP_NOTARIZATION:-0}" != "1" ]]; then
    echo "Submitting the disk image for Apple notarization"
    xcrun notarytool history \
        --keychain-profile "${notary_profile}" \
        --output-format json >/dev/null
    xcrun notarytool submit "${output_dmg}" \
        --keychain-profile "${notary_profile}" \
        --wait
    xcrun stapler staple "${output_dmg}"
    xcrun stapler validate "${output_dmg}"
else
    echo "Notarization was explicitly skipped."
fi

echo
echo "DMG: ${output_dmg}"
du -h "${output_dmg}"
shasum -a 256 "${output_dmg}"
