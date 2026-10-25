#!/bin/sh
# Benchmark the bundled synthetic workload using the shared Python entry point.
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
exec python3 "$here/perf_benchmark_all.py" "$@"
