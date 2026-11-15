cmake_minimum_required(VERSION 3.22)

# Also usable as add_subdirectory(windows_UI) after the engine defines
# spk_desktop_host. The standalone mode never modifies the engine checkout.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    # C is required by the engine's spk_volk (third_party/volk/volk.c), which
    # this standalone mode builds via add_subdirectory below. Without it the
    # generate step dies on CMAKE_C_CREATE_STATIC_LIBRARY; the engine's own
    # parent project enables C, so mirror it here.
    project(SpektraLabWindowsUI LANGUAGES C CXX)
    include(CTest)
    set(SPEKTRALAB_SOURCE_ROOT "" CACHE PATH "Source tree supplying engine/")
    if(NOT EXISTS "${SPEKTRALAB_SOURCE_ROOT}/engine/CMakeLists.txt")
        message(FATAL_ERROR "Set SPEKTRALAB_SOURCE_ROOT to the SpektraLab source tree")
    endif()
    set(SPEKTRALAB_BUILD_NATIVE_RAW ON CACHE BOOL "" FORCE)
    set(SPEKTRALAB_BUILD_DESKTOP ON CACHE BOOL "" FORCE)
    add_subdirectory("${SPEKTRALAB_SOURCE_ROOT}/engine" engine)
endif()

if(NOT TARGET spk_desktop_host)
    message(FATAL_ERROR "Build the engine's spk_desktop_host before windows_UI")
endif()
find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2)
qt_standard_project_setup(REQUIRES 6.8)

qt_add_executable(SpektraLabQt WIN32
    src/main.cpp src/PreviewController.hpp src/PreviewController.cpp
    src/FrameOutput.hpp src/FrameOutput.cpp
    src/EditStore.hpp src/EditStore.cpp
    src/Layer2.hpp src/Layer2.cpp
    src/Histogram.hpp
    src/FrameCanvas.hpp src/FrameCanvas.cpp)
target_compile_features(SpektraLabQt PRIVATE cxx_std_20)
target_link_libraries(SpektraLabQt PRIVATE
    spk_desktop_host Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2)
set_source_files_properties(qml/Theme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)
qt_add_qml_module(SpektraLabQt URI SpektraLab VERSION 1.0
    QML_FILES qml/Main.qml qml/Theme.qml qml/Section.qml qml/ActionButton.qml
        qml/CurveEditor.qml qml/ColorBalance.qml
    NO_CACHEGEN)
