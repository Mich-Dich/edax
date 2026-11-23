cmake_minimum_required(VERSION 3.16)
project(z C)
add_library(z STATIC z.c)
target_link_libraries(z PRIVATE dl)
