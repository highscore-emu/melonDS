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

install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/melonds.highscore DESTINATION ${CMAKE_INSTALL_LIBDIR}/highscore/cores)
install(TARGETS melonds-highscore LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/highscore/cores)
