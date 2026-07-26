#!/bin/bash

WORKDIR=`pwd`
export ROOT=/usr
export INSTALL_DIR=${ROOT}/local
mkdir -p $INSTALL_DIR

export CC=gcc
export CXX=g++
export AR=ar
export LD=ld
export NM=nm
export RANLIB=ranlib
export STRIP=strip

apt-get update && apt-get install -y libsoup-3.0 libcjson-dev

cd $ROOT
rm -rf rdk-halif-hdmi_cec
git clone https://github.com/rdkcentral/rdk-halif-hdmi_cec.git
export CEC_HALIF_PATH=$ROOT/rdk-halif-hdmi_cec
mkdir -p $INSTALL_DIR/include/ccec/drivers/
cp $CEC_HALIF_PATH/include/hdmi_cec_driver.h $INSTALL_DIR/include/ccec/drivers/

cd $ROOT
rm -rf telemetry
git clone https://github.com/rdkcentral/telemetry.git
export TELEMETRY_PATH=$ROOT/telemetry

echo "##### Building HdmiCec module"
cd $WORKDIR

# default PATHs - use `man readlink` for more info
# the path to combined build
export RDK_PROJECT_ROOT_PATH=${RDK_PROJECT_ROOT_PATH-`readlink -m ../../..`}

# path to build script (this script)
export RDK_SCRIPTS_PATH=${RDK_SCRIPTS_PATH-`readlink -m $0 | xargs dirname`}

# path to components sources and target
export RDK_SOURCE_PATH=${RDK_SOURCE_PATH-$RDK_SCRIPTS_PATH}

# default component name
export RDK_COMPONENT_NAME=${RDK_COMPONENT_NAME-`basename $RDK_SOURCE_PATH`}
cd ${RDK_SOURCE_PATH}

# functional modules
function configure()
{
   pd=`pwd`
   cd $RDK_SCRIPTS_PATH
   aclocal -I cfg
   libtoolize --automake
   autoheader
   automake --foreign --add-missing
   rm -f configure
   autoconf
   ./configure --with-libtool-sysroot=/ --disable-static --host=$HOST --disable-silent-rules --prefix=${RDK_FSROOT_PATH}/usr/
}

function clean()
{
    pd=`pwd`
    CLEAN_BUILD=1
    dnames="$RDK_SCRIPTS_PATH"
    for dName in $dnames
    do
        cd $dName
    	if [ -f Makefile ]; then
    		make distclean
	    fi
    	rm -f configure
    	rm -rf aclocal.m4 autom4te.cache config.log config.status libtool
	    rm -rf install
    	find . -iname "Makefile.in" -exec rm -f {} \;
    	ls cfg/* | grep -v "Makefile.am" | xargs rm -f
    	cd $pd
    done
    true #use this function to provide instructions to clean workspace
}

function build()
{
    cd ${RDK_SCRIPTS_PATH}

    make VERBOSE=1 AM_CXXFLAGS="-I${WORKDIR}/stubs -I${WORKDIR}/osal/include -I${WORKDIR}/ccec/include -I${TELEMETRY_PATH}/include"
}

function rebuild()
{
    clean
    configure
    build
    clean
}

rebuild
