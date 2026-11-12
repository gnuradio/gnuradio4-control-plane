cmake_minimum_required(VERSION 3.25)

project(gr4_control_plane_mvp VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(GNUInstallDirs)

option(USE_CCACHE "Use ccache if available" ON)
option(ENABLE_TESTING "Build control-plane tests" ON)

option(GR4CP_ENABLE_GR4_CATALOG "Enable the required GNU Radio 4-backed block catalog provider" ON)

set(GR4CP_EXTRA_PLUGIN_DIRECTORIES "" CACHE STRING "Colon-separated plugin directories searched last")
set(GR4CP_PLUGIN_DIRECTORIES "" CACHE STRING "Colon-separated plugin directories replacing the installation's plugin directory")

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
    src/api/session_json.cpp
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
    src/runtime/plugin_loader.cpp
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
    target_compile_definitions(gr4cp_core
        PUBLIC
            GR4CP_HAVE_GNURADIO4
        PRIVATE
            GR4CP_PLUGIN_DIRECTORIES="${GR4CP_PLUGIN_DIRECTORIES}"
            GR4CP_EXTRA_PLUGIN_DIRECTORIES="${GR4CP_EXTRA_PLUGIN_DIRECTORIES}"
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

if(WIN32)
    # the stream binding allocator calls Winsock itself rather than through cpp-httplib
    target_link_libraries(gr4cp_core
        PRIVATE
            ws2_32
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

    add_dependencies(gr4cp_smoke_test gr4cp_server)

    include(GoogleTest)
    gtest_discover_tests(gr4cp_smoke_test
        PROPERTIES
            ENVIRONMENT "GR4CP_SERVER_EXECUTABLE=$<TARGET_FILE:gr4cp_server>"
    )

    add_library(gr4cp_broken_plugin MODULE
        test/broken_plugin.cpp
    )


    add_executable(gr4cp_core_test
        test/block_settings_service_test.cpp
        test/session_service_test.cpp
        test/session_stream_service_test.cpp
        test/block_catalog_service_test.cpp
        test/gr4_block_catalog_provider_test.cpp
        test/final_settings_test.cpp
        test/gr4_runtime_manager_test.cpp
    )

    target_link_libraries(gr4cp_core_test
        PRIVATE
            gr4cp_core
            GTest::gtest_main
            Threads::Threads
    )

    target_compile_definitions(gr4cp_core_test
        PRIVATE
            GR4CP_PLUGIN_DIRECTORIES="${GR4CP_PLUGIN_DIRECTORIES}"
    )
    add_dependencies(gr4cp_core_test gr4cp_broken_plugin)

    gtest_discover_tests(gr4cp_core_test
        PROPERTIES
            ENVIRONMENT_MODIFICATION "GR4CP_BROKEN_PLUGIN_LIBRARY=set:$<TARGET_FILE:gr4cp_broken_plugin>"
            ENVIRONMENT_MODIFICATION "GR4CP_CORE_LIBRARY=set:$<TARGET_FILE:gnuradio4::gnuradio-shared-state>"
    )

    add_executable(gr4cp_http_test
        test/http_api_test.cpp
    )


    target_link_libraries(gr4cp_http_test
        PRIVATE
            gr4cp_core
            GTest::gtest_main
            Threads::Threads
    )

    gtest_discover_tests(gr4cp_http_test)

    # Exercise the in-process dispatcher on native builds without an Emscripten SDK.
    add_executable(gr4cp_wasm_api_test
        test/wasm_api_test.cpp
        src/api/wasm_api.cpp
    )

    target_link_libraries(gr4cp_wasm_api_test
        PRIVATE
            gr4cp_core
            GTest::gtest_main
            Threads::Threads
    )
    gtest_discover_tests(gr4cp_wasm_api_test)

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

    gtest_discover_tests(gr4cp_cli_test
        PROPERTIES
            ENVIRONMENT "GR4CP_CLI_EXECUTABLE=$<TARGET_FILE:gr4cp_cli_exec>"
    )

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
