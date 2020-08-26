#!/bin/bash

TARGET="host"
BUILD="debug"

while [ -n "$1" ]
do
	case "$1" in
	-h) echo "$0 -t <target: rpi|host> -b <build: debug|release> -h <print help>"
	exit ;;
	-b) BUILD="$2"
	shift ;;
	-t) TARGET="$2"
	shift ;;
	*) echo "unknown option $1"
	exit;;
	esac
	shift
done

if [ $TARGET = "host" ]
then
	echo "Target: host"
elif [ $TARGET = "rpi" ]
then
	echo "Target: rpi"
else
	echo "Unknown target $TARGET !"
	exit 1
fi


if [ $BUILD = "debug" ]
then
	echo "Build: debug"
	BUILD_TYPE="Debug"
elif [ $BUILD = "release" ]
then
	echo "Build: release"
	BUILD_TYPE="Release"
else
	echo "Unknown build type $BUILD !"
	exit 1
fi

BUILD_DIR="_build-$BUILD-$TARGET"
echo "BUILD directory $BUILD_DIR"

rm -rf $BUILD_DIR
mkdir $BUILD_DIR
cd $BUILD_DIR

if [ $TARGET = "rpi" ]
then
	if [ -f "../toolchain-rpi.cmake" ]
	then
		cmake -DCMAKE_BUILD_TYPE=$BUILD_TYPE -DCMAKE_TOOLCHAIN_FILE=../toolchain-rpi.cmake ../
	else
		echo "The file 'toolchain-rpi.cmake' not found!"
		exit 1
	fi
else
	cmake -DCMAKE_BUILD_TYPE=$BUILD_TYPE ../
fi

# rm -rf _build
# mkdir _build
# cd _build
# cmake -DCMAKE_BUILD_TYPE=Debug ../
make -j`nproc`
