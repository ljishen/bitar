# MIT License
#
# Copyright (c) 2022 University of California
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
#
# ~~~
# This config sets the following target in your project::
#   Arrow::arrow - for linked as static or shared library
#
# and, if parquet is built, the following target is also set:
#   Arrow::parquet - for linked as static or shared library
# ~~~

if(NOT BITAR_BUILD_ARROW)
  # Temporarily remove the path to the .pc files installed by vcpkg to allow
  # looking for system-installed libraries only when we do not intend to build
  # Arrow. Arrow internally uses the cmake pkg-config module for finding its
  # dependencies, e.g., in `FindThrift.cmake`.
  set(_backup_PKG_CONFIG_PATH "$ENV{PKG_CONFIG_PATH}")
  set(ENV{PKG_CONFIG_PATH} "${old_PKG_CONFIG_PATH}")
  set(PKG_CONFIG_USE_CMAKE_PREFIX_PATH FALSE)

  if(NOT Arrow_ROOT)
    # The default system-installed Arrow library is installed at /usr
    set(Arrow_ROOT "/usr")
  endif()

  if(ARROW_PARQUET)
    # find_package() for system-installed Parquet library should fail because of
    # a known bug about the misconfigured cmake directory structure:
    # https://issues.apache.org/jira/browse/ARROW-12175. Currently, to
    # temporarily work around this issue we need to set the variable
    # `Parquet_ROOT` to manually specify the directory list that may contain the
    # `ParquetConfig.cmake` file.
    set(Parquet_ROOT
        "${Arrow_ROOT}/lib/${CMAKE_HOST_SYSTEM_PROCESSOR}-linux-gnu/cmake/arrow;${Arrow_ROOT}/lib64/cmake/arrow"
    )

    # Since the Parquet library depends on the Arrow library, finding the
    # Parquet library will import the Arrow library as well.
    #
    # Internally, `ParquetConfig.cmake` uses `find_dependency(Arrow)` to load
    # the Arrow-provided `FindArrow.cmake` file. With the existing of the
    # current file that has the same name in the CMAKE_MODULE_PATH, the lookup
    # of the Arrow dependency becomes an infinite recursion. Therefore, we need
    # to temporarily remove the current file from the module search path.
    # https://github.com/apache/arrow/blob/apache-arrow-9.0.0/cpp/src/parquet/ParquetConfig.cmake.in
    list(REMOVE_ITEM CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}")
    find_package(Parquet QUIET CONFIG)
    list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}")
  else()
    # Use QUIET to silence the checking of possible dependencies by
    # ArrowConfig.cmake. This is not a problem because if possible dependencies
    # are not found, the installed arrow_bundled_dependencies.a will be used.
    find_package(${CMAKE_FIND_PACKAGE_NAME} QUIET CONFIG)

    # Loading the `ArrowConfig.cmake` file will automatically set this to `ON`
    # even though the system may not have the parquet library installed. To be
    # consistent with our configuration, we need to keep this to be `OFF` here.
    # https://github.com/apache/arrow/blob/apache-arrow-9.0.0/cpp/src/arrow/ArrowConfig.cmake.in
    set(ARROW_PARQUET
        OFF
        CACHE INTERNAL "Build the Parquet libraries")
  endif()

  set(PKG_CONFIG_USE_CMAKE_PREFIX_PATH TRUE)
  set(ENV{PKG_CONFIG_PATH} "${_backup_PKG_CONFIG_PATH}")
  unset(_backup_PKG_CONFIG_PATH)
endif()

if(${CMAKE_FIND_PACKAGE_NAME}_FOUND AND (NOT ARROW_PARQUET OR Parquet_FOUND))
  mark_as_advanced(
    ${CMAKE_FIND_PACKAGE_NAME}_CONFIG ${CMAKE_FIND_PACKAGE_NAME}_DIR
    ${CMAKE_FIND_PACKAGE_NAME}_VERSION)
  list(APPEND required_vars ${${CMAKE_FIND_PACKAGE_NAME}_CONFIG}
       ${${CMAKE_FIND_PACKAGE_NAME}_DIR})

  if(Parquet_FOUND)
    mark_as_advanced(Parquet_CONFIG Parquet_DIR Parquet_VERSION)
    list(APPEND required_vars ${Parquet_CONFIG} ${Parquet_DIR})
  endif()

  list(APPEND version_var ${${CMAKE_FIND_PACKAGE_NAME}_VERSION})
