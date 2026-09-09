#!/usr/bin/env bash
set -euo pipefail

# Configuration defaults
BUILD_SERVER_HOST="${BUILD_SERVER_HOST:-172.23.2.209}"
BUILD_SERVER_USER="${BUILD_SERVER_USER:-ubuntu}"
BUILD_SERVER_PASS="${BUILD_SERVER_PASS:-ubuntu}"
REMOTE_PROJECT_NAME="sung"
REMOTE_BUILD_DIR="/home/${BUILD_SERVER_USER}/remote-build/${REMOTE_PROJECT_NAME}"

echo "=================================================="
echo " Setting up Remote Build Server"
echo " Target: ${BUILD_SERVER_USER}@${BUILD_SERVER_HOST}"
echo " Directory: ${REMOTE_BUILD_DIR}"
echo "=================================================="

# 1. Test SSH Connection
echo "[1/4] Testing SSH connection..."
if ! ssh -o BatchMode=yes -o ConnectTimeout=5 "${BUILD_SERVER_USER}@${BUILD_SERVER_HOST}" "echo connection-ok" >/dev/null 2>&1; then
    echo "ERROR: Cannot connect to remote build server ${BUILD_SERVER_USER}@${BUILD_SERVER_HOST} via SSH."
    echo "Please verify network connectivity and SSH key configuration."
    exit 1
fi
echo "SSH connection OK."

# 2. Check and Install Required System Packages
echo "[2/4] Installing system build dependencies on remote server..."
REQUIRED_PKGS=(
    cmake
    ninja-build
    build-essential
    g++
    pkg-config
    qt6-base-dev
    qt6-declarative-dev
    qt6-multimedia-dev
    qt6-svg-dev
    qt6-tools-dev
    libqt6svg6-dev
    python3
    python3-venv
    python3-pip
    ffmpeg
    rsync
    libxkbcommon-dev
)

ssh "${BUILD_SERVER_USER}@${BUILD_SERVER_HOST}" "bash -s" << EOF
set -e
echo '${BUILD_SERVER_PASS}' | sudo -S DEBIAN_FRONTEND=noninteractive apt-get update -qq
echo '${BUILD_SERVER_PASS}' | sudo -S DEBIAN_FRONTEND=noninteractive apt-get install -y -qq ${REQUIRED_PKGS[*]}
EOF

# 3. Create Remote Directory Structure
echo "[3/4] Creating remote build directory..."
ssh "${BUILD_SERVER_USER}@${BUILD_SERVER_HOST}" "mkdir -p '${REMOTE_BUILD_DIR}'"

# 4. Verify Toolchain
echo "[4/4] Verifying installed remote toolchain..."
VERIFY_CMD="which cmake ninja g++ pkg-config python3 rsync >/dev/null && pkg-config --exists Qt6Core Qt6Gui Qt6Quick Qt6Multimedia Qt6Svg && echo 'toolchain-ok'"

if ssh "${BUILD_SERVER_USER}@${BUILD_SERVER_HOST}" "${VERIFY_CMD}" | grep -q "toolchain-ok"; then
    echo "Build environment setup complete and verified!"
else
    echo "ERROR: Toolchain verification failed on remote server."
    exit 1
fi

echo "=================================================="
echo " Remote build server is ready for Sung builds!"
echo " Run ./scripts/build-remote.sh to start a build."
echo "=================================================="
