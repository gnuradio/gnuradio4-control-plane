cmake_minimum_required(VERSION 3.25)

project(gr4_control_plane_mvp VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(GNUInstallDirs)

option(USE_CCACHE "Use ccache if available" ON)
option(ENABLE_TESTING "Build control-plane tests" ON)

option(GR4CP_ENABLE_GR4_CATALOG "Enable the required GNU Radio 4-backed block catalog provider" ON)

set(GR4CP_EXTRA_PLUGIN_DIRECTORIES "" CACHE STRING "Colon-separated plugin directories to search in addition to the ones under the gnuradio4 prefix, for a block library installed under its own prefix")
set(GR4CP_PLUGIN_DIRECTORIES "" CACHE STRING "Colon-separated plugin directories that replace the ones derived from the gnuradio4 prefix; use it when only a subset of an installation should be loaded")

# Use ccache if found and enabled
find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM AND USE_CCACHE)
    message(STATUS "ccache found and will be used")
    set_property(GLOBAL PROPERTY RULE_LAUNCH_COMPILE "${CCACHE_PROGRAM}")
else()
    message(STATUS "ccache will not be used")
endif()

# Prefer forced colored compiler output if there's a chance we'd otherwise get none at all. Ninja and ccache
# can consume compiler output and break compiler terminal color auto-detection.
if((CMAKE_GENERATOR STREQUAL "Ninja" OR (CCACHE_PROGRAM AND USE_CCACHE)) AND NOT DEFINED CMAKE_COLOR_DIAGNOSTICS)
    message(
        STATUS
        "Forcing compiler color output due to the use of Ninja and/or ccache. Use -DCMAKE_COLOR_DIAGNOSTICS=OFF to turn it off."
    )
    set(CMAKE_COLOR_DIAGNOSTICS ON)
endif()

# Third-party dependencies come from their own CMake package configs, as installed by gr-deps or a distribution. Their
# transitive requirements (e.g. cpp-httplib's OpenSSL, zlib or Brotli support) are carried by those configs.
find_package(nlohmann_json CONFIG REQUIRED)
find_package(Threads REQUIRED)

# only the native-only HTTP transport and CLI use these
if(NOT EMSCRIPTEN)
    find_package(Boost CONFIG REQUIRED)
    find_package(httplib CONFIG REQUIRED)
endif()

set(GR4CP_HAVE_GNURADIO4 FALSE)
if(GR4CP_ENABLE_GR4_CATALOG)
    find_package(gnuradio4 CONFIG REQUIRED)
    set(GR4CP_HAVE_GNURADIO4 TRUE)
    cmake_path(GET gnuradio4_DIR PARENT_PATH _gr4cp_gnuradio4_cmake_dir)
    cmake_path(GET _gr4cp_gnuradio4_cmake_dir PARENT_PATH GR4CP_GNURADIO4_LIB_DIR)
    message(STATUS "Building with GNU Radio 4-backed block catalog from ${GR4CP_GNURADIO4_LIB_DIR}")
else()
    message(WARNING "GR4CP_ENABLE_GR4_CATALOG=OFF disables the production server catalog path; tests may still use local fake providers.")
endif()

add_library(gr4cp_core
    src/app/block_settings_service.cpp
    src/app/block_catalog_service.cpp
    src/app/scheduler_catalog_service.cpp
    src/app/session_stream_service.cpp
    src/catalog/static_block_catalog_provider.cpp
    src/domain/block_catalog.cpp
    src/domain/scheduler_catalog.cpp
    src/domain/session.cpp
    src/domain/session_stream.cpp
    src/app/session_service.cpp
    src/runtime/gr4_runtime_manager.cpp
    src/runtime/stream_binding_allocator.cpp
    src/runtime/stub_runtime_manager.cpp
    src/storage/in_memory_session_repository.cpp
)

if(NOT EMSCRIPTEN)
    # cpp-httplib and Boost.Beast both need real sockets, so the HTTP transport is native-only.
    target_sources(gr4cp_core PRIVATE src/api/http_server.cpp)
else()
    # WASM talks to gnuradio4-studio in-process through this dispatcher instead.
    target_sources(gr4cp_core PRIVATE src/api/wasm_api.cpp)
endif()

