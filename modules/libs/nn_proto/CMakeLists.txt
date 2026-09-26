# SPDX-License-Identifier: Apache-2.0
#
# nn_proto — wire-format library for hub ↔ gateway ↔ device.
# Sources are added directly to the 'app' target, matching node_mgr's
# integration pattern (see modules/libs/node_mgr/CMakeLists.txt).

target_sources(app PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/nn_proto.c
)

target_include_directories(app PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
