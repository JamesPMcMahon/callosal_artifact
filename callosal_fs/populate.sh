 #!/usr/bin/env bash
  set -euo pipefail

  src_file="$1"
  target_dir="$2"

  for f in "$target_dir"/*; do
    [ -f "$f" ] || continue
    cat "$src_file" > "$f"
  done

#  Usage:

#  bash populate.sh /path/to/source_file /path/to/target_dir

