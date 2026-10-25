#!/bin/bash
cd "$(dirname "$0")"
scripts/build-macos.sh 2>&1 | tee build-macos.log
echo "exit status: ${PIPESTATUS[0]}" | tee -a build-macos.log