else()
  set(_arg_SOURCE_SUBDIR cpp)

  FetchContent_GetProperties(${CMAKE_FIND_PACKAGE_NAME})
  string(TOLOWER ${CMAKE_FIND_PACKAGE_NAME} _find_package_name_lower)

  if(${_find_package_name_lower}_POPULATED)
    # ${${_find_package_name_lower}_POPULATED} becomes 1 instead of True when it
    # is populated from FetchContent_GetProperties()
    set(${_find_package_name_lower}_POPULATED True)

    set(_arg_SOURCE_SUBDIR_ABS
        "${${_find_package_name_lower}_SOURCE_DIR}/${_arg_SOURCE_SUBDIR}")
  else()
    set(BITAR_ARROW_GIT_REPOSITORY
        "https://github.com/apache/arrow.git"
        CACHE
          STRING
          "Use the Arrow library from the git repository for building when needed"
    )
    set(BITAR_ARROW_GIT_TAG
        "1b9c57e20802fb061c90837c39e99d8fa69cc212"
        CACHE
          STRING
          "Use the source at the git branch, tag or commit hash from the Arrow repository for building when needed"
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
        OUTPUT_VARIABLE _arrow_git_tag
        OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL LAST)
      set(BITAR_ARROW_GIT_TAG
          "${_arrow_git_tag}"
          CACHE
            STRING
            "Use the source at the git branch, tag or commit hash from the Arrow repository for building when needed"
            FORCE)
      unset(_arrow_git_tag)
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
    set(_backup_CMAKE_INTERPROCEDURAL_OPTIMIZATION
        ${CMAKE_INTERPROCEDURAL_OPTIMIZATION})
    unset(CMAKE_CXX_STANDARD)
    unset(CMAKE_C_STANDARD)
    unset(CMAKE_CXX_CPPCHECK)
    unset(CMAKE_C_CPPCHECK)
    unset(CMAKE_CXX_CLANG_TIDY)
    unset(CMAKE_C_CLANG_TIDY)
    unset(CMAKE_CXX_INCLUDE_WHAT_YOU_USE)
    unset(CMAKE_INTERPROCEDURAL_OPTIMIZATION)

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

    FetchContent_Declare(
      ${CMAKE_FIND_PACKAGE_NAME}
      GIT_REPOSITORY "${BITAR_ARROW_GIT_REPOSITORY}"
      GIT_TAG "${BITAR_ARROW_GIT_TAG}"
      SOURCE_SUBDIR "${_arg_SOURCE_SUBDIR}" OVERRIDE_FIND_PACKAGE)

    FetchContent_Populate(${CMAKE_FIND_PACKAGE_NAME})

    set(_arg_SOURCE_SUBDIR_ABS
        "${${_find_package_name_lower}_SOURCE_DIR}/${_arg_SOURCE_SUBDIR}")

    if(EXISTS "${_arg_SOURCE_SUBDIR_ABS}/CMakeLists.txt")
      if(BITAR_INSTALL_ARROW)
        add_subdirectory("${_arg_SOURCE_SUBDIR_ABS}"
                         "${${_find_package_name_lower}_BINARY_DIR}")
      else()
        add_subdirectory(
          "${_arg_SOURCE_SUBDIR_ABS}"
          "${${_find_package_name_lower}_BINARY_DIR}" EXCLUDE_FROM_ALL)
      endif()
    else()
      message(
        FATAL_ERROR "Could not find CMakeLists.txt in ${_arg_SOURCE_SUBDIR_ABS}"
      )
    endif()

    # Restore our environment settings
    set(CMAKE_CXX_STANDARD ${_backup_CMAKE_CXX_STANDARD})
    set(CMAKE_C_STANDARD ${_backup_CMAKE_C_STANDARD})
    set(CMAKE_CXX_CPPCHECK "${_backup_CMAKE_CXX_CPPCHECK}")
    set(CMAKE_C_CPPCHECK "${_backup_CMAKE_C_CPPCHECK}")
    set(CMAKE_CXX_CLANG_TIDY "${_backup_CMAKE_CXX_CLANG_TIDY}")
    set(CMAKE_C_CLANG_TIDY "${_backup_CMAKE_C_CLANG_TIDY}")
    set(CMAKE_CXX_INCLUDE_WHAT_YOU_USE
        "${_backup_CMAKE_CXX_INCLUDE_WHAT_YOU_USE}")
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION
        ${_backup_CMAKE_INTERPROCEDURAL_OPTIMIZATION})
    unset(_backup_CMAKE_CXX_STANDARD)
    unset(_backup_CMAKE_C_STANDARD)
    unset(_backup_CMAKE_CXX_CPPCHECK)
    unset(_backup_CMAKE_C_CPPCHECK)
    unset(_backup_CMAKE_CXX_CLANG_TIDY)
    unset(_backup_CMAKE_C_CLANG_TIDY)
    unset(_backup_CMAKE_CXX_INCLUDE_WHAT_YOU_USE)
    unset(_backup_CMAKE_INTERPROCEDURAL_OPTIMIZATION)
  endif()

  set(Arrow_IS_BUILT TRUE) # Arrow is built by this project

  set(required_vars ${${_find_package_name_lower}_POPULATED})

  file(STRINGS "${_arg_SOURCE_SUBDIR_ABS}/CMakeLists.txt" _arrow_version_line
       REGEX "^set\\(ARROW_VERSION \"[0-9.]+.*\"\\)")
  if(_arrow_version_line MATCHES "^set\\(ARROW_VERSION \"([0-9.]+.*)\"\\)")
    set(version_var "${CMAKE_MATCH_1} (${BITAR_ARROW_GIT_TAG})")
  else()
    message(
      FATAL_ERROR
        "Could not find ARROW_VERSION in ${_arg_SOURCE_SUBDIR_ABS}/CMakeLists.txt"
    )
  endif()
  unset(_arrow_version_line)

  unset(${_find_package_name_lower}_POPULATED)
  unset(_find_package_name_lower)
  unset(_arg_SOURCE_SUBDIR_ABS)
  unset(_arg_SOURCE_SUBDIR)
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

  set(_libraries arrow parquet)
  foreach(_library_name ${_libraries})
    if(TARGET ${_${_library_name}_library})
      add_library(Arrow::${_library_name} INTERFACE IMPORTED)
      target_link_libraries(Arrow::${_library_name}
                            INTERFACE ${_${_library_name}_library})
      get_target_property(_${_library_name}_include_dirs
                          ${_${_library_name}_library} INCLUDE_DIRECTORIES)

      # This is the case where Arrow is built from source
      if(_${_library_name}_include_dirs)
        set_target_properties(
          Arrow::${_library_name}
          PROPERTIES INTERFACE_INCLUDE_DIRECTORIES
                     "${_${_library_name}_include_dirs}")
        unset(_${_library_name}_include_dirs)

        # Inhibit all warning from the library's headers and sources
        #
        # Apply the compile option to the internal library if it exists. See
        # https://github.com/apache/arrow/blob/apache-arrow-9.0.0/cpp/cmake_modules/BuildUtils.cmake#L224
        if(TARGET ${_library_name}_objlib)
          target_compile_options(${_library_name}_objlib PRIVATE -w)
        else()
          target_compile_options(${_${_library_name}_library} PRIVATE -w)
        endif()
      else()
        if(${_library_name} STREQUAL parquet)
          # The Arrow-provided `ParquetConfig.cmake` will create the
          # thrift::thrift target via `FindThrift.cmake`
          # https://github.com/apache/arrow/blob/apache-arrow-9.0.0/cpp/cmake_modules/FindThrift.cmake#L28
          target_link_libraries(Arrow::${_library_name}
                                INTERFACE thrift::thrift)
        endif()
      endif()

      unset(_${_library_name}_library)
    endif()
  endforeach()
  unset(_libraries)
endif()