set_target_properties(SpektraLabQt PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/app")

# The one resource directory is explicitly adjacent to the executable. No
# lookup through another checkout and no Python in the application.
add_custom_target(spektralab_qt_resources ALL
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "$<TARGET_FILE_DIR:spk_raw_render>/resources" "$<TARGET_FILE_DIR:SpektraLabQt>/resources"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "$<TARGET_FILE_DIR:spk_raw_render>/licenses" "$<TARGET_FILE_DIR:SpektraLabQt>/licenses"
    VERBATIM)
add_dependencies(spektralab_qt_resources spektralab_vulkan_resources)
add_dependencies(SpektraLabQt spektralab_qt_resources)

if(BUILD_TESTING)
    # The export path grades the print with Layer 2 before geometry, so every
    # target compiling FrameOutput.cpp also compiles Layer2.cpp.
    qt_add_executable(spk_qt_frame_output_test tests/frame_output.cpp
        src/FrameOutput.cpp src/Layer2.cpp)
    target_include_directories(spk_qt_frame_output_test PRIVATE src)
    target_compile_features(spk_qt_frame_output_test PRIVATE cxx_std_20)
    target_link_libraries(spk_qt_frame_output_test PRIVATE spk_desktop_host Qt6::Core Qt6::Gui)
    add_test(NAME spk_qt_frame_output COMMAND spk_qt_frame_output_test "$<TARGET_FILE_DIR:SpektraLabQt>/resources")

    qt_add_executable(spk_qt_library_workflow_test tests/library_workflow.cpp
        src/PreviewController.hpp src/PreviewController.cpp src/FrameOutput.cpp
        src/Layer2.cpp src/EditStore.cpp)
    target_include_directories(spk_qt_library_workflow_test PRIVATE src)
    target_compile_features(spk_qt_library_workflow_test PRIVATE cxx_std_20)
    target_link_libraries(spk_qt_library_workflow_test PRIVATE spk_desktop_host Qt6::Core Qt6::Gui)
    qt_add_executable(spk_qt_edit_store_test tests/edit_store.cpp src/EditStore.cpp
        src/Layer2.cpp)
    target_include_directories(spk_qt_edit_store_test PRIVATE src)
    target_compile_features(spk_qt_edit_store_test PRIVATE cxx_std_20)
    target_link_libraries(spk_qt_edit_store_test PRIVATE spk_desktop_host Qt6::Core Qt6::Gui)
    add_test(NAME spk_qt_edit_store COMMAND spk_qt_edit_store_test)
    # Layer 2's data contract needs neither the engine nor the GPU: only the
    # JSON wire shape shared with desktop/src/shared/adjustments.ts.
    qt_add_executable(spk_qt_layer2_test tests/layer2.cpp src/Layer2.cpp)
    target_include_directories(spk_qt_layer2_test PRIVATE src)
    target_compile_features(spk_qt_layer2_test PRIVATE cxx_std_20)
    target_link_libraries(spk_qt_layer2_test PRIVATE Qt6::Core Qt6::Gui)
    add_test(NAME spk_qt_layer2 COMMAND spk_qt_layer2_test)
    # The output histogram's counting core is header-only and needs neither the
    # engine nor the GPU; its bins must match the desktop's histogram-core.ts.
    qt_add_executable(spk_qt_histogram_test tests/histogram.cpp)
    target_include_directories(spk_qt_histogram_test PRIVATE src)
    target_compile_features(spk_qt_histogram_test PRIVATE cxx_std_20)
    target_link_libraries(spk_qt_histogram_test PRIVATE Qt6::Core Qt6::Gui)
    add_test(NAME spk_qt_histogram COMMAND spk_qt_histogram_test)
    # The Temperature/Tint math the white-balance row drives is pure engine
    # host code (engine/host/white_balance.cpp, compiled into spk_image_io
    # beside its raw_decoder caller): Robertson mapping, its inverse and the
    # as-shot solve. LibRaw is linked only to construct the synthetic camera
    # structure the math reads -- no RAW file, GPU or engine session.
    qt_add_executable(spk_qt_wb_math_test tests/wb_math.cpp)
    target_compile_features(spk_qt_wb_math_test PRIVATE cxx_std_20)
    target_include_directories(spk_qt_wb_math_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../engine/host")
    target_link_libraries(spk_qt_wb_math_test PRIVATE spk_desktop_host spk_libraw)
    add_test(NAME spk_qt_wb_math COMMAND spk_qt_wb_math_test)
    # The pair wire's host boundary: the split/b_* settings, the no-session
    # refusals of the two thin analysis wrappers, and a synthetic pair session
    # whose gate geometry the engine answers with two gates. No RAW fixture --
    # the pair's buffer is composed in the test itself and opened through the
    # C API with the same wire open_raw sends.
    qt_add_executable(spk_qt_pair_host_test tests/pair_host.cpp)
    target_compile_features(spk_qt_pair_host_test PRIVATE cxx_std_20)
    target_link_libraries(spk_qt_pair_host_test PRIVATE spk_desktop_host)
    add_test(NAME spk_qt_pair_host COMMAND spk_qt_pair_host_test
        "$<TARGET_FILE_DIR:SpektraLabQt>/resources")
    # The pair piece's composition core (PAIR-WIRING §2/§4): layout maths,
    # per-hole compose and the meter key need no engine at all; the exposure
    # solve opens its own throwaway 1024 px meter session on the bundled
    # resources, like the pair-host test opens its synthetic pair session.
    qt_add_executable(spk_qt_pair_piece_test tests/pair_piece.cpp src/PairPiece.cpp)
    target_include_directories(spk_qt_pair_piece_test PRIVATE src)
    target_compile_features(spk_qt_pair_piece_test PRIVATE cxx_std_20)
    target_link_libraries(spk_qt_pair_piece_test PRIVATE spk_desktop_host Qt6::Core Qt6::Gui)
    add_test(NAME spk_qt_pair_piece COMMAND spk_qt_pair_piece_test
        "$<TARGET_FILE_DIR:SpektraLabQt>/resources")
    if(WIN32)
        # Specify each test's DLL search path: CTest on this Windows host can
        # restore the pre-launch PATH between tests. Ambient PATH alone makes
        # the second Qt test fail to load even when it passes in isolation.
        set(qt_test_runtime_path "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
        if(MINGW)
            get_filename_component(qt_test_compiler_directory "${CMAKE_CXX_COMPILER}" DIRECTORY)
            list(APPEND qt_test_runtime_path "PATH=path_list_prepend:${qt_test_compiler_directory}")
        endif()
        set_tests_properties(spk_qt_frame_output spk_qt_edit_store spk_qt_layer2 spk_qt_histogram
            spk_qt_wb_math spk_qt_pair_host spk_qt_pair_piece PROPERTIES
            ENVIRONMENT_MODIFICATION "${qt_test_runtime_path}")
    endif()
    qt_add_executable(spk_qt_editing_workflow_test tests/editing_workflow.cpp
        src/PreviewController.hpp src/PreviewController.cpp src/FrameOutput.cpp
        src/Layer2.cpp src/EditStore.cpp src/FrameCanvas.hpp src/FrameCanvas.cpp)
    target_include_directories(spk_qt_editing_workflow_test PRIVATE src)
    target_compile_features(spk_qt_editing_workflow_test PRIVATE cxx_std_20)
    # The split-compare gate calls FrameCanvas's composite directly, which
    # pulls in QQuickItem.
    target_link_libraries(spk_qt_editing_workflow_test PRIVATE spk_desktop_host Qt6::Core Qt6::Gui Qt6::Quick)
    # Full-RAW fixtures and a fresh output directory are explicit test arguments;
    # no absent-fixture skip can masquerade as a green CTest run.
endif()
