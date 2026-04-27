add_subdirectory(sdcard)

target_sources(pT_dx7 PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/platform_clocks.c
    ${CMAKE_CURRENT_LIST_DIR}/platform_input.c
    ${CMAKE_CURRENT_LIST_DIR}/sdcard_fs.cpp
)

target_link_libraries(pT_dx7 PRIVATE
    platform_sdcard
)

target_include_directories(pT_dx7 PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
)
