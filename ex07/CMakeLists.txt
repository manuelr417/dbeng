cmake_minimum_required(VERSION 4.2)
project(ex07)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(ex07 main.cpp Person.h PersonSerializer.cpp PersonCsv.cpp PersonGenerator.cpp BlockFile.cpp)
add_executable(ex07_tests tests.cpp Person.h PersonSerializer.cpp PersonCsv.cpp PersonGenerator.cpp BlockFile.cpp)
