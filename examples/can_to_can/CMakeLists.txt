# can_to_can - ESP-IDF project
cmake_minimum_required(VERSION 3.16)

# Pull in the shared board component from the repo root.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(can_to_can)
