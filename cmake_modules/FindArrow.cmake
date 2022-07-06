# MIT License
#
# Copyright (c) 2022 Jianshen Liu
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in all
# copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.

if(NOT BITAR_BUILD_ARROW)
  # Since the Parquet library depends on the Arrow library, finding the Parquet
  # library will import the Arrow library as well.
  #
  # find_package() for system-installed Parquet library should fail because of a
  # known bug about the misconfigured cmake directory structure:
  # https://issues.apache.org/jira/browse/ARROW-12175. Currently, to temporarily
  # work around the issue we can pass a cmake variable `Parquet_ROOT` to
  # manually specify the directory that containing the `ParquetConfig.cmake`
  # file.
  #
  # Internally, `ParquetConfig.cmake` uses `find_dependency(Arrow)` to load the
  # Arrow-provided `FindArrow.cmake` file. With the existing of the current file
  # that has the same name in the CMAKE_MODULE_PATH, the Arrow dependency lookup
  # will become an infinite recursion. Therefore, we need to temporarily remove
  # the current file from the module search path.
  list(REMOVE_ITEM CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}")
  find_package(Parquet QUIET CONFIG)
  list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}")
endif()

if(${CMAKE_FIND_PACKAGE_NAME}_FOUND AND Parquet_FOUND)
  execute_process(
    COMMAND
      sh -c
      "ld --verbose | grep SEARCH_DIR | sed 's/\\([[:space:]]*SEARCH_DIR(\"=\\|\")\\)//g'"
    OUTPUT_VARIABLE ld_search_dirs
    OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL LAST)

  foreach(ld_search_dir ${ld_search_dirs})
    if(${CMAKE_FIND_PACKAGE_NAME}_CONFIG MATCHES "^${ld_search_dir}")
      set(${CMAKE_FIND_PACKAGE_NAME}_IS_SYSTEM_INSTALLED True)
      break()
    endif()
  endforeach()

  mark_as_advanced(
    ${CMAKE_FIND_PACKAGE_NAME}_CONFIG ${CMAKE_FIND_PACKAGE_NAME}_DIR
    ${CMAKE_FIND_PACKAGE_NAME}_VERSION Parquet_CONFIG Parquet_DIR
    Parquet_VERSION)
  list(APPEND required_vars ${${CMAKE_FIND_PACKAGE_NAME}_CONFIG}
       ${${CMAKE_FIND_PACKAGE_NAME}_DIR} ${Parquet_CONFIG} ${Parquet_DIR})
  list(APPEND version_var ${${CMAKE_FIND_PACKAGE_NAME}_VERSION})
