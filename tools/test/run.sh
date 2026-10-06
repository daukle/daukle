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
  # git cannot track an empty directory, so the empty-expected fixture has to
  # carry a .keep that makes its expected/ tree non-empty on disk, which is the
  # very shape it exists to refuse. Dropping it here is what makes the fixture
  # the case it claims to be rather than a file-differs case wearing its name.
  find "$root/examples/$name" -name .keep -exec rm -f {} +

  if sh "$harness" "$root" >"$root/out.txt" 2>&1; then
    actual=pass
  else
    actual=fail
  fi

  if [ "$verdict" = skip ]; then
    if ! grep -q "^skip $name" "$root/out.txt"; then
      echo "FAIL $name: expected a printed skip, got none" >&2
      failed=$((failed + 1))
      return
    fi
    passed=$((passed + 1))
    return
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
expect empty-expected fail
DAUKLE_EXAMPLE_E2E= expect gated skip

echo "$passed passed, $failed failed"
[ "$failed" -eq 0 ]
