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
    src/PairPiece.hpp src/PairPiece.cpp
    src/Layer2.hpp src/Layer2.cpp
    src/Histogram.hpp
    src/FrameCanvas.hpp src/FrameCanvas.cpp
    # The library window's data layer (task 4.2): the helper shell, the
    # sources registry and the row/collection/facet model over them. Task 4.3
    # adds the import pipeline, the folder sync and the thumbnail store.
    src/LibraryHost.hpp src/LibraryHost.cpp
    src/CatalogSourcesStore.hpp src/CatalogSourcesStore.cpp
    src/LibraryModel.hpp src/LibraryModel.cpp
    src/ThumbnailStore.hpp src/ThumbnailStore.cpp
    # The settings clipboard's pure group semantics (task 4.4), shared by the
    # model's copy/paste and its own test.
    src/SettingsClipboard.hpp src/SettingsClipboard.cpp)
target_compile_features(SpektraLabQt PRIVATE cxx_std_20)
target_include_directories(SpektraLabQt PRIVATE src)
target_link_libraries(SpektraLabQt PRIVATE
    spk_desktop_host Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2)
set_source_files_properties(qml/Theme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)
qt_add_qml_module(SpektraLabQt URI SpektraLab VERSION 1.0
    QML_FILES qml/Main.qml qml/Theme.qml qml/Section.qml qml/ActionButton.qml
        qml/CurveEditor.qml qml/ColorBalance.qml
        # The library page (task 4.4): the sidebar's three tabs and the
        # lazy grid over the library rows model.
        qml/LibrarySidebar.qml qml/LibraryGridPage.qml
    # The localization core (task 6.0): L() over the Mac's catalog, exposed
    # to QML as the Localization singleton (same-module, no import needed —
    # Theme sets that convention). Its sources compile here, into the module
    # host, so the QML type registrar sees QML_SINGLETON.
    SOURCES src/Localization.hpp src/Localization.cpp
    NO_CACHEGEN)
# The catalog is the executable's data (task 6.0): embedded as a Qt
# resource, so no deploy step can leave it behind. windows_UI/tools/
# xcstrings_to_json.py regenerates it from the Mac's Localizable.xcstrings.
qt_add_resources(SpektraLabQt "localization_catalog"
    PREFIX "/" FILES assets/localization.json)
