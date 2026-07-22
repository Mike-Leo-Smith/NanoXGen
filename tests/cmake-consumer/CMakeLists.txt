cmake_minimum_required(VERSION 3.20)
project(NanoXGenConsumer LANGUAGES CXX)

find_package(NanoXGen CONFIG REQUIRED)

add_executable(nanoxgen_consumer main.cpp)
target_link_libraries(nanoxgen_consumer PRIVATE NanoXGen::nanoxgen)
