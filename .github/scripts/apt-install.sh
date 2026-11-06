#!/usr/bin/env bash
# Usage: apt-install.sh [apt-get install options] package...
# Hardened apt-get update + install for GitHub-hosted Ubuntu runners.
set -euo pipefail

# x64 runners default to azure.archive.ubuntu.com, which stalls often.
if sudo grep -rqs azure.archive.ubuntu.com /etc/apt; then
  sudo grep -rlsZ azure.archive.ubuntu.com /etc/apt \
    | sudo xargs -0 sed -i 's#azure\.archive\.ubuntu\.com#archive.ubuntu.com#g'
  if sudo grep -rn azure.archive.ubuntu.com /etc/apt; then
    echo "azure.archive.ubuntu.com is still configured" >&2
    exit 1
  fi
fi

# The man-db trigger is slow and the runners do not need man pages.
echo 'set man-db/auto-update false' | sudo debconf-communicate
sudo dpkg-reconfigure man-db

retry() {
  local limit=$1
  shift
  for attempt in 1 2 3; do
    if sudo timeout "$limit" "$@"; then
      return 0
    fi
    echo "attempt $attempt failed: $*" >&2
    sleep $((attempt * 5))
  done
  return 1
}

export DEBIAN_FRONTEND=noninteractive
retry 300 apt-get -o Acquire::Retries=3 update
retry 900 apt-get -o Acquire::Retries=3 install -y "$@"
