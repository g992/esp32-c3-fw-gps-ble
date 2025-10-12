#!/usr/bin/env bash

set -euo pipefail

# Merge PlatformIO build artifacts for ESP32-C3 into a single flashable image.
# Usage: ./merge_firmware.sh [environment] [output_path]

env_name=${1:-dfrobot_beetle_esp32c3}
output_arg=${2:-}
build_root=".pio/build/${env_name}"
packages_dir="${PLATFORMIO_PACKAGES_DIR:-${HOME}/.platformio/packages}"

if [[ -n "${output_arg}" ]]; then
  output_path="${output_arg}"
  output_dir=$(dirname "${output_path}")
else
  output_dir="builds"
  output_path="${output_dir}/${env_name}_merged.bin"
fi

error() {
  echo "Error: $*" >&2
  exit 1
}

require_file() {
  local path="$1"
  local label="$2"
  [[ -f "$path" ]] || error "Missing ${label}: ${path}"
}

declare -a esptool_cmd
if command -v esptool.py >/dev/null 2>&1; then
  esptool_cmd=(esptool.py)
elif [[ -d "${packages_dir}" ]]; then
  esptool_path=$(find "${packages_dir}" -path '*tool-esptoolpy*' -name 'esptool.py' -print -quit || true)
  if [[ -n "${esptool_path}" ]]; then
    if command -v python3 >/dev/null 2>&1; then
      esptool_cmd=(python3 "${esptool_path}")
    elif command -v python >/dev/null 2>&1; then
      esptool_cmd=(python "${esptool_path}")
    fi
  fi
fi

[[ ${#esptool_cmd[@]} -gt 0 ]] || error "esptool.py not found. Install it or ensure PlatformIO packages are available."

require_file "${build_root}/bootloader.bin" "bootloader"
require_file "${build_root}/partitions.bin" "partition table"
require_file "${build_root}/firmware.bin" "application image"

boot_app0_path=""
if [[ -f "${build_root}/boot_app0.bin" ]]; then
  boot_app0_path="${build_root}/boot_app0.bin"
else
  if [[ -d "${packages_dir}" ]]; then
    boot_app0_path=$(find "${packages_dir}" -path '*boot_app0.bin' -print -quit)
  fi
fi

[[ -n "${boot_app0_path}" && -f "${boot_app0_path}" ]] || error "Unable to locate boot_app0.bin. Run a build first or set PLATFORMIO_PACKAGES_DIR."

mkdir -p "${output_dir}"

"${esptool_cmd[@]}" --chip esp32c3 merge_bin \
  -o "${output_path}" \
  --flash_mode dio --flash_freq 80m --flash_size 4MB \
  0x0000 "${build_root}/bootloader.bin" \
  0x8000 "${build_root}/partitions.bin" \
  0xe000 "${boot_app0_path}" \
  0x10000 "${build_root}/firmware.bin"

echo "Merged binary created at ${output_path}"
