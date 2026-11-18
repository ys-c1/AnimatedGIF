#!/bin/sh
# One-shot acceptance run: build, generate samples, run all tests.
set -e
cd "$(dirname "$0")"
make
mkdir -p out
./gifgen out
./test_resample out | tee out/report.txt
