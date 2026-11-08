#!/usr/bin/env bash
# Rebuild Core and Blocks together: native Runtime adoption changes the plugin ABI.
set -euo pipefail

if [[ $# -ne 3 ]]; then
  echo "usage: $0 CORE_SOURCE BLOCKS_SOURCE BUILD_ROOT" >&2
  exit 2
fi
core_source=$1
blocks_source=$2
build_root=$3
sdk_prefix=${SDK_PREFIX:-/opt/gr4cp-sdk}
configure=(cmake)
common=(
  "-DCMAKE_INSTALL_PREFIX=${sdk_prefix}"
  "-DCMAKE_PREFIX_PATH=${sdk_prefix};/opt/gnuradio4"
  "-DCMAKE_BUILD_TYPE=${SDK_BUILD_TYPE:-RelWithDebInfo}"
  -DENABLE_TESTING=OFF
  -DENABLE_EXAMPLES=OFF
  -DUSE_CCACHE=OFF
)
if [[ ${CMAKE_WRAPPER:-} == emcmake ]]; then
  configure=(emcmake cmake)
  common+=(
    "-DCMAKE_FIND_ROOT_PATH=${sdk_prefix};/opt/gnuradio4"
    "-DCMAKE_CROSSCOMPILING_EMULATOR=$(command -v node)"
  )
fi
if [[ -n ${SDK_CXX_FLAGS:-} ]]; then
  common+=("-DCMAKE_CXX_FLAGS=${SDK_CXX_FLAGS}")
fi
if [[ -n ${SDK_LINKER_FLAGS:-} ]]; then
  common+=(
    "-DCMAKE_EXE_LINKER_FLAGS=${SDK_LINKER_FLAGS}"
    "-DCMAKE_SHARED_LINKER_FLAGS=${SDK_LINKER_FLAGS}"
  )
fi

core_flags=()
if [[ ${SDK_CXX_FLAGS:-} == *-fsanitize=address* ]]; then
  core_flags+=(-DADDRESS_SANITIZER=ON)
fi

"${configure[@]}" -S "${core_source}" -B "${build_root}/core" "${common[@]}" "${core_flags[@]}" \
  -DENABLE_TOOLS=OFF -DGR_USE_FETCHCONTENT_DEPS=ON
cmake --build "${build_root}/core" --parallel 2
cmake --install "${build_root}/core"

# Start with an empty plugin directory in this prefix; SDK-image plugins use the previous Core ABI.
"${configure[@]}" -S "${blocks_source}" -B "${build_root}/blocks" "${common[@]}" \
  -DGR4_ENABLE_AUDIO=OFF -DGR4_ENABLE_SDR=OFF -DGR4_ENABLE_HTTP_TESTS=OFF
cmake --build "${build_root}/blocks" --parallel 2
cmake --install "${build_root}/blocks"
