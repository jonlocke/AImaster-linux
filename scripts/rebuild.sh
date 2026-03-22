#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

SERVICE_NAME="aimaster.service"
SERVICE_CONFIG="/var/lib/aimaster/config.txt"
TMP_CONFIG_BACKUP=""

cleanup() {
  if [[ -n "$TMP_CONFIG_BACKUP" && -f "$TMP_CONFIG_BACKUP" ]]; then
    rm -f "$TMP_CONFIG_BACKUP"
  fi
}
trap cleanup EXIT

require_cmd() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "Required command not found: $1" >&2
    exit 1
  fi
}

for cmd in git make dpkg dpkg-deb sudo systemctl; do
  require_cmd "$cmd"
done

current_branch="$(git rev-parse --abbrev-ref HEAD)"
echo "Current branch: $current_branch"

echo "Fetching latest refs..."
git fetch --all --prune

echo "Pulling latest changes for $current_branch..."
git pull --ff-only

newest_remote_ref="$(
  git for-each-ref --sort=-committerdate --format='%(refname:short)' refs/remotes \
    | grep -v '^origin/HEAD$' \
    | head -n1 \
    | sed 's#^origin/##'
)"
if [[ -n "$newest_remote_ref" ]]; then
  echo "Newest fetched branch: $newest_remote_ref"
fi

mapfile -t branch_lines < <(
  {
    git for-each-ref --sort=-committerdate --format='%(committerdate:iso8601)|local|%(refname:short)|%(upstream:short)' refs/heads
    git for-each-ref --sort=-committerdate --format='%(committerdate:iso8601)|remote|%(refname:short)|' refs/remotes \
      | grep -vE '\|remote\|origin/HEAD\|'
  } | awk -F'|' '
      {
        key=$3
        sub(/^origin\//, "", key)
      }
      !seen[key]++ {
        print $0
      }
    '
)

if [[ ${#branch_lines[@]} -eq 0 ]]; then
  echo "No branches available to select." >&2
  exit 1
fi

echo
echo "Available branches:"
selected_default=1
for i in "${!branch_lines[@]}"; do
  IFS='|' read -r branch_date branch_kind branch_name branch_upstream <<<"${branch_lines[$i]}"
  display_name="$branch_name"
  note=""

  if [[ "$branch_kind" == "remote" ]]; then
    display_name="${branch_name#origin/}"
    note="remote"
  else
    note="local"
  fi

  if [[ "$display_name" == "$current_branch" ]]; then
    note="$note,current"
    selected_default=$((i + 1))
  fi
  if [[ -n "$newest_remote_ref" && "$display_name" == "$newest_remote_ref" ]]; then
    note="$note,newest"
  fi
  if [[ -n "$branch_upstream" ]]; then
    note="$note,tracks ${branch_upstream#origin/}"
  fi

  printf ' [%d] %s (%s; %s)\n' "$((i + 1))" "$display_name" "$branch_date" "$note"
done

echo
read -r -p "Select branch [${selected_default}]: " selection
selection="${selection:-$selected_default}"
if ! [[ "$selection" =~ ^[0-9]+$ ]] || (( selection < 1 || selection > ${#branch_lines[@]} )); then
  echo "Invalid selection: $selection" >&2
  exit 1
fi

IFS='|' read -r _selected_date selected_kind selected_name _selected_upstream <<<"${branch_lines[$((selection - 1))]}"
checkout_branch="$selected_name"
if [[ "$selected_kind" == "remote" ]]; then
  local_name="${selected_name#origin/}"
  if git show-ref --verify --quiet "refs/heads/$local_name"; then
    git checkout "$local_name"
    git pull --ff-only origin "$local_name"
  else
    git checkout -b "$local_name" --track "$selected_name"
  fi
  checkout_branch="$local_name"
else
  git checkout "$selected_name"
  git pull --ff-only
fi

echo "Building Debian package for branch: $checkout_branch"
./scripts/build_deb.sh

package_path="$(find "$ROOT_DIR/dist/deb" -maxdepth 1 -type f -name '*.deb' -printf '%T@ %p\n' | sort -nr | head -n1 | cut -d' ' -f2-)"
if [[ -z "$package_path" ]]; then
  echo "No Debian package found in dist/deb." >&2
  exit 1
fi

echo "Using package: $package_path"

if sudo test -f "$SERVICE_CONFIG"; then
  TMP_CONFIG_BACKUP="$(mktemp)"
  sudo cp "$SERVICE_CONFIG" "$TMP_CONFIG_BACKUP"
  echo "Backed up existing service config from $SERVICE_CONFIG"
fi

sudo dpkg -i "$package_path"

if [[ -n "$TMP_CONFIG_BACKUP" && -f "$TMP_CONFIG_BACKUP" ]]; then
  sudo install -o aimaster -g aimaster -m 0640 "$TMP_CONFIG_BACKUP" "$SERVICE_CONFIG"
  echo "Restored existing service config to $SERVICE_CONFIG"
fi

sudo systemctl restart "$SERVICE_NAME"
sudo systemctl --no-pager --full status "$SERVICE_NAME" || true

echo "Rebuild, package install, and service restart complete."
