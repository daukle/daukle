#!/bin/sh
# The only example harness in the organization. Every repository that runs
# examples already clones core into .daukle to build the daukle it tests
# against, so this script arrives with that binary and matches its revision.
#
# An example asserts through its own ABOUT.md: a fenced console block whose
# "$ " lines are commands and whose remaining lines must appear in the output.
# That is the text a reader of the example already sees, which is why there is
# no expect-output.txt beside it: two copies of one claim drift.
set -eu

root=${1:-.}
root=$(CDPATH= cd -- "$root" && pwd)
work="$root/test/.work-examples"

daukle=${DAUKLE:-}
if [ -z "$daukle" ]; then
  for candidate in \
    "$root/.daukle/build/daukle" \
    "$root/.daukle/build/daukle.exe" \
    "$root/.daukle/build/Release/daukle.exe" \
    "$root/.daukle/build/Debug/daukle.exe" \
    "$root/build/daukle" \
    "$root/build/daukle.exe" \
    "$root/build/Release/daukle.exe" \
    "$root/build/Debug/daukle.exe"
  do
    [ -x "$candidate" ] && daukle=$candidate && break
  done
fi

passed=0
failed=0
skipped=0

fail() {
  echo "FAIL $1: $2" >&2
  failed=$((failed + 1))
}

# The console block, with its fences removed. Empty when there is none.
#
# @implNote the carriage return is stripped before anything is matched. Core
# does not pin `* -text` the way the plugin repositories do, so its own
# ABOUT.md files are CRLF on Windows, and an unstripped `\r` defeats the `$`
# anchor: the block reads as absent and the example is refused for asserting
# nothing, on one runner only.
console_block() {
  [ -f "$1/ABOUT.md" ] || return 0
  awk '
    { sub(/\r$/, "") }
    /^```console$/ { inside = 1; next }
    inside && /^```$/ { exit }
    inside { print }
  ' "$1/ABOUT.md"
}

run_example() {
  example=$1
  name=$(basename "$example")

  if [ -f "$example/needs-tools" ] && [ "${DAUKLE_EXAMPLE_E2E:-}" != "1" ]; then
    echo "skip $name: set DAUKLE_EXAMPLE_E2E=1 to provision real tools here" >&2
    skipped=$((skipped + 1))
    return
  fi

  block=$(console_block "$example")
  if [ -z "$block" ] && [ ! -d "$example/expected" ]; then
    fail "$name" "no console block in ABOUT.md and no expected/ tree, so it asserts nothing"
    return
  fi

  if [ -z "$daukle" ] || [ ! -x "$daukle" ]; then
    fail "$name" "no daukle binary: set DAUKLE, or check daukle/daukle out into .daukle and build it"
    return
  fi

  sandbox="$work/$name"
  rm -rf "$sandbox"
  mkdir -p "$(dirname "$sandbox")"
  cp -R "$example" "$sandbox"

  if ! run_block "$sandbox" "$name" "$block"; then
    return
  fi

  if [ -d "$example/expected" ]; then
    rm -rf "$sandbox/expected"
    if ! (cd "$sandbox" && "$daukle" sync >/dev/null 2>&1); then
      fail "$name" "sync failed"
      return
    fi
    # `|| return` here would return compare_expected's own non-zero status, and
    # under `set -e` that ends the whole run: no totals, and every example after
    # this one silently never runs while the exit code still says 1.
    if ! compare_expected "$example" "$sandbox" "$name"; then
      return
    fi
    if ! (cd "$sandbox" && "$daukle" sync >/dev/null 2>&1); then
      fail "$name" "second sync failed"
      return
    fi
    if ! compare_expected "$example" "$sandbox" "$name"; then
      return
    fi
  fi

  if [ -f "$root/plugin.lua" ]; then
    staged="$work/$name-staged"
    rm -rf "$staged"
    cp -R "$example" "$staged"
    stage_working_tree "$staged"
    if ! run_block "$staged" "$name (working tree)" "$block"; then
      return
    fi
  fi

  passed=$((passed + 1))
}

