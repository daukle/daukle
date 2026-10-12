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
  # A positional rather than an environment prefix: whether `VAR=x func` is
  # restored after the call differs between bash and dash, and this suite runs
  # under whichever `sh` the runner has.
  hosts=${3:-}
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

  # Which hosts are the WRONG ones depends on the runner, so the cases that
  # prove the host gate write the marker here rather than committing a fixture
  # that is only meaningful on one platform.
  if [ -n "$hosts" ]; then
    printf '%s\n' $hosts >"$root/examples/$name/needs-tools"
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

  # Both gates apply here, so a bare skip assertion would pass whichever one
  # spoke. The verdict is the skip naming the HOST, which is the only thing
  # that pins the order the two are read in.
  if [ "$verdict" = skip-names-host ]; then
    if ! grep -q "^skip $name.*this host is" "$root/out.txt"; then
      echo "FAIL $name: expected the skip to name the host, got:" >&2
      grep "^skip $name" "$root/out.txt" >&2 || echo "  (no skip at all)" >&2
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

  # A SKIPPED example also exits 0, so "the harness exited 0" is not the same
  # claim as "the example ran". Asserting the totals is what tells the two
  # apart: without it, a gate that wrongly skips everything passes every pass
  # case here. Found by mutating the empty-needs-tools guard and watching
  # nothing go red.
  if [ "$verdict" = pass ] && ! grep -q "^1 passed, 0 failed, 0 skipped" "$root/out.txt"; then
    echo "FAIL $name: the harness exited 0 without running the example" >&2
    grep "passed," "$root/out.txt" >&2
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

case $(uname -s) in
  Linux*) this_host=linux ;;
  Darwin*) this_host=macos ;;
  MINGW*|MSYS*|CYGWIN*|Windows*) this_host=windows ;;
  *) this_host=unknown ;;
esac
# Every host except this one, so the refusal case is wrong on every runner
# rather than on the one it was written on.
other_hosts=
for h in linux macos windows; do
  [ "$h" = "$this_host" ] || other_hosts="$other_hosts $h"
done

rm -rf "$work"
expect one-command pass
expect crlf-block pass
expect no-block fail
expect empty-expected fail
expect staged fail
expect secondpass working-tree-fail
DAUKLE_EXAMPLE_E2E= expect gated skip

# An EMPTY needs-tools must still mean every host: that is what all 18 markers
# in the org held when the gate was added, so this is the compatibility case.
DAUKLE_EXAMPLE_E2E=1 expect host-gate pass
DAUKLE_EXAMPLE_E2E=1 expect host-gate pass "linux macos windows"
DAUKLE_EXAMPLE_E2E=1 expect host-gate skip "$other_hosts"
# A typo'd host must FAIL. Skipping instead would hide the example on every
# runner at once and look exactly like a green organization.
DAUKLE_EXAMPLE_E2E=1 expect host-gate fail "plan9"
# The host gate is read FIRST, so an unsupported host is named even when the
# E2E flag is also missing. Asserting the message is what pins the order.
DAUKLE_EXAMPLE_E2E= expect host-gate skip-names-host "$other_hosts"

echo "$passed passed, $failed failed"
[ "$failed" -eq 0 ]
