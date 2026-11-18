# SPDX-License-Identifier: Apache-2.0

# OpenOCD debug adapter: the stem of an OpenOCD interface/<name>.cfg file.
# The default, cmsis-dap, is the onboard DAPLink. Use stlink for an ST-Link on
# the 10-pin DEBUG header:
#   west build -b adi_sdp_k1 -- -DADI_SDP_K1_DEBUG_ADAPTER=stlink
if("${ADI_SDP_K1_DEBUG_ADAPTER}" STREQUAL "")
  set(ADI_SDP_K1_DEBUG_ADAPTER "cmsis-dap")
endif()

board_runner_args(openocd "--config=interface/${ADI_SDP_K1_DEBUG_ADAPTER}.cfg")
board_runner_args(openocd "--config=${BOARD_DIR}/support/openocd.cfg")
board_runner_args(jlink "--device=STM32F469NI" "--speed=4000")

include(${ZEPHYR_BASE}/boards/common/openocd.board.cmake)
include(${ZEPHYR_BASE}/boards/common/jlink.board.cmake)