# One command of a console block, and the lines that must appear in its output.
run_one() {
  sandbox=$1
  name=$2
  command=$3
  expected=$4
  [ -n "$command" ] || return 0

  if ! (cd "$sandbox" && sh -c "$command" >stdout.txt 2>stderr.txt); then
    fail "$name" "\"$command\" failed"
    tail -20 "$sandbox/stderr.txt" >&2
    return 1
  fi

  printf '%s\n' "$expected" > "$work/.want"
  while IFS= read -r want; do
    [ -n "$want" ] || continue
    if ! grep -qF "$want" "$sandbox/stdout.txt" "$sandbox/stderr.txt"; then
      fail "$name" "\"$command\" printed no \"$want\""
      tail -20 "$sandbox/stdout.txt" >&2
      return 1
    fi
  done < "$work/.want"
  return 0
}

# The plugin the OWNING repository is developing, staged over a copy of the
# example so that pass two exercises the working tree where pass one exercised
# the published release. A repository with no plugin.lua at its root owns no
# plugin, so there is nothing to stage and the example runs once.
#
# @implNote the variables here are deliberately not named `name` or `sandbox`.
# A shell function has no locals, so reusing those would overwrite the caller's
# and every staged failure would report the repository's name in place of the
# example's, making one failing example indistinguishable from another.
stage_working_tree() {
  staged_root=$1
  [ -f "$root/plugin.lua" ] || return 1
  mkdir -p "$staged_root/plugins"
  cp "$root/plugin.lua" "$staged_root/plugins/plugin.lua"
  [ -d "$root/lib" ] && cp -R "$root/lib" "$staged_root/plugins/lib"
  plugin_name=$(basename "$root")
  awk -v plugin="$plugin_name" '
    $1 == plugin && $2 == "=" { print plugin " = \"./plugins\""; next }
    { print }
  ' "$staged_root/daukle.toml" > "$staged_root/daukle.toml.staged"
  mv "$staged_root/daukle.toml.staged" "$staged_root/daukle.toml"
  return 0
}

# An empty expected/ compares nothing and would pass, which is the one way an
# example can look green while asserting nothing at all.
compare_expected() {
  example=$1
  sandbox=$2
  name=$3
  if [ -z "$(cd "$example/expected" && find . -type f)" ]; then
    fail "$name" "expected/ holds no files, so this example asserts nothing"
    return 1
  fi
  ok=0
  for relative in $(cd "$example/expected" && find . -type f); do
    if ! cmp -s "$example/expected/$relative" "$sandbox/$relative"; then
      fail "$name" "$relative differs"
      diff -u "$example/expected/$relative" "$sandbox/$relative" >&2 || true
      ok=1
    fi
  done
  return $ok
}

# Runs each "$ " line of the console block in order.
#
# @implNote the loop reads from a REDIRECT, never from a pipe. A pipe puts the
# body in a subshell, so fail()'s increment of "failed" is lost there and a
# failing example reports neither a pass nor a fail: the totals stay green with
# one example silently missing from them.
run_block() {
  sandbox=$1
  name=$2
  block=$3
  [ -n "$block" ] || return 0

  printf '%s\n' "$block" > "$work/.console"
  command=
  expected=
  outcome=0

  while IFS= read -r line; do
    case "$line" in
      '$ '*)
        if ! run_one "$sandbox" "$name" "$command" "$expected"; then
          outcome=1
          break
        fi
        command=${line#\$ }
        expected=
        ;;
      *)
        expected="$expected$line
"
        ;;
    esac
  done < "$work/.console"

  if [ "$outcome" -eq 0 ]; then
    run_one "$sandbox" "$name" "$command" "$expected" || outcome=1
  fi
  return $outcome
}

rm -rf "$work"
mkdir -p "$work/bin"

# A console block is what a reader would type, so it says `daukle`, not a path
# to the binary under test. A shim on PATH is what makes that true without
# rewriting the command: `./daukle` in the wrapper example must keep meaning
# the wrapper, which a textual substitution of the leading token would break.
if [ -n "$daukle" ] && [ -x "$daukle" ]; then
  printf '#!/bin/sh\nexec "%s" "$@"\n' "$daukle" > "$work/bin/daukle"
  chmod +x "$work/bin/daukle"
  PATH="$work/bin:$PATH"
  export PATH
fi

for example in "$root"/examples/*/; do
  [ -d "$example" ] || continue
  run_example "${example%/}"
done

echo "$passed passed, $failed failed, $skipped skipped"
[ "$failed" -eq 0 ]
