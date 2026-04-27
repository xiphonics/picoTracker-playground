target_sources(pT_dx7 PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/dx7_rom_bank.c
    ${CMAKE_CURRENT_LIST_DIR}/main.c
    ${CMAKE_CURRENT_LIST_DIR}/midi.c
)

if (NOT USB_MIDI_HOST)
    target_sources(pT_dx7 PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/usb_descriptors.c
    )
endif()

target_include_directories(pT_dx7 PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
)
