#!/usr/bin/env bash
set -euo pipefail

# Configuration defaults
BUILD_SERVER_HOST="${BUILD_SERVER_HOST:-172.23.2.209}"
BUILD_SERVER_USER="${BUILD_SERVER_USER:-ubuntu}"
REMOTE_PROJECT_NAME="sung"
REMOTE_BASE_DIR="/home/${BUILD_SERVER_USER}/remote-build/${REMOTE_PROJECT_NAME}"
REMOTE_SOURCE_DIR="${REMOTE_BASE_DIR}/source"
REMOTE_BUILD_DIR="${REMOTE_BASE_DIR}/build"
REMOTE_LOCK_DIR="${REMOTE_BASE_DIR}/.build.lock"
LOCAL_OUTPUT_DIR="./build-output"
BUILD_MODE="${1:-release}"

root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
LOCKED=0

cleanup() {
    if [ "${LOCKED}" -eq 1 ]; then
        ssh "${BUILD_SERVER_USER}@${BUILD_SERVER_HOST}" "rm -rf '${REMOTE_LOCK_DIR}'" >/dev/null 2>&1 || true
    fi
}
trap cleanup EXIT SIGINT SIGTERM

echo "=================================================="
echo " Sung Remote Build System"
echo " Target Server: ${BUILD_SERVER_USER}@${BUILD_SERVER_HOST}"
echo " Mode: ${BUILD_MODE}"
echo " Local Root: ${root}"
echo "=================================================="

# [1/6] Checking SSH connection
echo "[1/6] Checking SSH connection to ${BUILD_SERVER_USER}@${BUILD_SERVER_HOST}..."
if ! ssh -o BatchMode=yes -o ConnectTimeout=5 "${BUILD_SERVER_USER}@${BUILD_SERVER_HOST}" "echo connection-ok" >/dev/null 2>&1; then
    echo "ERROR: Cannot connect to remote build server ${BUILD_SERVER_USER}@${BUILD_SERVER_HOST}."
    echo "Please verify network connectivity and SSH key setup."
    exit 1
fi
echo "SSH connection OK."

# [2/6] Acquiring remote build lock
echo "[2/6] Acquiring remote build lock..."
ssh "${BUILD_SERVER_USER}@${BUILD_SERVER_HOST}" "mkdir -p '${REMOTE_BASE_DIR}'"

