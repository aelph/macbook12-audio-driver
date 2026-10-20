#!/bin/bash
# Microphone boost for the CS4208 MacBooks (MacBook9,1 / MacBook10,1).
#
# The codec has two boost amplifiers: "Internal Mic Boost" (pin 0x19) and
# "Mic Boost" (headset microphone, pin 0x18).  Both default to 0, which is
# -20 dB on the headset microphone, and both get reset to 0 whenever
# WirePlumber applies the ALSA card profile (ACP) mixer paths: on every input
# port change and at every start of WirePlumber, including the instance of the
# display manager's session that runs before the user logs in.
# analog-input-internal-mic.conf switches "Mic Boost" off and
# analog-input-mic.conf switches "Internal Mic Boost" off, and with
# api.alsa.soft-mixer = true (required for the speakers, see the README)
# nothing ever turns them back on.
#
# The codec's "Capture" control is not touched by WirePlumber either (the
# software mixer leaves the hardware mixer alone), so a muted or zeroed
# "Capture" — the state some machines boot with — makes every microphone
# silent, and "alsactl store" then preserves it.
#
# This script, run as root:
#   * installs a system-wide copy of the ACP mixer paths in which those two
#     blocks are left alone ("ignore") and points every WirePlumber instance
#     at it through ACP_PATHS_DIR in a systemd user-unit drop-in;
#   * restarts the calling user's WirePlumber, opens "Capture" (100%,
#     unmuted), sets both boosts to the given level and stores the ALSA
#     mixer state.
#
# Run it again after a PipeWire update to refresh the copied paths.
#
# Usage: sudo contrib/mic-boost-setup.sh [install|remove] [--boost N] [--card N]
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
    -h|--help) sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "Unknown option: $1" >&2; exit 2 ;;
  esac
done

if (( EUID != 0 )); then
  echo "Run this script with sudo: it installs system-wide files and stores the ALSA state." >&2
  exit 1
fi

src_dir="/usr/share/alsa-card-profile/mixer/paths"
dst_dir="/etc/alsa-card-profile/mixer/paths"
dropin_dir="/etc/systemd/user/wireplumber.service.d"
dropin="$dropin_dir/acp-paths.conf"
user="${SUDO_USER:-}"

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

# the calling user's WirePlumber picks the drop-in up on restart; the display
# manager's instance does so at the next boot
restart_user_wireplumber() {
  [[ -n $user && $user != root ]] || return 0
  local uid
  uid="$(id -u "$user")"
  if sudo -u "$user" XDG_RUNTIME_DIR="/run/user/$uid" systemctl --user daemon-reload 2>/dev/null; then
    if sudo -u "$user" XDG_RUNTIME_DIR="/run/user/$uid" systemctl --user is-active --quiet wireplumber; then
      sudo -u "$user" XDG_RUNTIME_DIR="/run/user/$uid" systemctl --user restart wireplumber
      sleep 2
      echo "WirePlumber of $user restarted"
    fi
  else
    echo "Could not reach the user manager of $user: run 'systemctl --user daemon-reload && systemctl --user restart wireplumber' yourself" >&2
  fi
}

# an earlier version installed a per-user override; it is superseded by the system-wide one
remove_user_override() {
  [[ -n $user && $user != root ]] || return 0
  local home
  home="$(getent passwd "$user" | cut -d: -f6)"
  rm -f -- "$home/.config/systemd/user/wireplumber.service.d/acp-paths.conf"
  rmdir -- "$home/.config/systemd/user/wireplumber.service.d" 2>/dev/null || true
  rm -rf -- "$home/.config/alsa-card-profile/mixer/paths"
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
    remove_user_override
    restart_user_wireplumber
    echo "WirePlumber instances now use the mixer paths in $dst_dir"

    [[ -n $card ]] || card="$(find_card)"
    if amixer -c "$card" sset Capture 100% cap > /dev/null 2>&1; then
      echo "Capture set to 100%, unmuted, on card $card"
    else
      echo "Control 'Capture' not found on card $card, skipped" >&2
    fi
    for ctl in "Mic Boost" "Internal Mic Boost"; do
      if amixer -c "$card" sset "$ctl" "$boost" > /dev/null 2>&1; then
        echo "$ctl set to $boost on card $card"
      else
        echo "Control '$ctl' not found on card $card, skipped" >&2
      fi
    done
    alsactl store && echo "ALSA mixer state stored"
    ;;
  remove)
    rm -f -- "$dropin"
    rmdir -- "$dropin_dir" 2>/dev/null || true
    rm -rf -- "$dst_dir"
    remove_user_override
    restart_user_wireplumber
    echo "Override removed; WirePlumber uses the system mixer paths again"
    ;;
esac