else()
  FetchContent_GetProperties(${CMAKE_FIND_PACKAGE_NAME})
  string(TOLOWER ${CMAKE_FIND_PACKAGE_NAME} _find_package_name_lower)

  if(${_find_package_name_lower}_POPULATED)
    # ${${_find_package_name_lower}_POPULATED} becomes 1 instead of True when it
    # is populated from FetchContent_GetProperties()
    set(${_find_package_name_lower}_POPULATED True)
  else()
    set(BITAR_ARROW_GIT_REPOSITORY
        "https://github.com/apache/arrow.git"
        CACHE
          STRING
          "Use the Arrow library from the git repository for building when needed"
    )
    set(BITAR_ARROW_GIT_TAG
        "8abb941f57316d77b4a5eb209f5a108c275fe120"
        CACHE
          STRING
          "Use the source at the git branch, tag or commit hash of the Arrow repository for building when needed"
    )
    option(BITAR_INSTALL_ARROW
           "Install the Arrow library as part of the cmake installation process"
           OFF)

    if("${BITAR_ARROW_GIT_TAG}" STREQUAL "")
      # Get the list of tags of a remote repository without cloning
      # https://stackoverflow.com/a/12704727
      string(
        CONCAT
          extract_arrow_latest_release_tag
          "git ls-remote --tags --sort='v:refname' ${BITAR_ARROW_GIT_REPOSITORY}"
          " | grep --only-matching 'apache-arrow-[[:digit:]]\\+\\.[[:digit:]]\\+\\.[[:digit:]]\\+$'"
          " | tail --lines=1")
      execute_process(
        COMMAND sh -c "${extract_arrow_latest_release_tag}"
        OUTPUT_VARIABLE BITAR_ARROW_GIT_TAG
        OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL LAST)
    endif()

    message(
      STATUS
        "Could NOT find ${CMAKE_FIND_PACKAGE_NAME} and/or Parquet library. "
        "Configuring to build from source (version \"${BITAR_ARROW_GIT_TAG}\")..."
    )

    # Discard our environment settings to allow external projects to define with
    # compatible values
    set(_backup_CMAKE_CXX_STANDARD ${CMAKE_CXX_STANDARD})
    set(_backup_CMAKE_C_STANDARD ${CMAKE_C_STANDARD})
    set(_backup_CMAKE_CXX_CPPCHECK "${CMAKE_CXX_CPPCHECK}")
    set(_backup_CMAKE_C_CPPCHECK "${CMAKE_C_CPPCHECK}")
    set(_backup_CMAKE_CXX_CLANG_TIDY "${CMAKE_CXX_CLANG_TIDY}")
    set(_backup_CMAKE_C_CLANG_TIDY "${CMAKE_C_CLANG_TIDY}")
    set(_backup_CMAKE_CXX_INCLUDE_WHAT_YOU_USE
        "${CMAKE_CXX_INCLUDE_WHAT_YOU_USE}")
    unset(CMAKE_CXX_STANDARD)
    unset(CMAKE_C_STANDARD)
    unset(CMAKE_CXX_CPPCHECK)
    unset(CMAKE_C_CPPCHECK)
    unset(CMAKE_CXX_CLANG_TIDY)
    unset(CMAKE_C_CLANG_TIDY)
    unset(CMAKE_CXX_INCLUDE_WHAT_YOU_USE)

    # Define Arrow minimal build options
    set(ARROW_DEFINE_OPTIONS
        ON
        CACHE INTERNAL "Define Arrow options")
    set(ARROW_BUILD_STATIC
        ON
        CACHE INTERNAL "Build static Arrow libraries")
    set(ARROW_BUILD_SHARED
        OFF
        CACHE INTERNAL "Build shared libraries")
    set(ARROW_JEMALLOC
        OFF
        CACHE INTERNAL "Build the Arrow jemalloc-based allocator")
    set(ARROW_PARQUET
        ON
        CACHE INTERNAL "Build the Parquet libraries")
    set(ARROW_DEPENDENCY_USE_SHARED
        OFF
        CACHE INTERNAL "Link to shared libraries")
    set(ARROW_WITH_UTF8PROC
        OFF
        CACHE
          INTERNAL
          "Enable Arrow to support for Unicode properties using the utf8proc library"
    )
    set(ARROW_WITH_RE2
        OFF
        CACHE
          INTERNAL
          "Enable Arrow to support for regular expressions using the re2 library"
    )

    set(_arg_SOURCE_SUBDIR cpp)
    FetchContent_Declare(
      ${CMAKE_FIND_PACKAGE_NAME}
      GIT_REPOSITORY "${BITAR_ARROW_GIT_REPOSITORY}"
      GIT_TAG "${BITAR_ARROW_GIT_TAG}"
      SOURCE_SUBDIR "${_arg_SOURCE_SUBDIR}" OVERRIDE_FIND_PACKAGE)

    FetchContent_Populate(${CMAKE_FIND_PACKAGE_NAME})

    if(EXISTS
       "${${_find_package_name_lower}_SOURCE_DIR}/${_arg_SOURCE_SUBDIR}/CMakeLists.txt"
    )
      if(BITAR_INSTALL_ARROW)
        add_subdirectory(
          "${${_find_package_name_lower}_SOURCE_DIR}/${_arg_SOURCE_SUBDIR}"
          "${${_find_package_name_lower}_BINARY_DIR}")
      else()
        add_subdirectory(
          "${${_find_package_name_lower}_SOURCE_DIR}/${_arg_SOURCE_SUBDIR}"
          "${${_find_package_name_lower}_BINARY_DIR}" EXCLUDE_FROM_ALL)
      endif()
    endif()

    unset(_arg_SOURCE_SUBDIR)

    # Restore our environment settings
    set(CMAKE_CXX_STANDARD ${_backup_CMAKE_CXX_STANDARD})
    set(CMAKE_C_STANDARD ${_backup_CMAKE_C_STANDARD})
    set(CMAKE_CXX_CPPCHECK "${_backup_CMAKE_CXX_CPPCHECK}")
    set(CMAKE_C_CPPCHECK "${_backup_CMAKE_C_CPPCHECK}")
    set(CMAKE_CXX_CLANG_TIDY "${_backup_CMAKE_CXX_CLANG_TIDY}")
    set(CMAKE_C_CLANG_TIDY "${_backup_CMAKE_C_CLANG_TIDY}")
    set(CMAKE_CXX_INCLUDE_WHAT_YOU_USE
        "${_backup_CMAKE_CXX_INCLUDE_WHAT_YOU_USE}")
    unset(_backup_CMAKE_CXX_STANDARD)
    unset(_backup_CMAKE_C_STANDARD)
    unset(_backup_CMAKE_CXX_CPPCHECK)
    unset(_backup_CMAKE_C_CPPCHECK)
    unset(_backup_CMAKE_CXX_CLANG_TIDY)
    unset(_backup_CMAKE_C_CLANG_TIDY)
    unset(_backup_CMAKE_CXX_INCLUDE_WHAT_YOU_USE)
  endif()

  set(required_vars ${${_find_package_name_lower}_POPULATED})
  set(version_var ${BITAR_ARROW_GIT_TAG})
  unset(${_find_package_name_lower}_POPULATED)
  unset(_find_package_name_lower)