LOCK_RESULT=$(ssh "${BUILD_SERVER_USER}@${BUILD_SERVER_HOST}" "
if mkdir '${REMOTE_LOCK_DIR}' 2>/dev/null; then
    echo '$(hostname -f):$$' > '${REMOTE_LOCK_DIR}/info'
    echo 'LOCK_ACQUIRED'
else
    echo 'LOCK_FAILED'
fi
")

if [ "${LOCK_RESULT}" != "LOCK_ACQUIRED" ]; then
    echo "ERROR: Remote build server is currently busy (lock active at ${REMOTE_LOCK_DIR})."
    exit 1
fi
LOCKED=1
echo "Remote build lock acquired."

# [3/6] Verifying remote build environment
echo "[3/6] Checking remote build environment..."
ENV_CHECK_CMD="
MISSING=''
command -v cmake >/dev/null 2>&1 || MISSING=\"\${MISSING} cmake\"
command -v ninja >/dev/null 2>&1 || MISSING=\"\${MISSING} ninja\"
command -v g++ >/dev/null 2>&1 || MISSING=\"\${MISSING} g++\"
command -v pkg-config >/dev/null 2>&1 || MISSING=\"\${MISSING} pkg-config\"
command -v python3 >/dev/null 2>&1 || MISSING=\"\${MISSING} python3\"
pkg-config --exists Qt6Core Qt6Gui Qt6Quick Qt6Multimedia Qt6Svg 2>/dev/null || MISSING=\"\${MISSING} Qt6-libraries\"
if [ -n \"\${MISSING}\" ]; then
    echo \"MISSING:\${MISSING}\"
else
    echo 'ENV_OK'
fi
"

ENV_STATUS=$(ssh "${BUILD_SERVER_USER}@${BUILD_SERVER_HOST}" "${ENV_CHECK_CMD}")

if [[ "${ENV_STATUS}" == MISSING:* ]]; then
    MISSING_TOOLS="${ENV_STATUS#MISSING:}"
    echo "ERROR: Remote build environment is incomplete."
    echo "Missing:${MISSING_TOOLS}"
    echo "Please run ./scripts/setup-build-server.sh to provision the build server."
    exit 1
fi
echo "Remote build environment verified."

# [4/6] Synchronizing source code
echo "[4/6] Syncing working tree to remote server..."
ssh "${BUILD_SERVER_USER}@${BUILD_SERVER_HOST}" "mkdir -p '${REMOTE_SOURCE_DIR}'"

# Construct exclude parameters to protect build caches & prevent leaking secrets/credentials
RSYNC_EXCLUDES=(
    "--exclude=.git/"
    "--exclude=/build/"
    "--exclude=/build-tests/"
    "--exclude=/build-output/"
    "--exclude=/runtime/"
    "--exclude=.cache/"
    "--exclude=.tmp/"
    "--exclude=.env"
    "--exclude=.env.*"
    "--exclude=cookies*"
    "--exclude=credentials*"
    "--exclude=secrets*"
    "--exclude=*.key"
    "--exclude=*.pem"
    "--exclude=library.json"
    "--exclude=settings.json"
    "--exclude=.vscode/"
    "--exclude=.idea/"
    "--exclude=.codex/"
    "--exclude=.agents/"
    "--exclude=.gemini/"
    "--exclude=verification/"
    "--exclude=*.log"
    "--exclude=__pycache__/"
    "--exclude=*.pyc"
)

if ! rsync -az --delete "${RSYNC_EXCLUDES[@]}" "${root}/" "${BUILD_SERVER_USER}@${BUILD_SERVER_HOST}:${REMOTE_SOURCE_DIR}/"; then
    echo "ERROR: Remote source synchronization failed."
    exit 1
fi
echo "Source synchronization complete."

# [5/6] Building project remotely
echo "[5/6] Building project on ${BUILD_SERVER_USER}@${BUILD_SERVER_HOST} (${BUILD_MODE} mode)..."

CMAKE_BUILD_TYPE="Release"
if [ "${BUILD_MODE}" = "debug" ]; then
    CMAKE_BUILD_TYPE="Debug"
fi

REMOTE_BUILD_CMD="
set -e
mkdir -p '${REMOTE_BUILD_DIR}'
cmake -S '${REMOTE_SOURCE_DIR}' -B '${REMOTE_BUILD_DIR}' -G Ninja -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE} -DBUILD_TESTING=OFF -DSUNG_DIAGNOSTICS=OFF
cmake --build '${REMOTE_BUILD_DIR}' --parallel \$(nproc)
"

if ! ssh "${BUILD_SERVER_USER}@${BUILD_SERVER_HOST}" "${REMOTE_BUILD_CMD}"; then
    echo "ERROR: Remote build failed."
    exit 1
fi
echo "Remote compilation succeeded."

# [6/6] Downloading artifact
echo "[6/6] Validating and downloading compiled artifact..."
REMOTE_BINARY="${REMOTE_BUILD_DIR}/sung"

CHECK_ARTIFACT_CMD="[ -f '${REMOTE_BINARY}' ] && [ -x '${REMOTE_BINARY}' ] && echo 'ARTIFACT_OK'"
if ! ssh "${BUILD_SERVER_USER}@${BUILD_SERVER_HOST}" "${CHECK_ARTIFACT_CMD}" | grep -q "ARTIFACT_OK"; then
    echo "ERROR: Build completed but expected artifact was not found at ${REMOTE_BINARY}."
    exit 1
fi

mkdir -p "${LOCAL_OUTPUT_DIR}"
LOCAL_BINARY="${LOCAL_OUTPUT_DIR}/sung"

if ! rsync -az "${BUILD_SERVER_USER}@${BUILD_SERVER_HOST}:${REMOTE_BINARY}" "${LOCAL_BINARY}"; then
    echo "ERROR: Downloading artifact from remote server failed."
    exit 1
fi

chmod +x "${LOCAL_BINARY}"

echo "=================================================="
echo " Build successful!"
echo " Remote Server: ${BUILD_SERVER_USER}@${BUILD_SERVER_HOST}"
echo " Mode: ${BUILD_MODE}"
echo " Artifact: ${LOCAL_BINARY}"
echo "=================================================="
