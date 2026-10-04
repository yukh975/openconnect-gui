#!/bin/sh
# Tests of the privileged helper that need no root: builds ocg-helper and
# ocg-tun-client with -DOCG_HELPER_TEST (fake utun echoing packets, test
# vpnc-script, socket in the work directory) and runs them end to end.
# Usage: src/macos/test/run.sh [work-dir]
set -e
SRC=$(cd "$(dirname "$0")/.." && pwd)
WORK=${1:-$(mktemp -d)}
mkdir -p "$WORK"
for f in ocg-helper ocg-tun-client; do
    cc -O1 -Wall -Wextra -Wno-unused-but-set-variable -DOCG_HELPER_TEST \
        -o "$WORK/$f" "$SRC/$f.c" -framework CoreFoundation -framework SystemConfiguration
done
cp "$SRC/test/helper_test.py" "$SRC/test/helper_bench.py" "$WORK/"
python3 "$WORK/helper_test.py"
python3 "$WORK/helper_bench.py"
python3 "$WORK/helper_bench.py" big
