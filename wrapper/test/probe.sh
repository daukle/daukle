#!/bin/sh
# Run with:
#   docker run --rm -v "//$PWD/wrapper:/w:ro" -v "//$PWD/wrapper/test:/s:ro" ubuntu:24.04 sh /s/probe.sh
#
# ubuntu:24.04 rather than a hosted runner on purpose: a minimal image has sha256sum and tar and
# NO downloader, which is the condition this wrapper exists to handle well and the one CI can
# never show. It reaches the real published release, so a transient GitHub 503 reddens it; that is
# the download being real rather than a defect.
# Adversarial checks for the POSIX wrapper, run inside a bare image.
#
# The two checks that REMOVE the downloader run last: apt-get will not restore a binary that was
# deleted rather than uninstalled, so an earlier removal silently starves every later check.
failures=0
check() {
    name=$1; shift
    if "$@"; then
        echo "PASS $name"
    else
        echo "FAIL $name"
        failures=$((failures + 1))
    fi
}

apt-get update -qq >/dev/null 2>&1
apt-get install -y -qq curl >/dev/null 2>&1

mkdir -p /proj && cp -r /w/. /proj/ && chmod +x /proj/daukle
cd /proj || exit 1

# Read from the pin rather than written here. Every expectation below used to carry 0.1.0 and the
# first bytes of a digest as literals, and when D-67 moved the pin to 0.2.0 eight of these fifteen
# checks failed against a wrapper that was working perfectly. A test with a hand-maintained copy of
# the thing it tests is the bug D-67 exists to delete, one layer out.
want_version=$(sed -n 's/^[[:space:]]*version[[:space:]]*=[[:space:]]*"\([^"]*\)".*/\1/p'     /w/.daukle/wrapper.toml | head -n 1)
want_digest=$(awk '/^\[assets\."linux\/x86_64"\]/ { inside = 1; next } /^\[/ { inside = 0 }
    inside' /w/.daukle/wrapper.toml |
    sed -n 's/^[[:space:]]*sha256[[:space:]]*=[[:space:]]*"\([^"]*\)".*/\1/p' | head -n 1)
want_asset=$(awk '/^\[assets\."linux\/x86_64"\]/ { inside = 1; next } /^\[/ { inside = 0 }
    inside' /w/.daukle/wrapper.toml |
    sed -n 's/^[[:space:]]*asset[[:space:]]*=[[:space:]]*"\([^"]*\)".*/\1/p' | head -n 1)
if [ -z "$want_version" ] || [ -z "$want_digest" ] || [ -z "$want_asset" ]; then
    echo "FAIL could not read version, digest and asset out of the pin" >&2
    exit 1
fi
echo "pin under test: $want_version $want_asset $want_digest"

echo "=== 1. cold run downloads and runs ==="
out=$(./daukle --version 2>/dev/null)
echo "$out"
check "cold run prints the pinned version" test "$out" = "daukle $want_version"

echo
echo "=== 2. a pin digest that does not match is refused, and nothing runs ==="
mkdir -p /bad && cp -r /w/. /bad/ && chmod +x /bad/daukle
sed -i "s/^sha256 = \"$want_digest\"/sha256 = \"DEADBEEF\"/" /bad/.daukle/wrapper.toml
out=$(cd /bad && DAUKLE_CACHE_DIR=/tmp/bad-cache ./daukle --version 2>&1 || true)
echo "$out"
check "a wrong digest is refused" sh -c 'echo "$1" | grep -q "and the pin says"' _ "$out"
check "nothing was run" sh -c 'echo "$1" | grep -q "Nothing was run"' _ "$out"
check "the rejected download is not left behind" sh -c '! find /tmp/bad-cache -type f 2>/dev/null | grep -q .'

echo
echo "=== 3. a corrupt CACHED file is replaced rather than being fatal ==="
corrupt=/tmp/corrupt-cache/wrapper/$want_version/linux/x86_64/$want_asset
mkdir -p "$(dirname "$corrupt")" && echo "not a binary" > "$corrupt" && chmod +x "$corrupt"
out=$(DAUKLE_CACHE_DIR=/tmp/corrupt-cache ./daukle --version 2>/dev/null || true)
echo "$out"
check "a corrupt cache entry is re-downloaded" test "$out" = "daukle $want_version"

echo
echo "=== 4. a host with no section is named, not 404'd ==="
mkdir -p /nohost && cp -r /w/. /nohost/ && chmod +x /nohost/daukle
awk '/^\[assets\."linux\/x86_64"\]/ { skip = 1; next } /^\[/ { skip = 0 } !skip' \
    /w/.daukle/wrapper.toml > /nohost/.daukle/wrapper.toml
