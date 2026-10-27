cmake_minimum_required(VERSION 3.24)
project(VoicePanel LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# --- dependencies -----------------------------------------------------------
# Qt: pass -DCMAKE_PREFIX_PATH=<repo>/runtime/Qt/6.8.3/msvc2022_64
find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets Network Svg)

# ONNX Runtime (Silero VAD): -DONNXRUNTIME_DIR=<repo>/runtime/third_party/onnxruntime
set(ONNXRUNTIME_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../runtime/third_party/onnxruntime"
    CACHE PATH "ONNX Runtime root (include/ + lib/)")

if(WIN32)
  set(UIA_SOURCES src/core/TextInserter.cpp src/core/FocusTracker.cpp src/core/HotkeyManager.cpp)
endif()

set(PANEL_SOURCES
  resources/resources.qrc
  src/app/PanelApp.cpp
  src/app/MainWindow.cpp
  src/app/SettingsDialog.cpp
  src/app/ServerSettingsDialog.cpp
  src/app/UiKit.cpp
  src/app/VoicePlaybackDialog.cpp
  src/app/ServiceTray.cpp
  src/core/ServiceClient.cpp
  src/core/Settings.cpp
  src/core/DictationController.cpp
  src/core/TextProcessor.cpp
  src/core/TextDiff.cpp
  src/core/SpellingChecker.cpp
  src/core/TtsPlayer.cpp
  src/core/MarkdownStore.cpp
  src/core/VoiceProfiles.cpp
  src/audio/WasapiCapture.cpp
  src/audio/Vad.cpp
  ${UIA_SOURCES}
)

add_library(VoicePanelCore STATIC ${PANEL_SOURCES})
target_include_directories(VoicePanelCore PUBLIC
  ${ONNXRUNTIME_DIR}/include
  src
)

target_link_libraries(VoicePanelCore PUBLIC
  Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network Qt6::Svg
  ${ONNXRUNTIME_DIR}/lib/onnxruntime.lib
  ole32 oleaut32 uuid user32 version winmm dwmapi
)

if(MSVC)
  target_compile_options(VoicePanelCore PUBLIC /W4 /permissive- /utf-8 /Zc:preprocessor)
  target_compile_definitions(VoicePanelCore PUBLIC _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
endif()

add_executable(VoicePanel WIN32 src/main.cpp)
target_link_libraries(VoicePanel PRIVATE VoicePanelCore)

# Release builds strip the console; keep asserts informative.
set_target_properties(VoicePanel PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# Deployment helper: copy runtime DLLs next to the exe for local runs.
add_custom_target(copy_dlls
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${ONNXRUNTIME_DIR}/lib/onnxruntime.dll" "$<TARGET_FILE_DIR:VoicePanel>"
  COMMENT "Copying onnxruntime.dll"
)
add_dependencies(VoicePanel copy_dlls)

include(CTest)
if(BUILD_TESTING)
  find_package(Qt6 REQUIRED COMPONENTS Test)
  add_executable(VoicePanelTests tests/PanelTests.cpp)
  target_link_libraries(VoicePanelTests PRIVATE VoicePanelCore Qt6::Test)
  set_target_properties(VoicePanelTests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
  add_test(NAME panel COMMAND VoicePanelTests -platform offscreen)
  get_target_property(QT_QMAKE_EXECUTABLE Qt6::qmake IMPORTED_LOCATION)
  get_filename_component(QT_BIN_DIR "${QT_QMAKE_EXECUTABLE}" DIRECTORY)
  set_tests_properties(panel PROPERTIES WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    TIMEOUT 60
    ENVIRONMENT "QT_QPA_PLATFORM_PLUGIN_PATH=${QT_BIN_DIR}/../plugins/platforms"
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QT_BIN_DIR}")
endif()
