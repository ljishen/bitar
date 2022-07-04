#!/usr/bin/env bash
# MIT License
#
# Copyright (c) 2022 Jianshen Liu
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in all
# copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.

set -euo pipefail

OS_RELEASE_FILE=/etc/os-release

if ! [[ -f "$OS_RELEASE_FILE" ]]; then
    echo >&2 "[ERROR] Unable to read from file $OS_RELEASE_FILE"
    exit 1
fi

# ID should be a lower-case string
#   https://www.freedesktop.org/software/systemd/man/os-release.html#ID=
# shellcheck source=/dev/null
OS_ID_CODENAME=$(. $OS_RELEASE_FILE && echo "${ID:-}_${VERSION_CODENAME:-}")
if [[ -z "$OS_ID_CODENAME" ]]; then
    echo >&2 "[ERROR] Unable to read the ID and CODENAME of the operating system." \
        "Abort installation."
    exit 1
fi

if ! [[ "$OS_ID_CODENAME" =~ ^(ubuntu|debian)_ ]]; then
    echo >&2 "[ERROR] We only support installing dependencies on Debian and" \
        "Ubuntu derivatives."
    exit 1
fi

if [[ "$OS_ID_CODENAME" == "debian_"* && "$OS_ID_CODENAME" != "debian_bullseye" ]]; then
    echo >&2 "[ERROR] For Debian OS, we require at least Debian 11 (bullseye)."
    exit 1
fi

APT_NO_RECOMMENDS="sudo apt -y --no-install-recommends"

# Use `tr`` to convert to lowercase string
# https://stackoverflow.com/a/2264537/2926646
MACHINE_HARDWARE_NAME=$(uname -m | tr '[:upper:]' '[:lower:]')
if [[ "$MACHINE_HARDWARE_NAME" == "x86_64" ]]; then
    MACHINE_HARDWARE_NAME=x64
fi

cat <<EOF


# ---------------------------------------------------
# ------------- Prerequisite Dependencies -----------
# ---------------------------------------------------
EOF

# lsb-release, wget, software-properties-common, gnupg are required by llvm.sh.
# build-essential is required by building include-what-you-use.
# ninja-build is required by building vcpkg-tool.
# curl, zip, unzip, tar are required by bootstraping vcpkg.
# flex, bison are required by building Arrow libraries.
sudo apt update &&
    $APT_NO_RECOMMENDS install \
        lsb-release \
        wget \
        software-properties-common \
        gnupg \
        build-essential \
        ninja-build \
        curl \
        zip \
        unzip \
        tar \
        flex \
        bison \
        pkg-config \
        ccache \
        cppcheck

cat <<EOF


# ---------------------------------------------------
# ---------- Install/Update CMake Software ----------
# ---------------------------------------------------
EOF

# Install the latest version through additional APT repository
if [[ "$OS_ID_CODENAME" == "ubuntu_"* ]]; then
    # Use the Kitware APT repository, but it only supports
    # Xenial (16.04), Bionic (18.04), and Focal (20.04).
    # See https://apt.kitware.com/
    curl -fsSL https://apt.kitware.com/kitware-archive.sh | sudo sh
    $APT_NO_RECOMMENDS install cmake cmake-curses-gui
elif [[ "$OS_ID_CODENAME" == "debian_"* ]]; then
    cat <<EOF | sudo tee /etc/apt/sources.list.d/backports.list >/dev/null
deb http://deb.debian.org/debian bullseye-backports main contrib non-free
deb-src http://deb.debian.org/debian bullseye-backports main contrib non-free
EOF
    sudo apt update &&
        $APT_NO_RECOMMENDS -t bullseye-backports install \
            cmake cmake-curses-gui
fi

cat <<EOF


# ---------------------------------------------------
# -------- Install/Update GCC Software -------
# ---------------------------------------------------
EOF

GCC_VERSION=11

if [[ "$OS_ID_CODENAME" == "ubuntu_"* ]]; then
    sudo add-apt-repository ppa:ubuntu-toolchain-r/test -y &&
        $APT_NO_RECOMMENDS install g++-$GCC_VERSION
elif [[ "$OS_ID_CODENAME" == "debian_"* ]]; then
    cat <<EOF | sudo tee /etc/apt/sources.list.d/debian-testing.list >/dev/null
