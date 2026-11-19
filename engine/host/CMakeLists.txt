# spektralab-host: the engine as a child process (desktop/HOST-PROTOCOL.md).
# The IO layer (image_io/tiff_ifd/stb_impl/platform/white_balance) lives in
# spk_image_io now — shared with the Qt frontend's in-process thumbnail call.
add_executable(spektralab-host
    main.cpp
    output.cpp
)
target_compile_features(spektralab-host PRIVATE cxx_std_20)
target_include_directories(spektralab-host PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/../include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/stb")
target_link_libraries(spektralab-host PRIVATE spk_image_io spk_libraw spektralab_engine spektralab_core)
if(DEFINED SPEKTRALAB_HOST_VERSION)
    target_compile_definitions(spektralab-host PRIVATE SPEKTRALAB_HOST_VERSION="${SPEKTRALAB_HOST_VERSION}")
endif()
if(WIN32)
    target_link_libraries(spektralab-host PRIVATE ws2_32 shell32)
endif()
if(SPEKTRALAB_STATIC_RUNTIME AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_link_options(spektralab-host PRIVATE -static-libstdc++ -static-libgcc)
    if(MINGW)
        target_link_options(spektralab-host PRIVATE -static)
    endif()
endif()
