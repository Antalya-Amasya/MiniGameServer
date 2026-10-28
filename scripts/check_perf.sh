#!/bin/bash
set +e
perf --version
echo "PERF_VERSION_EXIT:$?"
perf stat -e cycles -- true
echo "PERF_STAT_EXIT:$?"
