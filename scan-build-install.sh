#!/bin/sh -x

CLANG_CHECKER_NAME=checker-278

cd ~

if [ ! -d checker-278 ]
then
  curl http://clang-analyzer.llvm.org/downloads/checker-278.tar.bz2 -o ~/$CLANG_CHECKER_NAME.tar.bz2
  tar -xf ~/$CLANG_CHECKER_NAME.tar.bz2
fi

export SCAN_BUILD_PATH=~/$CLANG_CHECKER_NAME/bin/scan-build
export PATH=$PATH:$SCAN_BUILD_PATH/bin

cd -
