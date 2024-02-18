#!/bin/bash

#
# This script is known to work on:
# OS X 10.5.8, Xcode 3.1.2, SDK 10.5, MacPorts 2.3.3
# OS X 10.9.5, Xcode 5.1.1, SDK 10.9, MacPorts 2.3.2
# OS X 10.10.3, XCode 6.3.2, SDK 10.10, Homebrew 0.9.5/8da6986
#
# You may need to set PATH to include the location of your
# PackageMaker binary, if your system is old enough to need that.
# Setting MACOSX_DEPLOYMENT_TARGET will select an SDK as usual.
#
# If you are using Homebrew, you should install protobuf (and any
# other future Homebrew dependencies) with
# `--universal --build-bottle`.
# The first option should be fairly obvious; the second has the side
# effect of disabling Homebrew's overzealous processor optimization
# with (effectively) `-march=native`.
#

set -e

protobuf_LIBS=$(l=libprotobuf.a; for i in /opt/local/lib /usr/local/lib; do  if [ -f $i/$l ]; then echo $i/$l; fi; done)
if [ -z "$protobuf_LIBS" ]; then echo "Can't find libprotobuf.a"; exit 1; fi
export protobuf_LIBS
if ! pkg-config --cflags protobuf > /dev/null 2>&1; then
    protobuf_CFLAGS=-I$(for i in /opt /usr; do d=$i/local/include; if [ -d $d/google/protobuf ]; then echo $d; fi; done)
    if [ "$protobuf_CFLAGS" = "-I" ]; then echo "Can't find protobuf includes"; exit 1; fi
    export protobuf_CFLAGS
fi

echo "Building into prefix..."


#
# XXX This script abuses Configure's --prefix argument badly.  It uses
# it as a $DESTDIR, but --prefix can also affect paths in generated
# objects.  That is not *currently* a problem in mosh.
#
PREFIX="$(pwd)/prefix"

HOST="x86_64-apple-macosx${MACOSX_DEPLOYMENT_TARGET}"
ARCH_TRIPLES="x86_64-apple-macosx arm64-apple-macos"

pushd .. > /dev/null

if [ ! -f configure ];
then
    echo "Running autogen."
    PATH=/opt/local/bin:$PATH ./autogen.sh
fi

#
# Build archs one by one.
#
for triple in $ARCH_TRIPLES; do
    arch=$(echo $triple | cut -d- -f1)
    echo "Building for ${arch}..."
    prefix="${PREFIX}_${arch}"
    rm -rf "${prefix}"
    mkdir "${prefix}"
    if ./configure --prefix="${prefix}/local" --build="${triple}${MACOSX_DEPLOYMENT_TARGET}"\
		   --host="${HOST}" \
		   CC="cc -arch ${arch}" CPP="cc -arch ${arch} -E" CXX="c++ -arch ${arch}" \
		   TINFO_LIBS=-lncurses &&
	    make clean &&
	    make install -j8 V=1 &&
	    rm -f "${prefix}/etc"
    then
	# mosh-client built with Xcode 3.1.2 bus-errors if the binary is stripped.
	# strip "${prefix}/local/bin/mosh-client" "${prefix}/local/bin/mosh-server"
	BUILT_ARCHS="$BUILT_ARCHS $arch"
    fi
done

if [ -z "$BUILT_ARCHS" ]; then
    echo "No architectures built successfully"
    exit 1
fi

echo "Building universal binaries for archs ${BUILT_ARCHS}..."


rm -rf "$PREFIX"
# Copy one architecture to get all files into place.
for arch in $BUILT_ARCHS; do
    cp -Rp "${PREFIX}_${arch}" "${PREFIX}"
    break
done

# Build fat binaries
# XXX will break with spaces in pathname
for prog in local/bin/mosh-client local/bin/mosh-server; do
    archprogs=()
    for arch in $BUILT_ARCHS; do
	archprogs+=("${PREFIX}_${arch}/$prog")
    done
    lipo -create "${archprogs[@]}" -output "${PREFIX}/$prog"
done

perl -wlpi -e 's{#!/usr/bin/env perl}{#!/usr/bin/perl}' "$PREFIX/local/bin/mosh"

popd > /dev/null

PACKAGE_VERSION=$(cat ../VERSION.stamp)

OUTFILE="$PACKAGE_VERSION.pkg"

rm -f "$OUTFILE"

if which -s pkgbuild; then
    # To replace PackageMaker, you:
    # * make a bare package with the build products
    # * essentially take the Distribution file that PackageMaker generated and
    #   use it as the --distribution input file for productbuild
    echo "Preprocessing package description..."
    PKGID=edu.mit.mosh.mosh.pkg
    for file in Distribution; do
	sed -e "s/@PACKAGE_VERSION@/${PACKAGE_VERSION}/g" ${file}.in > ${file}
    done
    echo "Running pkgbuild/productbuild..."
    mkdir -p Resources/en.lproj
    cp -p copying.rtf Resources/en.lproj/License
    cp -p readme.rtf Resources/en.lproj/Readme
    pkgbuild --root "$PREFIX" --identifier $PKGID $PKGID
    productbuild --distribution Distribution \
		 --resources Resources \
		 --package-path . \
		 "$OUTFILE"
    echo "Cleaning up..."
    rm -rf "$PKGID"
else
    echo "Preprocessing package description..."
    INDIR=mosh-package.pmdoc.in
    OUTDIR=mosh-package.pmdoc
    mkdir -p "$OUTDIR"
    pushd "$INDIR" > /dev/null
    for file in *
    do
	sed -e 's/$PACKAGE_VERSION/'"$PACKAGE_VERSION"'/g' "$file" > "../$OUTDIR/$file"
    done
    popd > /dev/null
    echo "Running PackageMaker..."
    env PATH="/Applications/PackageMaker.app/Contents/MacOS:/Developer/Applications/Utilities/PackageMaker.app/Contents/MacOS:$PATH" PackageMaker -d mosh-package.pmdoc -o "$OUTFILE" -i edu.mit.mosh.mosh.pkg
    echo "Cleaning up..."
    rm -rf "$OUTDIR"
fi


if [ -f "$OUTFILE" ];
then
    echo "Successfully built $OUTFILE with archs ${BUILT_ARCHS}."
else
    echo "There was an error building $OUTFILE."
    false
fi
