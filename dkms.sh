#!/bin/bash

set -euo pipefail

module_name="macbook12-audio-driver"
module_version="0.1"
source_dir="/usr/src/${module_name}-${module_version}"
cache_dir="/var/cache/macbook12-audio-driver"
script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
action="${1:-install}"

if (( EUID != 0 )); then
  echo "Run this script as root." >&2
  exit 1
fi

remove_driver() {
  dkms remove -m "$module_name" -v "$module_version" --all 2>/dev/null || true
  rm -rf -- "$source_dir" "$cache_dir"
  depmod -a
}

install_driver() {
  if dkms status -m "$module_name" -v "$module_version" 2>/dev/null | grep -q .; then
    dkms remove -m "$module_name" -v "$module_version" --all
  fi
  # dkms may keep modules built for other kernels and reuse them on the next
  # install; they would be stale after a source update
  rm -rf -- "/var/lib/dkms/${module_name}"

  rm -rf -- "$source_dir"
  install -d -m 0755 "$source_dir"
  install -m 0644 \
    "$script_dir/dkms.conf" \
    "$script_dir/Makefile_cirrus" \
    "$script_dir/Makefile_cs420x" \
    "$source_dir"
  install -m 0755 "$script_dir/prepare.cirrus.driver.sh" "$source_dir"
  cp -a "$script_dir/patch_cirrus" "$source_dir"

  dkms install -m "$module_name" -v "$module_version" --force

  # the other installed kernels with headers: build for them too, so that
  # switching kernels does not leave the stock module in place
  local k
  for k in /lib/modules/*/; do
    k="${k%/}"; k="${k##*/}"
    [[ $k == "$(uname -r)" || ! -e /lib/modules/$k/build/Makefile ]] && continue
    dkms install -m "$module_name" -v "$module_version" -k "$k" --force || echo "Build for $k failed, see /var/lib/dkms/$module_name/$module_version/build/make.log" >&2
  done
}

case "$action" in
  install|-i|--install)
    install_driver
    ;;
  remove|-r|-u|--remove|--uninstall)
    remove_driver
    ;;
  *)
    echo "Usage: $0 [install|remove]" >&2
    exit 2
    ;;
esac
