#!/usr/bin/env bash
set -euo pipefail

PROJECT_NAME="video-pipeline"
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build"

usage() {
    cat <<EOF
Usage:
  ./build.sh build [Debug|Release] [test] [--no-pcap] [--profile tauri|native|all|custom]
  ./build.sh run   [Debug|Release] [--no-pcap] [--profile tauri|native|all|custom]
  ./build.sh test  [Debug|Release] [--no-pcap] [--profile tauri|native|all|custom]
  ./build.sh clean
EOF
}

action="${1:-build}"
shift || true

config="${1:-Debug}"
case "${config,,}" in
    debug|release|relwithdebinfo|minsizerel)
        shift || true
        ;;
    *)
        config="Debug"
        ;;
esac

tests="OFF"
pcap="ON"
profile="tauri"
cli="ON"
tauri_module="OFF"
native_player="OFF"

while [[ $# -gt 0 ]]; do
    case "${1,,}" in
        test|--test|--tests)
            tests="ON"
            ;;
        --no-pcap)
            pcap="OFF"
            ;;
        --profile)
            shift
            if [[ $# -eq 0 ]]; then
                echo "--profile requires tauri, native, all, or custom" >&2
                exit 1
            fi
            profile="${1,,}"
            ;;
        --profile=*)
            profile="${1#*=}"
            profile="${profile,,}"
            ;;
        --no-cli)
            cli="OFF"
            ;;
        --tauri-module)
            tauri_module="ON"
            ;;
        --native-player)
            native_player="ON"
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Unknown argument: $1" >&2
            usage
            exit 1
            ;;
    esac
    shift
done

if [[ "${action,,}" == "test" ]]; then
    tests="ON"
fi

configure() {
    cmake -S "${PROJECT_ROOT}" -B "${BUILD_DIR}" \
        -DCMAKE_BUILD_TYPE="${config}" \
        -DBUILD_TESTS="${tests}" \
        -DENABLE_PCAP="${pcap}" \
        -DVIDEO_PIPELINE_PROFILE="${profile}" \
        -DVIDEO_PIPELINE_BUILD_CLI="${cli}" \
        -DVIDEO_PIPELINE_BUILD_TAURI_MODULE="${tauri_module}" \
        -DVIDEO_PIPELINE_BUILD_NATIVE_PLAYER="${native_player}"
}

build() {
    configure
    cmake --build "${BUILD_DIR}" --config "${config}" --parallel
}

app_path() {
    local ext=""
    if [[ "${OS:-}" == "Windows_NT" ]]; then
        ext=".exe"
    fi

    local candidates=(
        "${BUILD_DIR}/bin/${PROJECT_NAME}${ext}"
        "${BUILD_DIR}/bin/${config}/${PROJECT_NAME}${ext}"
        "${BUILD_DIR}/${config}/${PROJECT_NAME}${ext}"
        "${BUILD_DIR}/${PROJECT_NAME}${ext}"
    )

    for candidate in "${candidates[@]}"; do
        if [[ -f "${candidate}" ]]; then
            echo "${candidate}"
            return 0
        fi
    done

    echo "Could not find ${PROJECT_NAME} executable under ${BUILD_DIR}" >&2
    return 1
}

case "${action,,}" in
    clean)
        rm -rf "${BUILD_DIR}"
        echo "Clean completed."
        ;;
    build)
        build
        if [[ "${cli}" == "ON" ]]; then
            echo "Build completed: $(app_path)"
        else
            echo "Build completed."
        fi
        ;;
    run)
        if [[ "${cli}" != "ON" ]]; then
            echo "Action 'run' requires VIDEO_PIPELINE_BUILD_CLI=ON. Remove --no-cli and try again." >&2
            exit 1
        fi
        build
        exe="$(app_path)"
        echo "Running: ${exe}"
        "${exe}"
        ;;
    test)
        build
        ctest --test-dir "${BUILD_DIR}" -C "${config}" --output-on-failure
        ;;
    -h|--help)
        usage
        ;;
    *)
        echo "Unknown action: ${action}" >&2
        usage
        exit 1
        ;;
esac
