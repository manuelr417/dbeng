cmake_minimum_required(VERSION 4.3)
project(ex17)

set(CMAKE_CXX_STANDARD 20)

set(CORE_SOURCES
    file/BlockFile.cpp bufpool/BufferPool.cpp bufpool/DataFrame.h bufpool/BufferPool.h
    policy/ReplacementPolicy.h policy/LRUPolicy.cpp policy/LRUPolicy.h
    bufpool/BufferManager.cpp bufpool/BufferManager.h
    heapfile/SlottedPage.cpp heapfile/SlottedPage.h
    heapfile/HeapFile.cpp heapfile/HeapFile.h
    tuple/Column.h tuple/Tuple.cpp tuple/Tuple.h
    schema/ColumnSchema.h schema/TableSchema.cpp schema/TableSchema.h
    schema/TupleSerializer.cpp schema/TupleSerializer.h
    schema/TableSchemaJson.cpp schema/TableSchemaJson.h
    schema/TableCsv.cpp schema/TableCsv.h
    schema/TableGenerator.cpp schema/TableGenerator.h
    common/Error.h
    sql/Token.h
    sql/Lexer.h sql/Lexer.cpp
    sql/Ast.h
    sql/Parser.h sql/Parser.cpp
    sql/AstPrinter.h sql/AstPrinter.cpp
    sql/AstDraw.h sql/AstDraw.cpp
    index/KeyCodec.h index/KeyCodec.cpp
    index/IndexPages.h index/IndexPages.cpp
    index/IsamIndex.h index/IsamIndex.cpp
    hashindex/HashPages.h hashindex/HashPages.cpp
    hashindex/StaticHashPages.h hashindex/StaticHashPages.cpp
    hashindex/StaticHashIndex.h hashindex/StaticHashIndex.cpp
    hashindex/ExtensibleHashIndex.h hashindex/ExtensibleHashIndex.cpp
    group/GroupBy.h group/GroupBy.cpp
    group/PartitionRun.h group/PartitionRun.cpp
    tools/BulkLoader.h tools/BulkLoader.cpp
    exec/Value.h exec/Value.cpp
    exec/Binder.h exec/Binder.cpp
    exec/Evaluator.h exec/Evaluator.cpp
    exec/Planner.h exec/Planner.cpp
    exec/Executor.h exec/Executor.cpp)

add_library(ex17core STATIC ${CORE_SOURCES})
target_include_directories(ex17core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(ex17
    main.cpp
    check_storage.cpp
    check_sql.cpp
    check_index.cpp
    check_hash.cpp
    check_static.cpp
    check_exec.cpp
    check_loader.cpp
    check_group.cpp)
target_link_libraries(ex17 PRIVATE ex17core)

add_executable(ex17_cli ex17_cli.cpp)
target_link_libraries(ex17_cli PRIVATE ex17core)

add_executable(parts_bench parts_bench.cpp)
target_link_libraries(parts_bench PRIVATE ex17core)
