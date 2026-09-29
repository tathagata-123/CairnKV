cmake_minimum_required(VERSION 3.25)

project(
    CairnKV
    VERSION 1.0.0
    DESCRIPTION "A compact educational LSM-tree key-value storage engine"
    LANGUAGES CXX
)

set(CMAKE_CXX_EXTENSIONS OFF)

option(
    CAIRNKV_ENABLE_SANITIZERS
    "Build with AddressSanitizer and UndefinedBehaviorSanitizer"
    OFF
)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    add_compile_options(-Wall -Wextra -Wpedantic -Werror)
endif()

if(CAIRNKV_ENABLE_SANITIZERS)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        message(FATAL_ERROR "ASan+UBSan requires a Clang or GNU C++ compiler")
    endif()

    add_compile_options(
        -fno-omit-frame-pointer
        -fsanitize=address,undefined
    )
    add_link_options(-fsanitize=address,undefined)
endif()

add_library(minilsm STATIC
    src/cache/block_cache.cpp
    src/cache/table_cache.cpp
    src/common/checksum.cpp
    src/common/coding.cpp
    src/common/file_io.cpp
    src/common/file_names.cpp
    src/common/internal_key.cpp
    src/compaction/compaction.cpp
    src/db/db.cpp
    src/db/db_compaction.cpp
    src/db/db_get.cpp
    src/db/db_impl.cpp
    src/db/db_lifecycle.cpp
    src/db/db_scan.cpp
    src/db/file_install.cpp
    src/iterator/forward_range_iterator.cpp
    src/iterator/memtable_iterator.cpp
    src/iterator/merging_iterator.cpp
    src/iterator/sstable_iterator_adapter.cpp
    src/iterator/user_iterator.cpp
    src/memtable/memtable.cpp
    src/manifest/manifest.cpp
    src/manifest/version.cpp
    src/options.cpp
    src/sstable/block.cpp
    src/sstable/bloom_filter.cpp
    src/sstable/sstable_builder.cpp
    src/sstable/sstable_format.cpp
    src/sstable/sstable_iterator.cpp
    src/sstable/sstable_reader.cpp
    src/status.cpp
    src/wal/wal.cpp
    src/wal/wal_format.cpp
)

target_compile_features(minilsm PUBLIC cxx_std_20)
find_package(Threads REQUIRED)
target_link_libraries(minilsm PUBLIC Threads::Threads)

target_include_directories(minilsm
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

add_executable(cairnkv_bench
    tools/cairnkv_bench.cpp
)

target_link_libraries(cairnkv_bench PRIVATE minilsm)

enable_testing()

add_executable(cairnkv_smoke_test
    tests/smoke_test.cpp
)

target_link_libraries(cairnkv_smoke_test PRIVATE minilsm)

add_test(
    NAME cairnkv.smoke
    COMMAND cairnkv_smoke_test
)

function(add_cairnkv_test component)
    set(target "cairnkv_${component}_test")

    add_executable(${target}
        "tests/${component}_test.cpp"
    )

    target_include_directories(${target}
        PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests"
    )

    target_link_libraries(${target} PRIVATE minilsm)

    add_test(
        NAME "cairnkv.${component}"
        COMMAND ${target}
    )
endfunction()

add_cairnkv_test(options_status)
add_cairnkv_test(coding)
add_cairnkv_test(internal_key)
add_cairnkv_test(checksum)
add_cairnkv_test(file_io)
add_cairnkv_test(file_names)
add_cairnkv_test(memtable)
add_cairnkv_test(manifest)
add_cairnkv_test(wal)
add_cairnkv_test(db)
add_cairnkv_test(db_m4)
add_cairnkv_test(db_m5)
add_cairnkv_test(db_m6)
add_cairnkv_test(db_m7)
add_cairnkv_test(m8_crash)
add_cairnkv_test(m8_corruption)
add_cairnkv_test(m8_randomized)
add_cairnkv_test(iterator)
add_cairnkv_test(block)
add_cairnkv_test(sstable)
add_cairnkv_test(sstable_corruption)
add_cairnkv_test(sstable_randomized)
add_cairnkv_test(bloom_filter)
add_cairnkv_test(wal_batch)
add_cairnkv_test(compaction_picker)
add_cairnkv_test(table_cache)
add_cairnkv_test(block_cache)
add_cairnkv_test(prefix_block)
add_cairnkv_test(db_revision)
add_cairnkv_test(parser_fuzz)

add_executable(cairnkv_experiments tools/cairnkv_experiments.cpp)
target_link_libraries(cairnkv_experiments PRIVATE minilsm)

option(CAIRNKV_COMPARE_LEVELDB "Build the optional LevelDB comparison" OFF)
if(CAIRNKV_COMPARE_LEVELDB)
    find_path(LEVELDB_INCLUDE_DIR leveldb/db.h REQUIRED)
    find_library(LEVELDB_LIBRARY leveldb REQUIRED)
    add_executable(compare_leveldb tools/compare_leveldb.cpp)
    target_compile_definitions(compare_leveldb PRIVATE CAIRNKV_USE_LEVELDB)
    target_include_directories(compare_leveldb PRIVATE ${LEVELDB_INCLUDE_DIR})
    target_link_libraries(compare_leveldb PRIVATE minilsm ${LEVELDB_LIBRARY})
    add_executable(compare_cairnkv tools/compare_leveldb.cpp)
    target_link_libraries(compare_cairnkv PRIVATE minilsm)
endif()

add_cairnkv_test(async_randomized)
add_cairnkv_test(async_crash)
