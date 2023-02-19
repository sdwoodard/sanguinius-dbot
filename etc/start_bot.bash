#!/bin/bash

export LD_LIBRARY_PATH="/usr/local/lib"
export PROJ_ROOT="$( cd -- "$(dirname "${0}")" > /dev/null 2>&1; pwd -P | xargs dirname)"

${PROJ_ROOT}/bin/sanguinius_dbot &

echo "Sanguinius: ONLINE"
