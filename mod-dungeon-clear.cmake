if (BUILD_TESTING)
    function(define_dungeon_clear_tests)
        set(MOD_PATH "${CMAKE_SOURCE_DIR}/modules/mod-dungeon-clear")

        # Define our standalone test target
        add_executable(dungeon_clear_tests
            "${MOD_PATH}/t/TestDungeonClearMath.cpp"
            "${MOD_PATH}/t/TestDungeonClearUtil.cpp"
            "${MOD_PATH}/t/TestDungeonClearApproach.cpp"
            "${CMAKE_SOURCE_DIR}/src/test/mocks/TestMap.cpp"
        )

        # Link the necessary targets
        target_link_libraries(dungeon_clear_tests
            game
            gtest_main
            gmock_main
            game-interface
            modules
            scripts
        )

        # Include directories
        target_include_directories(dungeon_clear_tests PRIVATE
            "${MOD_PATH}/src"
            "${MOD_PATH}/src/Ai/Dungeon/DungeonClear/Util"
            "${CMAKE_SOURCE_DIR}/src/test/mocks"
        )

        # Place executable directly in the main build folder for easy execution
        set_target_properties(dungeon_clear_tests PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
        )
    endfunction()

    # Defer execution to the root directory's configuration end, after googletest has been fetched by the core
    cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL define_dungeon_clear_tests)
endif()
