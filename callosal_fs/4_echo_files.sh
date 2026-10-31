#!/usr/bin/env bash
  set -euo pipefail

  while IFS= read -r f; do
    [[ -z "$f" ]] && continue
    echo hi > "$f"
  done < ../4_node_files