out=$(cd /nohost && ./daukle --version 2>&1 || true)
echo "$out"
check "the uncovered host is named" sh -c 'echo "$1" | grep -q "no published daukle for linux/x86_64"' _ "$out"
check "the cells that DO exist are listed" sh -c 'echo "$1" | grep -q "macos/aarch64"' _ "$out"

echo
echo "=== 5. a missing pin file says where it should be ==="
mkdir -p /nopin && cp /w/daukle /nopin/ && chmod +x /nopin/daukle
out=$(cd /nopin && ./daukle --version 2>&1 || true)
echo "$out"
check "a missing pin is reported by path" sh -c 'echo "$1" | grep -q "no pin file at"' _ "$out"

echo
echo "=== 6. the wrapper runs from another directory ==="
out=$(cd / && /proj/daukle --version 2>/dev/null || true)
echo "$out"
check "resolves its pin relative to the script" test "$out" = "daukle $want_version"

echo
echo "=== 7. arguments and exit codes reach the binary ==="
out=$(./daukle --this-flag-does-not-exist 2>&1 || true)
code=$(./daukle --this-flag-does-not-exist >/dev/null 2>&1; echo $?)
echo "exit $code"
check "a bad flag is the binary's error, not the wrapper's" sh -c '! echo "$1" | grep -q "daukle wrapper:"' _ "$out"
check "a non-zero exit code is propagated" test "$code" != "0"

echo
echo "=== 8. a CRLF pin file still works ==="
# These three files are copied into someone else's repository and checked out under their git
# settings, so the pin can arrive CRLF whatever this repository does. A CR on the section header
# used to make the lookup miss and report the host it had just been given.
mkdir -p /crlf && cp -r /w/. /crlf/ && chmod +x /crlf/daukle
sed -i 's/$//' /crlf/.daukle/wrapper.toml
out=$(cd /crlf && DAUKLE_CACHE_DIR=/tmp/crlf-cache ./daukle --version 2>/dev/null || true)
echo "$out"
check "a CRLF pin is parsed" test "$out" = "daukle $want_version"

echo
echo "=== 9. wrapper update replaces all three files from a release ==="
# Against a DIFFERENT release from the one pinned, so "it updated" and "it did nothing" cannot
# look the same. 0.2.0 is the first release that carries the wrapper as assets at all.
mkdir -p /upd && cp -r /w/. /upd/ && chmod +x /upd/daukle
sed -i 's/^version = .*/version = "0.0.0-stale"/' /upd/.daukle/wrapper.toml
out=$(cd /upd && ./daukle wrapper update 2>&1 || true)
echo "$out"
check "it reports the version it moved to" sh -c 'echo "$1" | grep -q "updated to"' _ "$out"
check "the pin no longer names the stale version" sh -c '! grep -q "0.0.0-stale" /upd/.daukle/wrapper.toml'
check "the replaced pin names a version" sh -c 'grep -q "^version = \"[0-9]" /upd/.daukle/wrapper.toml'
check "the replaced daukle is still executable" test -x /upd/daukle
check "the powershell half came too" test -s /upd/daukle.ps1
# The script overwrote ITSELF while running, which is the hazard the staged rename exists for: a
# shell that resumed in the middle of new bytes would fail here rather than in the update.
out=$(cd /upd && ./daukle --version 2>&1 || true)
echo "$out"
check "the replaced wrapper still runs" sh -c 'echo "$1" | grep -q "^daukle [0-9]"' _ "$out"

echo
echo "=== 10. wrapper update names a version that has no wrapper assets ==="
mkdir -p /old && cp -r /w/. /old/ && chmod +x /old/daukle
out=$(cd /old && ./daukle wrapper update 0.1.0 2>&1 || true)
echo "$out"
check "a release without the assets is a download failure" sh -c 'echo "$1" | grep -q "could not download"' _ "$out"
check "the pin was left alone" sh -c 'cmp -s /old/.daukle/wrapper.toml /w/.daukle/wrapper.toml'
check "no staging file was left behind" sh -c '! ls /old/*.update.* >/dev/null 2>&1'

echo
echo "=== 11. DESTRUCTIVE from here: the downloader is removed ==="
rm -f /usr/bin/curl /usr/bin/wget
out=$(./daukle --version 2>/dev/null)
echo "$out"
check "a warm cache works with no downloader present" test "$out" = "daukle $want_version"

echo
echo "=== 12. no downloader and an empty cache names the packages ==="
out=$(DAUKLE_CACHE_DIR=/tmp/empty-cache ./daukle --version 2>&1 || true)
echo "$out"
check "message names curl and wget" sh -c 'echo "$1" | grep -q "neither curl nor wget"' _ "$out"
check "message names a package to install" sh -c 'echo "$1" | grep -q "apt install curl"' _ "$out"

echo
echo "failures: $failures"
exit $failures
