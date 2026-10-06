#!/bin/sh
# Proves the harness itself, including the shapes it must REFUSE. A harness
# that silently passes an example asserting nothing is the failure this
# organization keeps recording, so every refusal here is a case of its own.
set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
harness="$here/../run-examples.sh"
work="$here/.work"

passed=0
failed=0

expect() {
  name=$1
  verdict=$2
  root="$work/$name"
  rm -rf "$root"
  mkdir -p "$root/examples"
  cp -R "$here/fixtures/$name" "$root/examples/$name"

  if sh "$harness" "$root" >"$root/out.txt" 2>&1; then
    actual=pass
  else
    actual=fail
  fi

  if [ "$actual" != "$verdict" ]; then
    echo "FAIL $name: expected the harness to $verdict, it did $actual" >&2
    sed -n '1,20p' "$root/out.txt" >&2
    failed=$((failed + 1))
    return
  fi
  passed=$((passed + 1))
}

rm -rf "$work"
expect one-command pass
expect crlf-block pass
expect no-block fail

echo "$passed passed, $failed failed"
[ "$failed" -eq 0 ]
