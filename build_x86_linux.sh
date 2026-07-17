#!/bin/bash
set -e

# HockeyStack custom release-build entry point; keep this with custom build branches.

# Dynamic paths
REPO_ROOT="$(git rev-parse --show-toplevel)"
HOME_DIR="$HOME"
CACHE_DIR="${HOME_DIR}/.cache/sccache"

# Build configuration
BUILD_DIR="${BUILD_DIR:-build_amd_release}"
BUILD_JOBS="${BUILD_JOBS:-}"
CLEAN_BUILD="${CLEAN_BUILD:-1}"

# Image configuration
IMAGE="clickhouse/binary-builder:local"

sync_rust_locks_to_vendor() {
    docker run --rm --platform linux/amd64 \
      -v "${REPO_ROOT}:/ClickHouse" \
      -w /ClickHouse \
      "$IMAGE" \
      bash -lc "cd /ClickHouse/rust/workspace && /rust/rustup/toolchains/nightly-2026-03-22-x86_64-unknown-linux-gnu/bin/cargo update --config=/ClickHouse/${BUILD_DIR}/contrib/corrosion-cmake/config.toml --offline"

    docker run --rm --platform linux/amd64 \
      -v "${REPO_ROOT}:/ClickHouse" \
      -w /ClickHouse/contrib/chdig \
      "$IMAGE" \
      bash -lc '
set -e
cargo=/rust/rustup/toolchains/nightly-2026-03-22-x86_64-unknown-linux-gnu/bin/cargo
update_precise() {
    pkg="$1"
    from="$2"
    to="$3"
    if perl -0ne "exit((/name = \"\\Q${pkg}\\E\"\\nversion = \"\\Q${from}\\E\"/) ? 0 : 1)" Cargo.lock; then
        "$cargo" update -p "${pkg}@${from}" --precise "${to}"
    fi
}

update_precise clap 4.6.0 4.6.1
update_precise clap_complete 4.6.0 4.6.5
update_precise clap_derive 4.6.0 4.6.1
update_precise axum 0.8.8 0.8.9
update_precise bitflags 2.11.0 2.11.1
update_precise bumpalo 3.20.2 3.20.3
update_precise cc 1.2.58 1.2.62
update_precise compact_str 0.8.1 0.8.2
update_precise compact_str 0.9.0 0.9.1
update_precise enumset 1.1.10 1.1.13
update_precise js-sys 0.3.92 0.3.99
update_precise libc 0.2.183 0.2.186
update_precise num-conv 0.2.1 0.2.2
update_precise onig 6.5.1 6.5.3
update_precise pin-project 1.1.11 1.1.13
update_precise plist 1.8.0 1.9.0
update_precise signal-hook 0.4.3 0.4.4
update_precise tokio 1.50.0 1.52.3
update_precise tokio-macros 2.7.1 2.7.0
update_precise tonic 0.14.5 0.14.6
update_precise tonic-prost 0.14.5 0.14.6
update_precise uuid 1.23.0 1.23.1
update_precise zerocopy 0.8.47 0.8.48
'
}

cleanup_rust_locks() {
    if [[ "${RUST_LOCK_WAS_CLEAN:-0}" == "1" ]]; then
        git checkout -- rust/workspace/Cargo.lock
    fi
    if [[ "${CHDIG_LOCK_WAS_CLEAN:-0}" == "1" ]]; then
        git -C contrib/chdig checkout -- Cargo.lock
    fi
}

if [[ "${1:-}" == "--sync-rust-locks-only" ]]; then
    sync_rust_locks_to_vendor
    exit 0
fi

# Check if local image exists, if not offer to build it
if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    echo "Image '$IMAGE' not found locally."
    echo "The Docker Hub 'latest' image has clang 19, but ClickHouse requires clang 21."
    echo ""
    echo "Options:"
    echo "  1) Build images locally (takes ~10-20 min)"
    echo "  2) Abort"
    echo ""
    read -p "Build locally? [y/N] " -n 1 -r
    echo
    if [[ $REPLY =~ ^[Yy]$ ]]; then
        echo "Building clickhouse/fasttest:local (amd64)..."
        docker build --platform linux/amd64 --network=host -t clickhouse/fasttest:local "${REPO_ROOT}/ci/docker/fasttest/"

        echo "Building clickhouse/binary-builder:local (amd64)..."
        docker build --platform linux/amd64 --network=host --build-arg FROM_TAG=local -t clickhouse/binary-builder:local "${REPO_ROOT}/ci/docker/binary-builder/"
    else
        echo "Aborted. You can also manually build the images with:"
        echo "  docker build --platform linux/amd64 --network=host -t clickhouse/fasttest:local ${REPO_ROOT}/ci/docker/fasttest/"
        echo "  docker build --platform linux/amd64 --network=host --build-arg FROM_TAG=local -t clickhouse/binary-builder:local ${REPO_ROOT}/ci/docker/binary-builder/"
        exit 1
    fi
