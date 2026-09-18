cmake_minimum_required(VERSION 4.3)
project(ex10)

set(CMAKE_CXX_STANDARD 20)

set(SOURCES
    main.cpp
    part/Person.h part/PersonSerializer.cpp part/PersonCsv.cpp part/PersonGenerator.cpp
    file/BlockFile.cpp bufpool/BufferPool.cpp bufpool/DataFrame.h bufpool/BufferPool.h
    policy/ReplacementPolicy.h policy/LRUPolicy.cpp policy/LRUPolicy.h
    bufpool/BufferManager.cpp bufpool/BufferManager.h
    heapfile/SlottedPage.cpp heapfile/SlottedPage.h
    heapfile/HeapFile.cpp heapfile/HeapFile.h)

add_executable(ex10 ${SOURCES})
add_executable(ex10_demo ex10_demo.cpp part/Person.h part/PersonSerializer.cpp part/PersonCsv.cpp part/PersonGenerator.cpp file/BlockFile.cpp bufpool/BufferPool.cpp bufpool/DataFrame.h bufpool/BufferPool.h policy/ReplacementPolicy.h policy/LRUPolicy.cpp policy/LRUPolicy.h bufpool/BufferManager.cpp bufpool/BufferManager.h heapfile/SlottedPage.cpp heapfile/SlottedPage.h heapfile/HeapFile.cpp heapfile/HeapFile.h)

set(PROJECT_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(ex10 PRIVATE ${PROJECT_INCLUDE_DIR})
target_include_directories(ex10_demo PRIVATE ${PROJECT_INCLUDE_DIR})