endif()

find_package_handle_standard_args(
  ${CMAKE_FIND_PACKAGE_NAME}
  REQUIRED_VARS
  required_vars
  VERSION_VAR version_var)
unset(required_vars)
unset(version_var)

if(${CMAKE_FIND_PACKAGE_NAME}_FOUND)
  if(TARGET arrow_static)
    set(_arrow_library arrow_static)
  elseif(TARGET arrow_shared)
    set(_arrow_library arrow_shared)
  endif()

  if(TARGET parquet_static)
    set(_parquet_library parquet_static)
  elseif(TARGET parquet_shared)
    set(_parquet_library parquet_shared)
  endif()

  add_library(Arrow::arrow INTERFACE IMPORTED)
  add_library(Arrow::parquet INTERFACE IMPORTED)
  target_link_libraries(Arrow::arrow INTERFACE ${_arrow_library})
  target_link_libraries(Arrow::parquet INTERFACE ${_parquet_library})

  get_target_property(_arrow_include_dirs ${_arrow_library} INCLUDE_DIRECTORIES)
  get_target_property(_parquet_include_dirs ${_parquet_library}
                      INCLUDE_DIRECTORIES)

  # This is the case where Arrow is built from source
  if(_arrow_include_dirs)
    set_target_properties(Arrow::arrow PROPERTIES INTERFACE_INCLUDE_DIRECTORIES
                                                  "${_arrow_include_dirs}")
    set_target_properties(
      Arrow::parquet PROPERTIES INTERFACE_INCLUDE_DIRECTORIES
                                "${_parquet_include_dirs}")
    unset(_arrow_include_dirs)
    unset(_parquet_include_dirs)
  endif()

  unset(_arrow_library)
  unset(_parquet_library)
endif()