fi

if [[ "${CLEAN_BUILD}" == "1" ]]; then
    rm -rf "${REPO_ROOT}/${BUILD_DIR}" 2>/dev/null || \
        docker run --rm \
            -v "${REPO_ROOT}:/ClickHouse" \
            "$IMAGE" \
            rm -rf "/ClickHouse/${BUILD_DIR}"
fi
mkdir -p "$CACHE_DIR" "${REPO_ROOT}/${BUILD_DIR}"

RUST_LOCK_WAS_CLEAN=0
CHDIG_LOCK_WAS_CLEAN=0
git diff --quiet -- rust/workspace/Cargo.lock && RUST_LOCK_WAS_CLEAN=1
git -C contrib/chdig diff --quiet -- Cargo.lock && CHDIG_LOCK_WAS_CLEAN=1

LOG_FILE="${REPO_ROOT}/${BUILD_DIR}/build_amd_release_$(date +%Y%m%d_%H%M%S).log"

echo "Building ClickHouse (amd_release) in ${REPO_ROOT}..."
echo "Build log: ${LOG_FILE}"

JOB_EXPORTS=""
CARGO_JOBS="${CARGO_BUILD_JOBS:-${BUILD_JOBS:-1}}"
CMAKE_JOB_FLAGS="-UPARALLEL_COMPILE_JOBS -DPARALLEL_LINK_JOBS=2"
if [[ -n "${BUILD_JOBS}" ]]; then
    JOB_EXPORTS="export CMAKE_BUILD_PARALLEL_LEVEL=${BUILD_JOBS}; "
    CMAKE_JOB_FLAGS="-DPARALLEL_COMPILE_JOBS=${BUILD_JOBS} -DPARALLEL_LINK_JOBS=2"
fi
JOB_EXPORTS="${JOB_EXPORTS}export CARGO_BUILD_JOBS=${CARGO_JOBS}; export CARGO_BUILD_RUSTC_WRAPPER=; "

docker run --rm --platform linux/amd64 \
  -v "${REPO_ROOT}:/ClickHouse" \
  -v "${CACHE_DIR}:/root/.cache/sccache" \
  -w /ClickHouse \
  "$IMAGE" \
  bash -c "${JOB_EXPORTS}cmake --debug-trycompile -DCMAKE_VERBOSE_MAKEFILE=1 -LA -S /ClickHouse -B /ClickHouse/${BUILD_DIR} -G Ninja -DCMAKE_BUILD_TYPE=Release -DENABLE_THINLTO=1 -DSANITIZE= -DENABLE_CHECK_HEAVY_BUILDS=1 -DBUILD_STRIPPED_BINARY=1 -DENABLE_CLICKHOUSE_SELF_EXTRACTING=1 -DCMAKE_C_COMPILER=clang-21 -DCMAKE_CXX_COMPILER=clang++-21 -DCOMPILER_CACHE=sccache -DCMAKE_TOOLCHAIN_FILE=/ClickHouse/cmake/linux/toolchain-x86_64.cmake -DENABLE_BUILD_PROFILING=1 -DENABLE_TESTS=0 -DENABLE_LEXER_TEST=0 -DENABLE_UTILS=0 -DCMAKE_FIND_PACKAGE_NO_PACKAGE_REGISTRY=ON -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_SYSCONFDIR=/etc -DCMAKE_INSTALL_LOCALSTATEDIR=/var -DCMAKE_SKIP_INSTALL_ALL_DEPENDENCY=ON -DSPLIT_DEBUG_SYMBOLS=ON -DBUILD_STANDALONE_KEEPER=1 -DCLICKHOUSE_OFFICIAL_BUILD=1 -DCLICKHOUSE_DISABLE_RUSTC_WRAPPER=1 ${CMAKE_JOB_FLAGS}" \
  > "${LOG_FILE}" 2>&1

sync_rust_locks_to_vendor >> "${LOG_FILE}" 2>&1

docker run --rm --platform linux/amd64 \
  -v "${REPO_ROOT}:/ClickHouse" \
  -v "${CACHE_DIR}:/root/.cache/sccache" \
  -w /ClickHouse \
  "$IMAGE" \
  bash -c "${JOB_EXPORTS}cmake --build /ClickHouse/${BUILD_DIR} --target clickhouse-bundle" \
  >> "${LOG_FILE}" 2>&1

cleanup_rust_locks

echo "Build finished."
echo "Binary: ${REPO_ROOT}/${BUILD_DIR}/programs/clickhouse"
