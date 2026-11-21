cmake_minimum_required(VERSION 3.16)
project(odd C)
execute_process(COMMAND git describe OUTPUT_VARIABLE ODD_VERSION)
add_library(odd odd.c)
