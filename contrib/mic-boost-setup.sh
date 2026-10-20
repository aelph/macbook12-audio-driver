#!/bin/bash
# Microphone boost for the CS4208 MacBooks (MacBook9,1 / MacBook10,1).
#
# The codec has two boost amplifiers: "Internal Mic Boost" (pin 0x19) and
# "Mic Boost" (headset microphone, pin 0x18).  Both default to 0, which is
# -20 dB on the headset microphone, and both get reset to 0 on every input
# port change: WirePlumber applies the ALSA card profile (ACP) mixer paths,
# where analog-input-internal-mic.conf switches "Mic Boost" off and
# analog-input-mic.conf switches "Internal Mic Boost" off, and with
# api.alsa.soft-mixer = true (required for the speakers, see the README)
# nothing ever turns them back on.
#
# This script:
#   * installs a per-user copy of the ACP mixer paths in which those two
#     blocks are left alone ("ignore"), and points WirePlumber at it through
#     ACP_PATHS_DIR in a systemd drop-in;
#   * sets both boosts to the given level and stores the ALSA mixer state.
#
# Run it again after a PipeWire update to refresh the copied paths.
#
# Usage: contrib/mic-boost-setup.sh [install|remove] [--boost N] [--card N]
#   install   (default) install the override, set the boosts, store the state
#   remove    remove the override and restart WirePlumber
#   --boost N boost level 0-3 for both amplifiers (default 2 = 0 dB for the headset mic)
#   --card N  ALSA card index (default: the card with the CS4208 codec)

set -euo pipefail

action="install"
boost=2
card=""

while (($#)); do
  case "$1" in
    install|remove) action="$1"; shift ;;
    --boost) boost="${2:?--boost needs a value}"; shift 2 ;;
    --card) card="${2:?--card needs a value}"; shift 2 ;;
    -h|--help) sed -n '2,25p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "Unknown option: $1" >&2; exit 2 ;;
  esac
done

if (( EUID == 0 )); then
  echo "Run this script as your desktop user, not as root (it configures the user's WirePlumber)." >&2
  exit 1
fi

src_dir="/usr/share/alsa-card-profile/mixer/paths"
dst_dir="${XDG_CONFIG_HOME:-$HOME/.config}/alsa-card-profile/mixer/paths"
dropin_dir="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user/wireplumber.service.d"
dropin="$dropin_dir/acp-paths.conf"

find_card() {
  local f
  for f in /proc/asound/card*/codec#*; do
    [[ -r $f ]] || continue
    if grep -q "CS4208" "$f"; then
      f="${f#/proc/asound/card}"
      echo "${f%%/*}"
      return
    fi
  done
  echo 0
}

# leave one [Element ...] block of a path file alone instead of switching it off
ignore_element() {
  local file="$1" element="$2"
  awk -v el="[Element $element]" '
    $0 == el { inblock = 1; print; next }
    inblock && /^\[/ { inblock = 0 }
    inblock && /^switch = off$/ { print "switch = ignore"; next }
    inblock && /^volume = off$/ { print "volume = ignore"; next }
    { print }
  ' "$file" > "$file.tmp" && mv "$file.tmp" "$file"
  grep -A3 -F "[Element $element]" "$file" | grep -q "^volume = ignore$" ||
    { echo "Could not patch $element in $file" >&2; exit 1; }
}

restart_wireplumber() {
  systemctl --user daemon-reload
  if systemctl --user is-active --quiet wireplumber; then
    systemctl --user restart wireplumber
    sleep 2
  fi
}

case "$action" in
  install)
    [[ -d $src_dir ]] || { echo "ACP mixer paths not found in $src_dir (is PipeWire installed?)" >&2; exit 1; }
    rm -rf -- "$dst_dir"
    mkdir -p -- "$dst_dir"
    cp -- "$src_dir"/* "$dst_dir"/
    ignore_element "$dst_dir/analog-input-internal-mic.conf" "Mic Boost"
    ignore_element "$dst_dir/analog-input-mic.conf" "Internal Mic Boost"
    mkdir -p -- "$dropin_dir"
    cat > "$dropin" <<EOF
# CS4208 MacBook: ACP mixer paths that leave the mic boost amplifiers alone.
# Installed by contrib/mic-boost-setup.sh of macbook12-audio-driver.
[Service]
Environment=ACP_PATHS_DIR=$dst_dir
EOF
    restart_wireplumber
    echo "WirePlumber now uses the mixer paths in $dst_dir"

    [[ -n $card ]] || card="$(find_card)"
    for ctl in "Mic Boost" "Internal Mic Boost"; do
      if amixer -c "$card" sset "$ctl" "$boost" > /dev/null 2>&1; then
        echo "$ctl set to $boost on card $card"
      else
        echo "Control '$ctl' not found on card $card, skipped" >&2
      fi
    done
    if command -v alsactl > /dev/null; then
      if sudo -n true 2>/dev/null || { [[ -t 0 ]] && sudo -v; }; then
        sudo alsactl store && echo "ALSA mixer state stored"
      else
        echo "Run 'sudo alsactl store' to keep the boosts across reboots" >&2
      fi
    fi
    ;;
  remove)
    rm -f -- "$dropin"
    rmdir -- "$dropin_dir" 2>/dev/null || true
    rm -rf -- "$dst_dir"
    restart_wireplumber
    echo "Override removed; WirePlumber uses the system mixer paths again"
    ;;
esac
