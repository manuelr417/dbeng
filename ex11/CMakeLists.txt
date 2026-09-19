cmake_minimum_required(VERSION 4.3)
project(ex11)

set(CMAKE_CXX_STANDARD 20)

set(CORE_SOURCES
    file/BlockFile.cpp bufpool/BufferPool.cpp bufpool/DataFrame.h bufpool/BufferPool.h
    policy/ReplacementPolicy.h policy/LRUPolicy.cpp policy/LRUPolicy.h
    bufpool/BufferManager.cpp bufpool/BufferManager.h
    heapfile/SlottedPage.cpp heapfile/SlottedPage.h
    heapfile/HeapFile.cpp heapfile/HeapFile.h
    tuple/Column.h tuple/Tuple.cpp tuple/Tuple.h
    tuple/TupleSerializer.cpp tuple/TupleSerializer.h
    schema/ColumnSchema.h schema/TableSchema.cpp schema/TableSchema.h
    schema/TableSchemaJson.cpp schema/TableSchemaJson.h
    schema/RecordCodec.cpp schema/RecordCodec.h
    schema/TableCsv.cpp schema/TableCsv.h
    schema/TableGenerator.cpp schema/TableGenerator.h)

add_library(ex11core STATIC ${CORE_SOURCES})
target_include_directories(ex11core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(ex11 main.cpp)
target_link_libraries(ex11 PRIVATE ex11core)

add_executable(ex11_cli ex11_cli.cpp)
target_link_libraries(ex11_cli PRIVATE ex11core)