set_target_properties(SpektraLabQt PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/app")
# qmlimportscanner passes the source tree through a UTF-8 response file, and
# the Windows tool decodes it in the host code page — a non-ASCII checkout
# path (here: 项目复刻) reaches the scanner as mojibake and configure fails.
# Shared Qt resolves its QML plugins through the qml/ directory at runtime,
# so the configure-time import scan (a static-build concern) adds nothing.
set_property(TARGET SpektraLabQt PROPERTY QT_QML_MODULE_NO_IMPORT_SCAN TRUE)

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
        src/PairPiece.cpp src/Layer2.cpp src/EditStore.cpp)
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
    # The library helper's wire. The protocol tier is pure JSON (line shapes,
    # id routing, the ready event, the abi gate) and always runs; the host
    # smoke runs only when the build knows a built spklib-host (the local
    # spklib checkout's build/spklib-host.exe on Windows), so a machine
    # without one still proves the wire code compiles and its protocol tier
    # passes. Phase 4's full method surface (task 4.1) grows on this shell.
    set(SPKLIB_HOST_EXECUTABLE "" CACHE FILEPATH
        "Optional built spklib-host for the library-host smoke gate")
    qt_add_executable(spk_qt_library_host_test tests/library_host.cpp
        src/LibraryHost.hpp src/LibraryHost.cpp)
    target_include_directories(spk_qt_library_host_test PRIVATE src)
    target_compile_features(spk_qt_library_host_test PRIVATE cxx_std_20)
    target_link_libraries(spk_qt_library_host_test PRIVATE Qt6::Core)
    add_test(NAME spk_qt_library_host COMMAND spk_qt_library_host_test
        "${SPKLIB_HOST_EXECUTABLE}")
    # The sources.json registry (task 4.1): pure file I/O over QtCore —
    # round-trip, the unreadable set-aside, the date precision, the
    # selected-source semantics and the tree-input partition. No helper
    # process, no engine.
    qt_add_executable(spk_qt_sources_store_test tests/sources_store.cpp
        src/CatalogSourcesStore.hpp src/CatalogSourcesStore.cpp)
    target_include_directories(spk_qt_sources_store_test PRIVATE src)
    target_compile_features(spk_qt_sources_store_test PRIVATE cxx_std_20)
    target_link_libraries(spk_qt_sources_store_test PRIVATE Qt6::Core)
    add_test(NAME spk_qt_sources_store COMMAND spk_qt_sources_store_test)
    # The library data model (task 4.2) over the helper wire. Two tiers like
    # the library-host test: the pure tier (tree partition, query mapping,
    # filter vocabulary, develop-join naming) always runs; the full model
    # workflow — fixture catalog, thirty synthetic PNGs, paging, facets,
    # meta round trips, filters, the EditStore develop join, the
    # openInDevelop signal and a source switch — runs only when a built
    # spklib-host is handed over as argv[1].
    qt_add_executable(spk_qt_library_model_test tests/library_model.cpp
        src/LibraryModel.cpp src/CatalogSourcesStore.cpp src/LibraryHost.cpp
        src/EditStore.cpp src/Layer2.cpp src/ThumbnailStore.cpp
        src/SettingsClipboard.cpp)
    target_include_directories(spk_qt_library_model_test PRIVATE src)
    target_compile_features(spk_qt_library_model_test PRIVATE cxx_std_20)
    target_link_libraries(spk_qt_library_model_test PRIVATE spk_desktop_host Qt6::Core Qt6::Gui)
    add_test(NAME spk_qt_library_model COMMAND spk_qt_library_model_test
        "${SPKLIB_HOST_EXECUTABLE}")
    # The persistent thumbnail store (task 4.3): paint-decode-persist over
    # QtCore/QtGui — no helper process, no GPU, no engine. RAW is the
    # documented placeholder cut (the header records the judgment); its
    # decode paths belong to a future engine seam, not this file.
    qt_add_executable(spk_qt_thumbnail_store_test tests/thumbnail_store.cpp
        src/ThumbnailStore.cpp src/CatalogSourcesStore.cpp)
    target_include_directories(spk_qt_thumbnail_store_test PRIVATE src)
    target_compile_features(spk_qt_thumbnail_store_test PRIVATE cxx_std_20)
    target_link_libraries(spk_qt_thumbnail_store_test PRIVATE Qt6::Core Qt6::Gui)
    add_test(NAME spk_qt_thumbnail_store COMMAND spk_qt_thumbnail_store_test)
    # The settings clipboard's group semantics (task 4.4): pure state
    # arithmetic — a paste writes only the ticked groups' fields, the target
    # keeps its own metering, As Shot white balance, placement, crop and
    # decode mode. No helper process, no engine, no GPU.
    qt_add_executable(spk_qt_settings_clip_test tests/settings_clip.cpp
        src/SettingsClipboard.cpp src/EditStore.cpp src/Layer2.cpp)
    target_include_directories(spk_qt_settings_clip_test PRIVATE src)
    target_compile_features(spk_qt_settings_clip_test PRIVATE cxx_std_20)
    target_link_libraries(spk_qt_settings_clip_test PRIVATE spk_desktop_host Qt6::Core Qt6::Gui)
    add_test(NAME spk_qt_settings_clip COMMAND spk_qt_settings_clip_test)
    # The localization core (task 6.0): the Mac catalog behind L() — the
    # lookup, the fallback, the switch's one signal, the first launch's
    # derive-then-persist, and the shipped asset's own script spot checks.
    # The shipped catalog comes in as argv[1] (the source asset, the same
    # file the app embeds); the language store is a temp INI, never the
    # user's real setting.
    qt_add_executable(spk_qt_localization_test tests/localization.cpp
        src/Localization.cpp)
    target_include_directories(spk_qt_localization_test PRIVATE src)
    target_compile_features(spk_qt_localization_test PRIVATE cxx_std_20)
    target_link_libraries(spk_qt_localization_test PRIVATE Qt6::Core Qt6::Gui Qt6::Qml)
    # Same non-ASCII-path skip as SpektraLabQt above (qmlimportscanner and the
    # UTF-8 response file).
    set_property(TARGET spk_qt_localization_test PROPERTY QT_QML_MODULE_NO_IMPORT_SCAN TRUE)
    add_test(NAME spk_qt_localization COMMAND spk_qt_localization_test
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/localization.json")
    # The catalog's check gate (task 6.0): missing languages, placeholder
    # mismatches, the Simplified/Traditional/kyūjitai script policing and the
    # unwrapped-CJK scan over the QML and C++ sources — the Mac's
    # tools/localization_check.py rules, ported for this frontend. The
    # pre-6.1 Chinese literals stand in the exceptions file until task 6.1
    # migrates them; a migrated string's stale exception fails the gate.
    find_package(Python3 QUIET COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(NAME spk_localization_check COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/localization_check_windows.py"
            "--root" "${CMAKE_CURRENT_SOURCE_DIR}")
    else()
        message(WARNING "Python3 not found: the spk_localization_check gate is not registered")
    endif()
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
            spk_qt_wb_math spk_qt_pair_host spk_qt_pair_piece spk_qt_library_host
            spk_qt_sources_store spk_qt_library_model spk_qt_thumbnail_store
            spk_qt_settings_clip spk_qt_localization PROPERTIES
            ENVIRONMENT_MODIFICATION "${qt_test_runtime_path}")
    endif()
    qt_add_executable(spk_qt_editing_workflow_test tests/editing_workflow.cpp
        src/PreviewController.hpp src/PreviewController.cpp src/FrameOutput.cpp
        src/PairPiece.cpp src/Layer2.cpp src/EditStore.cpp src/FrameCanvas.hpp src/FrameCanvas.cpp)
    target_include_directories(spk_qt_editing_workflow_test PRIVATE src)
    target_compile_features(spk_qt_editing_workflow_test PRIVATE cxx_std_20)
    # The split-compare gate calls FrameCanvas's composite directly, which
    # pulls in QQuickItem.
    target_link_libraries(spk_qt_editing_workflow_test PRIVATE spk_desktop_host Qt6::Core Qt6::Gui Qt6::Quick)
    # Same non-ASCII-path skip as SpektraLabQt above.
    set_property(TARGET spk_qt_editing_workflow_test PROPERTY QT_QML_MODULE_NO_IMPORT_SCAN TRUE)
    # Full-RAW fixtures and a fresh output directory are explicit test arguments;
    # no absent-fixture skip can masquerade as a green CTest run.
endif()