if(GR4CP_HAVE_GNURADIO4)
    target_sources(gr4cp_core
        PRIVATE
            src/catalog/gr4_block_catalog_provider.cpp
            src/catalog/gr4_scheduler_catalog_provider.cpp
    )
    if(GR4CP_PLUGIN_DIRECTORIES)
        set(_gr4cp_plugin_dirs "${GR4CP_PLUGIN_DIRECTORIES}")
    else()
        set(_gr4cp_plugin_dirs "${GR4CP_GNURADIO4_LIB_DIR}/gnuradio-4/plugins:${GR4CP_GNURADIO4_LIB_DIR}")
    endif()
    if(GR4CP_EXTRA_PLUGIN_DIRECTORIES)
        set(_gr4cp_plugin_dirs "${_gr4cp_plugin_dirs}:${GR4CP_EXTRA_PLUGIN_DIRECTORIES}")
    endif()
    target_compile_definitions(gr4cp_core
        PUBLIC
            GR4CP_HAVE_GNURADIO4
            GR4CP_DEFAULT_GNURADIO4_PLUGIN_DIR="${_gr4cp_plugin_dirs}"
    )
    set(GR4CP_GNURADIO4_TARGETS
        gnuradio4::gnuradio-core
        gnuradio4::gnuradio-blocklib-core
    )
    if(TARGET gnuradio4::gnuradio-plugin)
        list(APPEND GR4CP_GNURADIO4_TARGETS gnuradio4::gnuradio-plugin)
    endif()
    target_link_libraries(gr4cp_core
        PUBLIC
            ${GR4CP_GNURADIO4_TARGETS}
    )
    set(_gr4cp_test_env
        "GNURADIO4_PLUGIN_DIRECTORIES=${_gr4cp_plugin_dirs}\;LD_LIBRARY_PATH=$<TARGET_FILE_DIR:gr4cp_core>:${GR4CP_GNURADIO4_LIB_DIR}"
    )
endif()

target_include_directories(gr4cp_core
    PUBLIC
        include
)

target_link_libraries(gr4cp_core
    PUBLIC
        nlohmann_json::nlohmann_json
)

if(NOT EMSCRIPTEN)
    target_link_libraries(gr4cp_core
        PUBLIC
            Boost::headers
            httplib::httplib
    )
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_link_libraries(gr4cp_core
        PUBLIC
            stdc++exp
    )
endif()

if(NOT EMSCRIPTEN)
    add_library(gr4cp_cli
        src/cli/cli.cpp
    )

    target_include_directories(gr4cp_cli
        PUBLIC
            include
    )

    target_link_libraries(gr4cp_cli
        PUBLIC
            httplib::httplib
            nlohmann_json::nlohmann_json
    )

    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_link_libraries(gr4cp_cli
            PUBLIC
                stdc++exp
        )
    endif()
endif()

if(NOT EMSCRIPTEN)
    add_executable(gr4cp_server
        src/main.cpp
    )

    target_link_libraries(gr4cp_server
        PRIVATE
            gr4cp_core
    )
