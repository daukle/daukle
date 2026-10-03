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

echo "=== 1. cold run downloads and runs ==="
out=$(./daukle --version 2>/dev/null)
echo "$out"
check "cold run prints the pinned version" test "$out" = "daukle 0.1.0"

echo
echo "=== 2. a pin digest that does not match is refused, and nothing runs ==="
mkdir -p /bad && cp -r /w/. /bad/ && chmod +x /bad/daukle
sed -i 's/^sha256 = "02d96646/sha256 = "DEADBEEF/' /bad/.daukle/wrapper.toml
out=$(cd /bad && DAUKLE_CACHE_DIR=/tmp/bad-cache ./daukle --version 2>&1 || true)
echo "$out"
check "a wrong digest is refused" sh -c 'echo "$1" | grep -q "and the pin says"' _ "$out"
check "nothing was run" sh -c 'echo "$1" | grep -q "Nothing was run"' _ "$out"
check "the rejected download is not left behind" sh -c '! find /tmp/bad-cache -type f 2>/dev/null | grep -q .'

echo
echo "=== 3. a corrupt CACHED file is replaced rather than being fatal ==="
corrupt=/tmp/corrupt-cache/wrapper/0.1.0/linux/x86_64/daukle-linux-x86_64
mkdir -p "$(dirname "$corrupt")" && echo "not a binary" > "$corrupt" && chmod +x "$corrupt"
out=$(DAUKLE_CACHE_DIR=/tmp/corrupt-cache ./daukle --version 2>/dev/null || true)
echo "$out"
check "a corrupt cache entry is re-downloaded" test "$out" = "daukle 0.1.0"

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
check "resolves its pin relative to the script" test "$out" = "daukle 0.1.0"

echo
echo "=== 7. arguments and exit codes reach the binary ==="
out=$(./daukle --this-flag-does-not-exist 2>&1 || true)
code=$(./daukle --this-flag-does-not-exist >/dev/null 2>&1; echo $?)
echo "exit $code"
check "a bad flag is the binary's error, not the wrapper's" sh -c '! echo "$1" | grep -q "daukle wrapper:"' _ "$out"
check "a non-zero exit code is propagated" test "$code" != "0"

echo
echo "=== 8. DESTRUCTIVE from here: the downloader is removed ==="
rm -f /usr/bin/curl /usr/bin/wget
out=$(./daukle --version 2>/dev/null)
echo "$out"
check "a warm cache works with no downloader present" test "$out" = "daukle 0.1.0"

echo
echo "=== 9. no downloader and an empty cache names the packages ==="
out=$(DAUKLE_CACHE_DIR=/tmp/empty-cache ./daukle --version 2>&1 || true)
echo "$out"
check "message names curl and wget" sh -c 'echo "$1" | grep -q "neither curl nor wget"' _ "$out"
check "message names a package to install" sh -c 'echo "$1" | grep -q "apt install curl"' _ "$out"

echo
echo "failures: $failures"
exit $failures
