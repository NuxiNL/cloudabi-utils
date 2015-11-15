cmake_minimum_required(VERSION 3.3)
project(cloudabi-utils)

include_directories ("${PROJECT_SOURCE_DIR}/src/libcloudabi")
add_subdirectory(src/cloudabi-run)
add_subdirectory(src/libcloudabi)
