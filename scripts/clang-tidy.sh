#!/usr/bin/env bash
# Copyright (c) 2026 ADBC Drivers Contributors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#         http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# Run clang-tidy from the public adbc-drivers/dev manylinux image against the
# compile database produced by the generated Linux test build. A checkout-only
# pre-commit run has no compile database and skips this hook; generated build CI
# invokes it after compilation.
set -euo pipefail

repo_root="$(git rev-parse --show-toplevel)"
cd "$repo_root"

build_dir="${FIREBOLT_ADBC_BUILD_DIR:-build/ci-test-linux-amd64}"
if [[ ! -f "$build_dir/compile_commands.json" ]]; then
  printf 'Skipping clang-tidy: %s/compile_commands.json is missing\n' "$build_dir"
  exit 0
fi

image="${ADBC_DEV_IMAGE:-ghcr.io/adbc-drivers/dev:manylinux_2_28-cpp1.27.1}"
files=()
for file in "$@"; do
  case "$file" in
    *.cpp) files+=("/source/$file") ;;
    *) files=(); break ;;
  esac
done
if [[ ${#files[@]} -eq 0 ]]; then
  files=("/source/(src|tests/unit)/.*\\.cpp")
fi

docker run --rm \
  -u "$(id -u):$(id -g)" \
  -v "$repo_root:/source" \
  -w /source \
  "$image" \
  run-clang-tidy-18 -p "/source/$build_dir" -quiet \
    -header-filter='^/source/(src|tests/unit)/' \
    "${files[@]}"
