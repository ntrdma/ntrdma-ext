#!/bin/bash -e

TOP="$(git rev-parse --show-toplevel)"
DEVOPS_TOP="$TOP"/devops-scripts

. "$TOP"/devops-scripts/bashlib/init.libraries

devops.parseBuildArgs "$0" "$@" 2> /dev/null
devops.getBuildInputs
OUTPUT_DIR=$(devops.outputDir)
OUTPUT_FILE=$OUTPUT_DIR/ntrdma_modules/ntrdma_modules.tar.gz
TMP_DIR=$TOP/output

echo "INFO: builder file is ${build_inputs[centos7-builder]}"
BUILD_DOCKER=$(<"${build_inputs[centos7-builder]}")
KERNEL_HEADERS_PATH=${build_inputs[coreos-linux-headers-gcached]}
KERNEL_VERSION_PATH="${build_inputs[coreos-version-text]:-${build_inputs[coreos-version]}}"

echo "INFO: kernel headers are $KERNEL_HEADERS_PATH version is $KERNEL_VERSION_PATH"

mkdir -p $TMP_DIR

if [[ $(/bin/diff -qN $TMP_DIR/version.txt $KERNEL_VERSION_PATH) ]]; then
        rm -fr $TMP_DIR/*
        tar -C $TMP_DIR -xvf $KERNEL_HEADERS_PATH
        cp $KERNEL_VERSION_PATH $TMP_DIR
fi

# Use devtoolset 7 (Updated toolchain) with gcc 7.3 instead of default 4.8
CMD="set +o nounset; source /opt/rh/devtoolset-7/enable; set -o nounset;
     gcc --version;
     make -j CONFIG_NTC=m CONFIG_NTRDMA=m  DEBUG=1 -C ${TMP_DIR}/lib/modules/*/build M=${TOP} modules"

echo "Kernel header ready firing make from docker..."

devops.run_in_builder_docker "$CMD" "$TOP" "$BUILD_DOCKER"

echo "NTRDMA compilation completed, starting packaging..."
mkdir -p $OUTPUT_DIR/ntrdma_modules/

tar -czvf $OUTPUT_FILE --xform s:^.*/:: drivers/ntc/ntc_ntb.ko drivers/infiniband/hw/ntrdma/ntrdma.ko

