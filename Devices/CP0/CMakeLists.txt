# CardputerZero (CP0) device family.
#
# P0 bring-up: the Device facade (Devices/CP0/Device.cpp) renders the 240x135 engine framebuffer and
# Present()s it nearest-neighbour upscaled to the CP0 panel's 320x170. CP0Shot boots the real runtime,
# replays a key script and writes 320x170 PNG frames (plus optional PCM) on any host OS with no display.
#
# The shared audio library (CardenzaDeviceAudio, defined in the root CMakeLists) takes this device's
# DeviceAudio.cpp, exactly as the CardenzaSim family does.

target_sources(CardenzaDeviceAudio PRIVATE
    ${PROJECT_SOURCE_DIR}/Devices/CP0/DeviceAudio.cpp
)

add_library(CP0Device)
target_sources(CP0Device PRIVATE
    ${PROJECT_SOURCE_DIR}/Devices/CP0/Device.cpp
    ${PROJECT_SOURCE_DIR}/Devices/CP0/Storage.cpp
)
target_include_directories(CP0Device PUBLIC
    ${PROJECT_SOURCE_DIR}
)
target_link_libraries(CP0Device PUBLIC
    CardenzaDeviceInterface
)

# Headless CP0 presenter/capture: boots os::system, runs frames, upscales to 320x170 and writes PNGs.
add_executable(CP0Shot
    ${PROJECT_SOURCE_DIR}/Devices/CP0/Headless/HeadlessScript.cpp
    ${PROJECT_SOURCE_DIR}/Devices/CP0/Headless/CP0Shot.cpp
    ${PROJECT_SOURCE_DIR}/Devices/CP0/Headless/PngWriter.cpp
)
target_include_directories(CP0Shot PRIVATE
    ${PROJECT_SOURCE_DIR}
)
target_link_libraries(CP0Shot PRIVATE
    CP0Device
    CardenzaSystem
)

# Native Linux host (P1): blit to /dev/fb0, read the CP0 keyboard from evdev, stream the synth's
# PCM to ALSA. Not built for the web (that path is Devices/CardenzaWeb).
if(NOT EMSCRIPTEN)
    find_package(Threads REQUIRED)
    find_library(ASOUND_LIB asound REQUIRED)
    add_executable(CP0Main
        ${PROJECT_SOURCE_DIR}/Devices/CP0/CP0Main.cpp
        ${PROJECT_SOURCE_DIR}/Devices/CP0/UsbMidi.cpp
    )
    target_include_directories(CP0Main PRIVATE
        ${PROJECT_SOURCE_DIR}
    )
    target_link_libraries(CP0Main PRIVATE
        CP0Device
        CardenzaSystem
        CardenzaApplications
        Threads::Threads
        ${ASOUND_LIB}
    )
endif()
