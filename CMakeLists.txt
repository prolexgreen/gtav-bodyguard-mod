cmake_minimum_required(VERSION 3.20)
project(BodyguardMod CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_library(BodyguardMod SHARED
    src/main.cpp
    src/script.cpp
    src/keyboard.cpp
    src/config.cpp
    src/guards.cpp
    src/escort.cpp
    src/combat.cpp
    src/menu.cpp
    src/vehicles.cpp
    src/chauffeur.cpp
    src/motorcade.cpp
    src/heli.cpp
    src/registry.cpp
)

target_compile_definitions(BodyguardMod PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(BodyguardMod PRIVATE /W3 /MP /wd4244 /wd4838)
target_link_libraries(BodyguardMod PRIVATE ${CMAKE_SOURCE_DIR}/sdk/lib/ScriptHookV.lib)

set_target_properties(BodyguardMod PROPERTIES
    SUFFIX ".asi"
    OUTPUT_NAME "BodyguardMod"
)

# Keep the default ini next to the built .asi
add_custom_command(TARGET BodyguardMod POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/BodyguardMod.ini $<TARGET_FILE_DIR:BodyguardMod>
)
