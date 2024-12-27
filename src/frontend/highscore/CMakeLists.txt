include(CMakeDependentOption)
include(GNUInstallDirs)
include(FixInterfaceIncludes)

set(SOURCES_HIGHSCORE
    melonds-highscore.cpp
    Platform.cpp

    ../glad/glad.c
    ../mic_blow.h
)

find_package(PkgConfig REQUIRED)

cmake_policy(SET CMP0070 NEW)

execute_process(COMMAND git describe --always --tags
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    OUTPUT_VARIABLE GIT_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE)

if("${GIT_VERSION}" STREQUAL "")
    set(HS_VERSION "${CMAKE_PROJECT_VERSION}")
else()
    set(HS_VERSION "${GIT_VERSION}")
endif()

pkg_check_modules(highscore-1 REQUIRED IMPORTED_TARGET highscore-1)
pkg_check_modules(gobject-2.0 REQUIRED IMPORTED_TARGET gobject-2.0)

fix_interface_includes(PkgConfig::highscore-1)

add_library(melonds-highscore MODULE ${SOURCES_HIGHSCORE})

set_target_properties(melonds-highscore PROPERTIES PREFIX "")
target_include_directories(melonds-highscore PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_include_directories(melonds-highscore PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_include_directories(melonds-highscore PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../..")
target_link_libraries(melonds-highscore PRIVATE core)
target_link_libraries(melonds-highscore PRIVATE PkgConfig::highscore-1 PkgConfig::gobject-2.0)
target_link_libraries(melonds-highscore PRIVATE ${CMAKE_DL_LIBS})

set(MELONDS_GL_HEADER \"frontend/glad/glad.h\" CACHE STRING "Path to a header that contains OpenGL function and type declarations.")

target_compile_definitions(melonds-highscore PUBLIC MELONDS_GL_HEADER=${MELONDS_GL_HEADER})
target_compile_definitions(core PUBLIC MELONDS_GL_HEADER=${MELONDS_GL_HEADER})

file(READ melonds.highscore.in TEXT)
string(REPLACE "@version@" "${HS_VERSION}" TEXT "${TEXT}")
file(GENERATE OUTPUT melonds.highscore CONTENT "${TEXT}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS melonds.highscore.in)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/melonds.highscore DESTINATION ${CMAKE_INSTALL_LIBDIR}/highscore/cores)
install(TARGETS melonds-highscore LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/highscore/cores)
