# C++ unit tests and Python CLI checks. Included by the root build.
ADD_EXECUTABLE(inspeccy_test_block_io_flags cpp/test_block_io_flags.cpp)
TARGET_LINK_LIBRARIES(inspeccy_test_block_io_flags PRIVATE inspeccy_core)
ADD_TEST(NAME block_io_flags COMMAND inspeccy_test_block_io_flags)
ADD_EXECUTABLE(inspeccy_test_transparency_fallback cpp/test_transparency_fallback.cpp)
TARGET_LINK_LIBRARIES(inspeccy_test_transparency_fallback PRIVATE inspeccy_core)
ADD_TEST(NAME transparency_fallback COMMAND inspeccy_test_transparency_fallback)
ADD_EXECUTABLE(inspeccy_test_video_cadence cpp/test_video_cadence.cpp)
TARGET_LINK_LIBRARIES(inspeccy_test_video_cadence PRIVATE inspeccy_core)
ADD_TEST(NAME video_capture_cadence COMMAND inspeccy_test_video_cadence)
ADD_EXECUTABLE(inspeccy_test_beam_frame cpp/test_beam_frame.cpp)
TARGET_LINK_LIBRARIES(inspeccy_test_beam_frame PRIVATE inspeccy_core)
ADD_TEST(NAME beam_completed_frame COMMAND inspeccy_test_beam_frame)
ADD_EXECUTABLE(inspeccy_test_copper_wait_jitter cpp/test_copper_wait_jitter.cpp)
TARGET_LINK_LIBRARIES(inspeccy_test_copper_wait_jitter PRIVATE inspeccy_core)
ADD_TEST(NAME copper_wait_jitter COMMAND inspeccy_test_copper_wait_jitter)
ADD_EXECUTABLE(inspeccy_test_next_ay cpp/test_next_ay.cpp)
TARGET_LINK_LIBRARIES(inspeccy_test_next_ay PRIVATE inspeccy_core)
ADD_TEST(NAME next_three_ay COMMAND inspeccy_test_next_ay)
ADD_EXECUTABLE(inspeccy_test_emul_audio cpp/test_emul_audio.cpp)
TARGET_LINK_LIBRARIES(inspeccy_test_emul_audio PRIVATE inspeccy_core)
ADD_TEST(NAME emul_ay_audio COMMAND inspeccy_test_emul_audio)
ADD_EXECUTABLE(inspeccy_test_inspection cpp/test_inspection.cpp)
TARGET_LINK_LIBRARIES(inspeccy_test_inspection PRIVATE inspeccy_core)
ADD_TEST(NAME inspection_core COMMAND inspeccy_test_inspection)
ADD_EXECUTABLE(inspeccy_test_dma_addressing cpp/test_dma_addressing.cpp)
TARGET_LINK_LIBRARIES(inspeccy_test_dma_addressing PRIVATE inspeccy_core)
ADD_TEST(NAME dma_addressing COMMAND inspeccy_test_dma_addressing)
ADD_EXECUTABLE(inspeccy_test_nex_contract cpp/test_nex_contract.cpp)
TARGET_LINK_LIBRARIES(inspeccy_test_nex_contract PRIVATE inspeccy_core)
ADD_TEST(NAME nex_loader_contract COMMAND inspeccy_test_nex_contract ${CMAKE_CURRENT_BINARY_DIR}/nex-contract.nex)
ADD_EXECUTABLE(inspeccy_test_shared cpp/test_shared.cpp)
TARGET_LINK_LIBRARIES(inspeccy_test_shared PRIVATE inspeccy_shared)
ADD_TEST(NAME shared_input_validation COMMAND inspeccy_test_shared)
ADD_EXECUTABLE(inspeccy_test_asm_index cpp/test_asm_index.cpp)
TARGET_LINK_LIBRARIES(inspeccy_test_asm_index PRIVATE inspeccy_shared)
ADD_TEST(NAME asm_index COMMAND inspeccy_test_asm_index)
ADD_EXECUTABLE(inspeccy_test_sprites cpp/test_sprites.cpp)
TARGET_LINK_LIBRARIES(inspeccy_test_sprites PRIVATE inspeccy_core)
ADD_TEST(NAME sprite_attributes COMMAND inspeccy_test_sprites ${CMAKE_CURRENT_BINARY_DIR}/sprite-test.bmp)
IF(Python3_Interpreter_FOUND)
    ADD_TEST(NAME fixture_provenance COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/python/test_fixtures.py)
    ADD_TEST(NAME agent_skills COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/python/test_agent_skills.py)
    ADD_TEST(NAME inspection_cli COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/python/test_inspection.py $<TARGET_FILE:inspeccy>)
    ADD_TEST(NAME batch_cli COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/python/test_batch.py $<TARGET_FILE:inspeccy>)
    ADD_TEST(NAME uart_process COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/python/test_uart_process.py $<TARGET_FILE:inspeccy>)
    SET_TESTS_PROPERTIES(uart_process PROPERTIES SKIP_RETURN_CODE 77)
    ADD_TEST(NAME shared_tooling COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/python/test_tooling.py $<TARGET_FILE:inspeccy>)
    ADD_TEST(NAME nex_bc_handle COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/python/test_nex_bc_handle.py $<TARGET_FILE:inspeccy>)
    ADD_TEST(NAME comparison_tools COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/python/test_comparison_tools.py
        $<TARGET_FILE:ssim-compare> $<TARGET_FILE:wav-compare>)
    ADD_EXECUTABLE(inspeccy_test_vfs cpp/test_vfs.cpp)
    TARGET_LINK_LIBRARIES(inspeccy_test_vfs PRIVATE inspeccy_core)
    ADD_TEST(NAME vfs_mount_boundary COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/python/test_vfs.py $<TARGET_FILE:inspeccy_test_vfs>)
    ADD_TEST(NAME outd_flags COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/python/test_outd_flags.py --inspeccy $<TARGET_FILE:inspeccy>)
    IF(INSPECCY_TRACE_SUPPORT)
        ADD_TEST(NAME deferred_trace COMMAND ${CMAKE_COMMAND} -E env
            "INSPECCY=$<TARGET_FILE:inspeccy>" ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/python/test_deferred_trace.py)
        ADD_TEST(NAME cli_input_validation COMMAND ${CMAKE_COMMAND} -E env
            "INSPECCY=$<TARGET_FILE:inspeccy>" ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/python/test_cli_validation.py)
    ENDIF()
ENDIF()
