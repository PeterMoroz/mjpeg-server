#!/bin/bash

rm -rf _build
mkdir _build
cd _build
cmake -DOpenCV_DIR=${HOME}/opencv/build/lib/cmake/opencv4 \
	-DCMAKE_BUILD_TYPE=Debug ../
make -j`nproc`