else()
    # gnuradio4-studio loads this module and calls Module.handleRequest() directly; there is no
    # listening socket, so src/main.cpp (which drives HttpServer) has no WASM counterpart.
    add_executable(gr4cp_wasm
        src/wasm_main.cpp
    )
    set(GR4CP_WASM_BLOCK_LIBRARIES "GrFourierBlocks;GrHttpBlocks;Gr4StudioBlocks;GrBasicBlocks" CACHE STRING
        "gnuradio4 block libraries statically linked into the WASM module")

    set(GR4CP_WASM_BLOCK_INCLUDES "")
    set(GR4CP_WASM_BLOCK_CALLS "")
    foreach(_gr4cp_block_library IN LISTS GR4CP_WASM_BLOCK_LIBRARIES)
        set(_gr4cp_block_archive
            "${GR4CP_GNURADIO4_LIB_DIR}/gnuradio-4/plugins/${CMAKE_STATIC_LIBRARY_PREFIX}${_gr4cp_block_library}Static${CMAKE_STATIC_LIBRARY_SUFFIX}")
        if(NOT EXISTS "${_gr4cp_block_archive}")
            message(FATAL_ERROR
                "GR4CP_WASM_BLOCK_LIBRARIES names ${_gr4cp_block_library}, but ${_gr4cp_block_archive} "
                "does not exist. Build that block library with MAKE_STATIC_LIBRARY and install it.")
        endif()
        target_link_libraries(gr4cp_wasm PRIVATE "${_gr4cp_block_archive}")
        string(APPEND GR4CP_WASM_BLOCK_INCLUDES
            "#include <gnuradio-4.0/${_gr4cp_block_library}.hpp>\n")
        string(APPEND GR4CP_WASM_BLOCK_CALLS
            "    (void)gr::blocklib::init${_gr4cp_block_library}(gr::globalBlockRegistry());\n")
    endforeach()

    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/wasm_block_registration.hpp.in"
        "${CMAKE_CURRENT_BINARY_DIR}/generated/gr4cp/wasm_block_registration.hpp"
        @ONLY
    )
    target_include_directories(gr4cp_wasm PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")

    target_link_libraries(gr4cp_wasm
        PRIVATE
            gr4cp_core
            embind
    )

    target_link_options(gr4cp_wasm
        PRIVATE
            "-sMODULARIZE=1"
            "-sEXPORT_ES6=1"
            "-sEXPORT_NAME=createGr4cpModule"
    )

    set_target_properties(gr4cp_wasm PROPERTIES SUFFIX ".mjs")
endif()

if(NOT EMSCRIPTEN)
    add_executable(gr4cp_cli_exec
        src/cli_main.cpp
    )

    set_target_properties(gr4cp_cli_exec PROPERTIES OUTPUT_NAME gr4cp-cli)

    if(APPLE)
        set(_gr4cp_executable_install_rpath "@loader_path/../${CMAKE_INSTALL_LIBDIR}")
        set(_gr4cp_library_install_rpath "@loader_path")
    elseif(UNIX)
        set(_gr4cp_executable_install_rpath "\$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")
        set(_gr4cp_library_install_rpath "\$ORIGIN")
    endif()

    if(DEFINED _gr4cp_executable_install_rpath)
        set_target_properties(gr4cp_server gr4cp_cli_exec PROPERTIES
            INSTALL_RPATH "${_gr4cp_executable_install_rpath}"
        )
        set_target_properties(gr4cp_core gr4cp_cli PROPERTIES
            INSTALL_RPATH "${_gr4cp_library_install_rpath}"
        )
    endif()

    target_link_libraries(gr4cp_cli_exec
        PRIVATE
            gr4cp_cli
    )
endif()

if(ENABLE_TESTING)
if(NOT EMSCRIPTEN)
    find_package(GTest CONFIG REQUIRED)
endif()

enable_testing()

if(NOT EMSCRIPTEN)
    add_executable(gr4cp_smoke_test
        test/healthz_smoke_test.cpp
    )

    target_link_libraries(gr4cp_smoke_test
        PRIVATE
            gr4cp_core
            GTest::gtest_main
            Threads::Threads
    )

    include(GoogleTest)
    if(GR4CP_HAVE_GNURADIO4)
        gtest_discover_tests(gr4cp_smoke_test PROPERTIES ENVIRONMENT "${_gr4cp_test_env}")
    else()
        gtest_discover_tests(gr4cp_smoke_test)
    endif()


    add_executable(gr4cp_core_test
        test/block_settings_service_test.cpp
        test/session_service_test.cpp
        test/session_stream_service_test.cpp
        test/block_catalog_service_test.cpp
        test/gr4_block_catalog_provider_test.cpp
        test/gr4_runtime_manager_test.cpp
    )

    target_link_libraries(gr4cp_core_test
        PRIVATE
            gr4cp_core
            GTest::gtest_main
            Threads::Threads
    )

    if(GR4CP_HAVE_GNURADIO4)
        gtest_discover_tests(gr4cp_core_test PROPERTIES ENVIRONMENT "${_gr4cp_test_env}")
    else()
        gtest_discover_tests(gr4cp_core_test)
    endif()

    add_executable(gr4cp_http_test
        test/http_api_test.cpp
    )


    target_link_libraries(gr4cp_http_test
        PRIVATE
            gr4cp_core
            GTest::gtest_main
            Threads::Threads
    )

    if(GR4CP_HAVE_GNURADIO4)
        gtest_discover_tests(gr4cp_http_test PROPERTIES ENVIRONMENT "${_gr4cp_test_env}")
    else()
        gtest_discover_tests(gr4cp_http_test)
    endif()

    add_executable(gr4cp_cli_test
        test/cli_test.cpp
    )

    target_link_libraries(gr4cp_cli_test
        PRIVATE
            gr4cp_cli
            gr4cp_core
            GTest::gtest_main
            Threads::Threads
    )

    target_compile_definitions(gr4cp_cli_test
        PRIVATE
            GR4CP_CLI_EXECUTABLE="$<TARGET_FILE:gr4cp_cli_exec>"
    )

    if(GR4CP_HAVE_GNURADIO4)
        gtest_discover_tests(gr4cp_cli_test PROPERTIES ENVIRONMENT "${_gr4cp_test_env}")
    else()
        gtest_discover_tests(gr4cp_cli_test)
    endif()

endif()
endif()

if(NOT EMSCRIPTEN)
    install(TARGETS gr4cp_core gr4cp_cli gr4cp_server gr4cp_cli_exec
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    )
endif()

install(DIRECTORY include/gr4cp/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/gr4cp
    FILES_MATCHING PATTERN "*.hpp"
)
