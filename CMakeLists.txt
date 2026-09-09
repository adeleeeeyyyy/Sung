cmake_minimum_required(VERSION 3.24)
project(Sung VERSION 0.11.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

include(CheckIPOSupported)
check_ipo_supported(RESULT ipo)
if(ipo)
  set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
endif()

find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Quick Qml QuickControls2 Multimedia Network Svg)
if(UNIX AND NOT APPLE AND NOT ANDROID)
    find_package(Qt6 6.4 OPTIONAL_COMPONENTS DBus)
endif()

if(Qt6DBus_FOUND)
    set(SUNG_HAS_DBUS TRUE)
endif()

if(COMMAND qt_policy)
    qt_policy(SET QTP0004 NEW)
endif()

qt_add_executable(sung
    src/windowresources.h
    src/desktoptheme.cpp
    src/desktoptheme.h
    src/main.cpp
    src/roundedart.cpp
    src/roundedart.h
    src/romanizer.cpp
    src/romanizer.h
    src/backend.cpp
    src/backend.h
    src/collectionview.h
    src/rowselection.h
    src/mpris.cpp
    src/mpris.h
    src/playbacknotifier.cpp
    src/playbacknotifier.h
)

target_link_libraries(sung PRIVATE Qt6::Core Qt6::Gui Qt6::Quick Qt6::Qml Qt6::QuickControls2 Qt6::Multimedia Qt6::Network Qt6::Svg)
if(SUNG_HAS_DBUS)
    target_link_libraries(sung PRIVATE Qt6::DBus)
    target_compile_definitions(sung PRIVATE SUNG_HAS_DBUS)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(sung PRIVATE -Wall -Wextra -Wpedantic "$<$<CONFIG:Release>:-Oz;-ffunction-sections;-fdata-sections>")
    if(NOT APPLE AND NOT WIN32)
        target_link_options(sung PRIVATE "$<$<CONFIG:Release>:-Oz;-Wl,--gc-sections;-Wl,-z,pack-relative-relocs>")
    else()
        target_link_options(sung PRIVATE "$<$<CONFIG:Release>:-Oz;-Wl,--gc-sections>")
    endif()
endif()

set_target_properties(sung PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN YES)

# This small, first-use-only component can compile on demand. Keep the player
# and animation components precompiled, without embedding unused picker bytecode.
set_source_files_properties(qml/FileDialogs.qml PROPERTIES QT_QML_SKIP_CACHEGEN TRUE)
set_source_files_properties(qml/Theme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)
set_target_properties(sung PROPERTIES QT_QMLCACHEGEN_ARGUMENTS "--only-bytecode")

qt_add_qml_module(sung URI SungUi VERSION 1.0 NO_PLUGIN NO_RESOURCE_TARGET_PATH RESOURCE_PREFIX "/"
  QML_FILES qml/TrackList.qml qml/SelectionBar.qml qml/FileDialogs.qml qml/SettingSlider.qml qml/LyricsView.qml qml/ImmersivePlayer.qml qml/MiniPlayer.qml qml/MMenu.qml qml/MMenuItem.qml qml/MDivider.qml qml/MDialog.qml qml/MSwitch.qml qml/Main.qml qml/Theme.qml qml/Icon.qml qml/MButton.qml qml/SungText.qml qml/Artwork.qml qml/TrackRow.qml qml/ArtCard.qml qml/SeekBar.qml)

file(GLOB icons CONFIGURE_DEPENDS RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}" assets/icons/*.svg)
qt_add_resources(sung assets PREFIX "/" FILES ${icons} assets/sung.svg
  OPTIONS --compress 19 --threshold 0)

if(CMAKE_BUILD_TYPE STREQUAL "Release" AND UNIX AND NOT APPLE AND NOT ANDROID AND CMAKE_STRIP)
  add_custom_command(TARGET sung POST_BUILD COMMAND ${CMAKE_STRIP} --strip-unneeded $<TARGET_FILE:sung>)
endif()

