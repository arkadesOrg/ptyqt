project(ptyqt-tests)

enable_testing()

find_package(Qt6Test REQUIRED)

add_executable(ptyqt_tests ptyqt_tests.cpp)
add_dependencies(ptyqt_tests ptyqt)

if (MSVC)
    # Keep the Windows CI gate focused on the ConPTY backend. The legacy
    # WinPTY test remains available as "ptyqt_tests winpty", but currently
    # waits indefinitely for shell output on hosted Windows runners.
    add_test(NAME ptyqt_tests COMMAND ptyqt_tests conpty -v2)
else()
    add_test(NAME ptyqt_tests COMMAND ptyqt_tests)
endif()

if (MSVC)
    # Windows does not search the sibling core/ directory when loading the test
    # executable. Deploy the shared library beside the test binary.
    add_custom_command(
        TARGET ptyqt_tests
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:ptyqt>"
            "$<TARGET_FILE_DIR:ptyqt_tests>"
        COMMENT "Deploy ptyqt runtime library for tests"
    )

    if ("${PTYQT_DEBUG}")
        set(WINPTY_DEBUG_SERVER_PATH ${WINPTY_ROOT_DIR}/${TARGET_ARCH}/bin/winpty-debugserver.exe)
        add_definitions(-DWINPTY_DEBUG_SRV_PATH="${WINPTY_DEBUG_SERVER_PATH}")
        message("WinPty debug server path " ${WINPTY_DEBUG_SERVER_PATH})
    endif()
endif()

target_link_libraries(ptyqt_tests ptyqt Qt6::Core Qt6::Test)

if (MSVC)
    target_link_libraries(ptyqt_tests Qt6::Network ${WINPTY_LIBS})
endif()

foreach( file_i ${WINPTY_DIST_FILES})
    add_custom_command(
    TARGET ptyqt_tests
    POST_BUILD
    COMMAND ${CMAKE_COMMAND}
    ARGS -E copy ${file_i} ${CMAKE_CURRENT_BINARY_DIR}
)
endforeach( file_i )

#file(COPY ${WINPTY_DIST_FILES} DESTINATION ${CMAKE_CURRENT_BINARY_DIR})
