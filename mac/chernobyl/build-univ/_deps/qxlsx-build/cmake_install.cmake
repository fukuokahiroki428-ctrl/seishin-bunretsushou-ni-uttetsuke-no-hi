# Install script for directory: /Users/shio/chernobyl-app/mac/chernobyl/build-univ/_deps/qxlsx-src/QXlsx

# Set the install prefix
if(NOT DEFINED CMAKE_INSTALL_PREFIX)
  set(CMAKE_INSTALL_PREFIX "/usr/local")
endif()
string(REGEX REPLACE "/$" "" CMAKE_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")

# Set the install configuration name.
if(NOT DEFINED CMAKE_INSTALL_CONFIG_NAME)
  if(BUILD_TYPE)
    string(REGEX REPLACE "^[^A-Za-z0-9_]+" ""
           CMAKE_INSTALL_CONFIG_NAME "${BUILD_TYPE}")
  else()
    set(CMAKE_INSTALL_CONFIG_NAME "")
  endif()
  message(STATUS "Install configuration: \"${CMAKE_INSTALL_CONFIG_NAME}\"")
endif()

# Set the component getting installed.
if(NOT CMAKE_INSTALL_COMPONENT)
  if(COMPONENT)
    message(STATUS "Install component: \"${COMPONENT}\"")
    set(CMAKE_INSTALL_COMPONENT "${COMPONENT}")
  else()
    set(CMAKE_INSTALL_COMPONENT)
  endif()
endif()

# Is this installation the result of a crosscompile?
if(NOT DEFINED CMAKE_CROSSCOMPILING)
  set(CMAKE_CROSSCOMPILING "FALSE")
endif()

# Set path to fallback-tool for dependency-resolution.
if(NOT DEFINED CMAKE_OBJDUMP)
  set(CMAKE_OBJDUMP "/usr/bin/objdump")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "devel" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib" TYPE STATIC_LIBRARY FILES "/Users/shio/chernobyl-app/mac/chernobyl/build-univ/_deps/qxlsx-build/libQXlsxQt6.a")
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/libQXlsxQt6.a" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/libQXlsxQt6.a")
    execute_process(COMMAND "/usr/bin/ranlib" "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/libQXlsxQt6.a")
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "devel" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/QXlsxQt6" TYPE FILE FILES
    "/Users/shio/chernobyl-app/mac/chernobyl/build-univ/_deps/qxlsx-src/QXlsx/header/xlsxabstractooxmlfile.h"
    "/Users/shio/chernobyl-app/mac/chernobyl/build-univ/_deps/qxlsx-src/QXlsx/header/xlsxabstractsheet.h"
    "/Users/shio/chernobyl-app/mac/chernobyl/build-univ/_deps/qxlsx-src/QXlsx/header/xlsxabstractsheet_p.h"
    "/Users/shio/chernobyl-app/mac/chernobyl/build-univ/_deps/qxlsx-src/QXlsx/header/xlsxcellformula.h"
    "/Users/shio/chernobyl-app/mac/chernobyl/build-univ/_deps/qxlsx-src/QXlsx/header/xlsxcell.h"
    "/Users/shio/chernobyl-app/mac/chernobyl/build-univ/_deps/qxlsx-src/QXlsx/header/xlsxcelllocation.h"
    "/Users/shio/chernobyl-app/mac/chernobyl/build-univ/_deps/qxlsx-src/QXlsx/header/xlsxcellrange.h"
    "/Users/shio/chernobyl-app/mac/chernobyl/build-univ/_deps/qxlsx-src/QXlsx/header/xlsxcellreference.h"
    "/Users/shio/chernobyl-app/mac/chernobyl/build-univ/_deps/qxlsx-src/QXlsx/header/xlsxchart.h"
    "/Users/shio/chernobyl-app/mac/chernobyl/build-univ/_deps/qxlsx-src/QXlsx/header/xlsxchartsheet.h"
    "/Users/shio/chernobyl-app/mac/chernobyl/build-univ/_deps/qxlsx-src/QXlsx/header/xlsxconditionalformatting.h"
    "/Users/shio/chernobyl-app/mac/chernobyl/build-univ/_deps/qxlsx-src/QXlsx/header/xlsxdatavalidation.h"
    "/Users/shio/chernobyl-app/mac/chernobyl/build-univ/_deps/qxlsx-src/QXlsx/header/xlsxdatetype.h"
    "/Users/shio/chernobyl-app/mac/chernobyl/build-univ/_deps/qxlsx-src/QXlsx/header/xlsxdocument.h"
    "/Users/shio/chernobyl-app/mac/chernobyl/build-univ/_deps/qxlsx-src/QXlsx/header/xlsxformat.h"
    "/Users/shio/chernobyl-app/mac/chernobyl/build-univ/_deps/qxlsx-src/QXlsx/header/xlsxglobal.h"
    "/Users/shio/chernobyl-app/mac/chernobyl/build-univ/_deps/qxlsx-src/QXlsx/header/xlsxrichstring.h"
    "/Users/shio/chernobyl-app/mac/chernobyl/build-univ/_deps/qxlsx-src/QXlsx/header/xlsxworkbook.h"
    "/Users/shio/chernobyl-app/mac/chernobyl/build-univ/_deps/qxlsx-src/QXlsx/header/xlsxworksheet.h"
    )
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  include("/Users/shio/chernobyl-app/mac/chernobyl/build-univ/_deps/qxlsx-build/CMakeFiles/QXlsx.dir/install-cxx-module-bmi-noconfig.cmake" OPTIONAL)
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "devel" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/QXlsxQt6/QXlsxQt6Targets.cmake")
    file(DIFFERENT _cmake_export_file_changed FILES
         "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/QXlsxQt6/QXlsxQt6Targets.cmake"
         "/Users/shio/chernobyl-app/mac/chernobyl/build-univ/_deps/qxlsx-build/CMakeFiles/Export/5e1a71f991ec0867fe453527b0963803/QXlsxQt6Targets.cmake")
    if(_cmake_export_file_changed)
      file(GLOB _cmake_old_config_files "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/QXlsxQt6/QXlsxQt6Targets-*.cmake")
      if(_cmake_old_config_files)
        string(REPLACE ";" ", " _cmake_old_config_files_text "${_cmake_old_config_files}")
        message(STATUS "Old export file \"$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/QXlsxQt6/QXlsxQt6Targets.cmake\" will be replaced.  Removing files [${_cmake_old_config_files_text}].")
        unset(_cmake_old_config_files_text)
        file(REMOVE ${_cmake_old_config_files})
      endif()
      unset(_cmake_old_config_files)
    endif()
    unset(_cmake_export_file_changed)
  endif()
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/QXlsxQt6" TYPE FILE FILES "/Users/shio/chernobyl-app/mac/chernobyl/build-univ/_deps/qxlsx-build/CMakeFiles/Export/5e1a71f991ec0867fe453527b0963803/QXlsxQt6Targets.cmake")
  if(CMAKE_INSTALL_CONFIG_NAME MATCHES "^()$")
    file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/QXlsxQt6" TYPE FILE FILES "/Users/shio/chernobyl-app/mac/chernobyl/build-univ/_deps/qxlsx-build/CMakeFiles/Export/5e1a71f991ec0867fe453527b0963803/QXlsxQt6Targets-noconfig.cmake")
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/QXlsxQt6" TYPE FILE FILES
    "/Users/shio/chernobyl-app/mac/chernobyl/build-univ/_deps/qxlsx-build/QXlsxQt6Config.cmake"
    "/Users/shio/chernobyl-app/mac/chernobyl/build-univ/_deps/qxlsx-build/QXlsxQt6ConfigVersion.cmake"
    )
endif()

string(REPLACE ";" "\n" CMAKE_INSTALL_MANIFEST_CONTENT
       "${CMAKE_INSTALL_MANIFEST_FILES}")
if(CMAKE_INSTALL_LOCAL_ONLY)
  file(WRITE "/Users/shio/chernobyl-app/mac/chernobyl/build-univ/_deps/qxlsx-build/install_local_manifest.txt"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
endif()
