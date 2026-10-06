#!/bin/sh
# Proves the harness itself, including the shapes it must REFUSE. A harness
# that silently passes an example asserting nothing is the failure this
# organization keeps recording, so every refusal here is a case of its own.
set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
harness="$here/../run-examples.sh"
work="$here/.work"

# Each fixture is handed a throwaway root, so the harness's own discovery looks
# under that root and never finds the binary this repository just built. The
# search belongs here, where the repository root is known, which is what keeps
# CI's post_test to one platform-independent line.
if [ -z "${DAUKLE:-}" ]; then
  repository=$(CDPATH= cd -- "$here/../.." && pwd)
  for candidate in \
    "$repository/build/daukle" \
    "$repository/build/daukle.exe" \
    "$repository/build/Release/daukle.exe" \
    "$repository/build/Debug/daukle.exe"
  do
    [ -x "$candidate" ] && DAUKLE=$candidate && export DAUKLE && break
  done
fi

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
  # A plugin.lua at the harness root is what tells the harness this repository
  # owns a plugin, which is the only trigger for the second pass.
  if [ -f "$here/fixtures/$name.root-plugin.lua" ]; then
    cp "$here/fixtures/$name.root-plugin.lua" "$root/plugin.lua"
  fi

  if sh "$harness" "$root" >"$root/out.txt" 2>&1; then
    actual=pass
  else
    actual=fail
  fi

  # The second pass is invisible when it silently does nothing, so asserting a
  # plain failure would pass just as well with the staging removed. The verdict
  # is the failure NAMING the working tree.
  if [ "$verdict" = working-tree-fail ]; then
    if [ "$actual" = pass ]; then
      echo "FAIL $name: the staged working tree did not run" >&2
      failed=$((failed + 1))
      return
    fi
    if ! grep -q "(working tree)" "$root/out.txt"; then
      echo "FAIL $name: it failed, but not in the second pass" >&2
      sed -n '1,20p' "$root/out.txt" >&2
      failed=$((failed + 1))
      return
    fi
    passed=$((passed + 1))
    return
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

  # A refused example must still leave the harness running. Under `set -e` a
  # run_example that returns non-zero kills the whole script, so the totals
  # never print and every example after the failing one is silently skipped,
  # while the exit code still says 1 and CI still looks correctly red.
  if ! grep -q "passed, .* failed" "$root/out.txt"; then
    echo "FAIL $name: the harness stopped before printing its totals" >&2
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
expect staged fail
expect secondpass working-tree-fail
DAUKLE_EXAMPLE_E2E= expect gated skip

echo "$passed passed, $failed failed"
[ "$failed" -eq 0 ]
