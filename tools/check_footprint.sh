#!/bin/sh
# Fails if edgetop links anything beyond libc or exceeds its memory/CPU budget.
set -eu
bin=${1:-./edgetop}
fail=0

extra=$(ldd "$bin" | grep -Ev 'linux-vdso|libc\.so|libdl\.so|ld-linux' || true)
if [ -n "$extra" ]; then
	echo "FAIL unexpected shared libraries:"; echo "$extra"; fail=1
else
	echo "ok   shared libraries: libc only"
fi

check() { # label json max_rss_kib
	rss=$(printf '%s' "$2" | sed -n 's/.*"rss_kib":\([0-9]*\).*/\1/p')
	if [ "$rss" -le "$3" ]; then echo "ok   $1 rss ${rss} KiB (budget $3)"; else echo "FAIL $1 rss ${rss} KiB > $3"; fail=1; fi
}
check "--no-gpu" "$("$bin" --json --no-gpu -d 0.25)" 2048
check "with gpu" "$("$bin" --json -d 0.25)" 21504

"$bin" --bench 2000 --no-gpu
"$bin" --bench 2000
exit $fail
