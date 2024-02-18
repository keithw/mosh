#!/bin/sh

#
# Install Homebrew dependencies
#
# This script handles build dependencies other than those provided by
# MacOS and Xcode, for a Mosh build using macosx/build.sh or the
# native autoconf/automake build for CI.  It is intended to be used by
# a build system, and should be agnostic to any particular system.
#
# Similar scripts could be developed for MacPorts, direct dependency
# builds, etc.
#

#
# Install and/or configure the system used to provide dependencies.
#
install()
{
    # Straight from https://brew.sh
    if ! brew --version > /dev/null 2>&1; then
        /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
    fi
}

#
# Install up-to-date build dependencies required for a development or
# CI build.  These dependencies only need to provide runtime
# dependencies for the build system, support for things like previous
# OS versions and fat binaries is not needed.
#
deps()
{
    brew update
    brew update
    brew upgrade
    brew reinstall tmux
    brew reinstall protobuf
}

#
# Install build dependencies required for the MacOS package build.
# Runtime dependencies are required to support the targeted OS X
# version, static libraries, and fat binaries for the package build.
#
# This reinstalls protobuf with --universal --build-bottle to get a
# fat library that will run on any machine.  (This takes about 15
# minutes on current Travis infrastructure.)
#
package_deps()
{
    deps
    brew rm protobuf
    brew install protobuf --universal --build-bottle
}

#
# Describe the dependencies installed and used as best as possible.
#
describe()
{
    brew --version > brew-version.txt
    brew info --json=v1 --installed > brew-info.json
}

#
# Do something.
#
set -e
"$@"