deb http://deb.debian.org/debian testing main contrib non-free
deb-src http://deb.debian.org/debian testing main contrib non-free
EOF
    sudo apt update &&
        $APT_NO_RECOMMENDS -t testing install g++-$GCC_VERSION
fi

sudo update-alternatives \
    --install /usr/bin/gcc gcc /usr/bin/gcc-$GCC_VERSION $GCC_VERSION \
    --slave /usr/bin/g++ g++ /usr/bin/g++-$GCC_VERSION \
    --slave /usr/bin/gcov gcov /usr/bin/gcov-$GCC_VERSION

cat <<EOF


# ---------------------------------------------------
# -------- Install/Update LLVM/Clang Software -------
# ---------------------------------------------------
EOF

# Install through additional APT repository
# See https://apt.llvm.org/
LLVM_VERSION=14

# Use the BASH_XTRACEFD (from bash-4.1) to ditch the debug output from the script
exec {BASH_XTRACEFD}>/dev/null
sudo bash <<EOF
curl -fsSL https://apt.llvm.org/llvm.sh \
    | { exec $BASH_XTRACEFD>/dev/null && BASH_XTRACEFD=$BASH_XTRACEFD bash -s \
    -- $LLVM_VERSION all; }
EOF
# Close the file descriptor
exec {BASH_XTRACEFD}>&-

# Create symbolic links
CLANG_BINS=(llvm-symbolizer clang-tidy clang-format clang clang++)
for bin in "${CLANG_BINS[@]}"; do
    bin_path="$(which "$bin"-$LLVM_VERSION)"
    sudo ln --force --symbolic "$bin_path" "$(dirname "$bin_path")"/"$bin"
done

cat <<EOF


# ---------------------------------------------------
# ----------- Install Include-What-You-Use ----------
# ---------------------------------------------------
EOF

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &>/dev/null && pwd)
IWYU_INSTALL_DIR="$SCRIPT_DIR"/../opt/$MACHINE_HARDWARE_NAME
IWYU_SOURCE_DIR="$IWYU_INSTALL_DIR"/src/include-what-you-use

if [[ -x "$IWYU_INSTALL_DIR"/bin/include-what-you-use ]]; then
    echo "[INFO] include-what-you-use is already installed. Try updating..."
fi

if [[ -d "$IWYU_SOURCE_DIR" ]]; then
    echo "[INFO] Found existing source repository. Try updating..."
    cd "$IWYU_SOURCE_DIR"
    OLD_COMMIT_HASH=$(git rev-parse HEAD)
    (git remote set-branches --add origin clang_$LLVM_VERSION &&
        git fetch origin clang_$LLVM_VERSION &&
        git reset --hard origin/clang_$LLVM_VERSION) ||
        (echo "[INFO] Failed to update the source repository. Removing it before re-cloning..." &&
            cd .. && rm --force --recursive "$IWYU_SOURCE_DIR")
fi

if ! [[ -d "$IWYU_SOURCE_DIR" ]]; then
    git clone --single-branch --branch clang_$LLVM_VERSION \
        https://github.com/include-what-you-use/include-what-you-use.git \
        "$IWYU_SOURCE_DIR" && cd "$IWYU_SOURCE_DIR"
fi

IWYU_BUILD_DIRNAME=build
CURRENT_COMMIT_HASH=$(git rev-parse HEAD)
if ! [[ -x "$IWYU_INSTALL_DIR"/bin/include-what-you-use ]] ||
    [[ "${OLD_COMMIT_HASH:-}" != "$CURRENT_COMMIT_HASH" ]]; then
    rm --force --recursive "$IWYU_BUILD_DIRNAME" &&
        mkdir "$IWYU_BUILD_DIRNAME" && cd "$IWYU_BUILD_DIRNAME"
    cmake -G "Unix Makefiles" \
        -DCMAKE_PREFIX_PATH="/usr/lib/llvm-$LLVM_VERSION" \
        -DCMAKE_INSTALL_PREFIX="$IWYU_INSTALL_DIR" ..
    # Generate executable "$IWYU_INSTALL_DIR"/bin/include-what-you-use
    cmake --build . -j
    cmake --install .
else
    echo "[INFO] Nothing to update"
fi

echo
echo "Done!"
