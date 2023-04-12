#
# Copyright 2020, Data61, CSIRO (ABN 41 687 119 230)
#
# SPDX-License-Identifier: BSD-2-Clause
#

# This module contains functions for creating a cpio archive containing a list
# of input files and turning it into an object file that can be linked into a binary.

include_guard(GLOBAL)

# Checks the existence of an argument to cpio -o.
# flag refers to a variable in the parent scope that contains the argument, if
# the argument isn't supported then the flag is set to the empty string in the parent scope.
function(CheckCPIOArgument var flag)
    if(NOT (DEFINED ${var}))
        file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/cpio-testfile "Testfile contents")
        execute_process(
            COMMAND bash -c "echo cpio-testfile | cpio ${flag} -o"
            WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
            OUTPUT_QUIET ERROR_QUIET
            RESULT_VARIABLE result
        )
        if(result)
            set(${var} "" CACHE INTERNAL "")
            message(STATUS "CPIO test ${var} FAILED")
        else()
            set(${var} "${flag}" CACHE INTERNAL "")
            message(STATUS "CPIO test ${var} PASSED")
        endif()
        file(REMOVE ${CMAKE_CURRENT_BINARY_DIR}/cpio-testfile)
    endif()
endfunction()

# Function for declaring rules to build a cpio archive that can be linked
# into another target
function(MakeCPIO output_name input_files)
    cmake_parse_arguments(PARSE_ARGV 2 MAKE_CPIO "" "CPIO_SYMBOL" "DEPENDS")
    if(MAKE_CPIO_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "Unknown arguments to MakeCPIO")
    endif()
    set(archive_symbol "_cpio_archive")
    if(MAKE_CPIO_CPIO_SYMBOL)
        set(archive_symbol ${MAKE_CPIO_CPIO_SYMBOL})
    endif()

    set(cpio_archive "${CMAKE_CURRENT_BINARY_DIR}/archive.${output_name}.cpio")

    # Create a script that prepares CPIO archive contents in a tmp folder and
    # then builds the archive. We have to pass the files to cpio in the same
    # order as we go them, because some application will access the contents by
    # index  and not by name.
    # For some cpio versions "--reproducible" is available, if this is the case
    # the archive is created with consistent inodes and device numbering. With
    # "touch -date  @0 <file>" the 'modified time' is set to 0. The cpio
    # parameter "--owner=+0:+0" sets user and group values to 0:0.
    CheckCPIOArgument(cpio_reproducible_flag "--reproducible")
    set(cpio_archive_creator "${CMAKE_CURRENT_BINARY_DIR}/${output_name}.sh")
    file(
        WRITE
            "${cpio_archive_creator}"
            "#!/usr/bin/env bash\n"
            "# auto-generated file from MakeCPIO(), do not edit\n"
            "TMP_DIR=temp_${output_name}\n"
            "mkdir -p \${TMP_DIR}\n"
            "cp -a \"$@\" \${TMP_DIR}/\n"
            "(\n"
            "  cd \${TMP_DIR}\n"
            "  touch --date @0 *\n"
            "  printf '%s\\n' \${@##*/} | cpio ${cpio_reproducible_flag} --quiet --create --format=newc --owner=+0:+0\n"
            ") >${cpio_archive}\n"
            "rm -rf \${TMP_DIR}\n"
    )

    # Create a "program" that makes the compiler generate and object file that
    # contains the cpio archive.
    set(cpio_archive_s "${CMAKE_CURRENT_BINARY_DIR}/${output_name}.S")
    file(
        WRITE
            "${cpio_archive_s}"
            "# auto-generated file from MakeCPIO(), do not edit\n"
            ".section ._archive_cpio, \"aw\"\n"
            ".globl ${archive_symbol}, ${archive_symbol}_end\n"
            "${archive_symbol}:\n"
            ".incbin \"${cpio_archive}\"\n"
            "${archive_symbol}_end:\n"
    )

    # Re-run CMake configuration in case ${cpio_archive_s} is deleted
    set_property(
        DIRECTORY
        APPEND
        PROPERTY CMAKE_CONFIGURE_DEPENDS "${cpio_archive_creator}" "${cpio_archive_s}"
    )

    # The 'cpio_archive' is no explicit parameter for the command, because it is
    # hard-coded already in the specific script we have generated above. The
    # 'input_files' are explicit here, because they are a CMake list that
    # will be converted to parameters for the invokation,
    add_custom_command(
        OUTPUT ${cpio_archive}
        COMMAND bash "${cpio_archive_creator}" ${input_files}
        DEPENDS "${cpio_archive_creator}" ${input_files} ${MAKE_CPIO_DEPENDS}
        COMMENT "Generate CPIO archive ${cpio_archive}"
    )

    separate_arguments(cmake_c_flags_sep NATIVE_COMMAND "${CMAKE_C_FLAGS}")
    if(CMAKE_C_COMPILER_ID STREQUAL "Clang")
        list(APPEND cmake_c_flags_sep "${CMAKE_C_COMPILE_OPTIONS_TARGET}${CMAKE_C_COMPILER_TARGET}")
    endif()
    # The 'cpio_archive' is no explicit parameter for the command, because it is
    # hard-coded already in the specific 'cpio_archive_s' file we have generated
    # above.
    add_custom_command(
        OUTPUT ${output_name}
        COMMAND
            ${CMAKE_C_COMPILER} ${cmake_c_flags_sep} -c -o "${output_name}" "${cpio_archive_s}"
        DEPENDS "${cpio_archive_creator}" "${cpio_archive_s}" "${cpio_archive}"
        COMMENT "Generate CPIO archive ${output_name}"
    )

endfunction(MakeCPIO)