include(GNUInstallDirs)
install(TARGETS sung RUNTIME DESTINATION bin)
if(UNIX AND NOT APPLE AND NOT ANDROID)
  install(FILES helper/catalog.py helper/requirements.txt DESTINATION lib/sung)
  install(FILES assets/sung.desktop DESTINATION share/applications)
  install(FILES assets/sung.svg DESTINATION share/icons/hicolor/scalable/apps)
  install(FILES LICENSE NOTICE licenses/MaterialSymbols-LICENSE.txt DESTINATION share/licenses/sung)
endif()

include(CTest)
if(BUILD_TESTING)
  find_package(Qt6 REQUIRED COMPONENTS Test)
  qt_add_executable(sung-artwork-tests tests/artwork_test.cpp src/roundedart.cpp src/roundedart.h)
  target_include_directories(sung-artwork-tests PRIVATE src)
  target_link_libraries(sung-artwork-tests PRIVATE Qt6::Core Qt6::Gui Qt6::Quick Qt6::Network Qt6::Test)
  add_test(NAME artwork COMMAND sung-artwork-tests)
  set_tests_properties(artwork PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software")

  qt_add_executable(sung-tests tests/backend_test.cpp src/desktoptheme.cpp src/desktoptheme.h src/romanizer.cpp src/romanizer.h src/backend.cpp src/backend.h src/roundedart.cpp src/roundedart.h src/collectionview.h src/rowselection.h src/playbacknotifier.cpp src/playbacknotifier.h)
  target_include_directories(sung-tests PRIVATE src)
  target_link_libraries(sung-tests PRIVATE Qt6::Core Qt6::Gui Qt6::Quick Qt6::Multimedia Qt6::Network Qt6::Test)
  if(SUNG_HAS_DBUS)
    target_link_libraries(sung-tests PRIVATE Qt6::DBus)
    target_compile_definitions(sung-tests PRIVATE SUNG_HAS_DBUS)
  endif()
  add_test(NAME backend COMMAND sung-tests)
  set_tests_properties(backend PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;SUNG_FIXTURE_HELPER=${CMAKE_CURRENT_SOURCE_DIR}/tests/catalog_fixture.py;QT_FFMPEG_DECODING_HW_DEVICE_TYPES=,;QT_FFMPEG_ENCODING_HW_DEVICE_TYPES=,")

  if(SUNG_HAS_DBUS)
    find_program(DBUS_RUN_SESSION dbus-run-session)
    if(DBUS_RUN_SESSION)
      qt_add_executable(sung-notification-tests tests/notification_test.cpp src/romanizer.cpp src/romanizer.h src/backend.cpp src/backend.h src/roundedart.cpp src/roundedart.h src/collectionview.h src/rowselection.h src/playbacknotifier.cpp src/playbacknotifier.h)
      target_include_directories(sung-notification-tests PRIVATE src)
      target_link_libraries(sung-notification-tests PRIVATE Qt6::Core Qt6::Gui Qt6::Quick Qt6::Multimedia Qt6::Network Qt6::DBus Qt6::Test)
      target_compile_definitions(sung-notification-tests PRIVATE SUNG_HAS_DBUS)
      add_test(NAME notifications COMMAND ${DBUS_RUN_SESSION} -- $<TARGET_FILE:sung-notification-tests>)
      set_tests_properties(notifications PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;SUNG_FIXTURE_HELPER=${CMAKE_CURRENT_SOURCE_DIR}/tests/catalog_fixture.py;QT_FFMPEG_DECODING_HW_DEVICE_TYPES=,;QT_FFMPEG_ENCODING_HW_DEVICE_TYPES=,")
    endif()
  endif()
endif()

option(SUNG_DIAGNOSTICS "Build the live UI verification harness" OFF)
if(SUNG_DIAGNOSTICS)
  qt_add_executable(sung-artwork-fixture tests/artwork_fixture.cpp)
  target_link_libraries(sung-artwork-fixture PRIVATE Qt6::Core Qt6::Gui Qt6::Network)
  find_package(Qt6 REQUIRED COMPONENTS Test GuiPrivate)
  target_sources(sung PRIVATE tests/uitest.cpp tests/uitest.h tests/benchmark.cpp)
  target_include_directories(sung PRIVATE tests src)
  target_compile_definitions(sung PRIVATE SUNG_DIAGNOSTICS)
  target_link_libraries(sung PRIVATE Qt6::Test Qt6::GuiPrivate)
endif()

