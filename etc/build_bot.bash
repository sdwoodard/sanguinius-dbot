#!/bin/bash

export PROJ_ROOT="$( cd -- "$(dirname "${0}")" > /dev/null 2>&1; pwd -P | xargs dirname)"

[[ -d ${PROJ_ROOT}/bin ]] || mkdir -p ${PROJ_ROOT}/bin
[[ -f ${PROJ_ROOT}/bin/sanguinius ]] && rm -f ${PROJ_ROOT}/bin/sanguinius

g++ ${PROJ_ROOT}/src/*.cpp -o ${PROJ_ROOT}/bin/sanguinius -ldpp
echo "Build complete."

