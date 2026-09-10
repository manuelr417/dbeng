cmake_minimum_required(VERSION 4.3)
project(ex09)

set(CMAKE_CXX_STANDARD 20)

add_executable(ex09 main.cpp Person.h PersonSerializer.cpp PersonCsv.cpp PersonGenerator.cpp BlockFile.cpp BufferPool.cpp DataFrame.h BufferPool.h ReplacementPolicy.h LRUPolicy.cpp LRUPolicy.h BufferManager.cpp BufferManager.h)
add_executable(ex09_demo ex09_demo.cpp Person.h PersonSerializer.cpp PersonCsv.cpp PersonGenerator.cpp BlockFile.cpp BufferPool.cpp DataFrame.h BufferPool.h ReplacementPolicy.h LRUPolicy.cpp LRUPolicy.h BufferManager.cpp BufferManager.h)
