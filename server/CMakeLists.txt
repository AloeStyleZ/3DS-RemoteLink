cmake_minimum_required(VERSION 3.20)
project(threeds_stream_server CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- Opciones -----------------------------------------------------------------
# Nucleo (captura+video+input log) compila solo con el Windows SDK.
option(USE_VIGEM     "Inyectar input como gamepad Xbox 360 virtual (ViGEmClient)" OFF)
option(USE_TURBOJPEG "Habilitar codec MJPEG (libjpeg-turbo)"                       OFF)

add_executable(server
    src/main.cpp
    src/capture.cpp
    src/convert.cpp
    src/video.cpp
    src/input.cpp)

target_include_directories(server PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared)

# Windows SDK: Direct3D 11, DXGI, Winsock.
target_link_libraries(server PRIVATE d3d11 dxgi ws2_32)

if (MSVC)
    target_compile_options(server PRIVATE /O2 /W3)
    target_compile_definitions(server PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
endif()

# --- ViGEm (opcional) ---------------------------------------------------------
# ViGEmClient se compila directamente desde third_party/ (clonado del repo).
# En runtime necesita el driver ViGEmBus instalado (si falta, el servidor arranca
# igual y solo avisa de que ViGEm no esta disponible).
if (USE_VIGEM)
    set(VIGEM_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/ViGEmClient)
    target_sources(server PRIVATE ${VIGEM_DIR}/src/ViGEmClient.cpp)
    target_include_directories(server PRIVATE ${VIGEM_DIR}/include)
    target_link_libraries(server PRIVATE setupapi)
    target_compile_definitions(server PRIVATE HAVE_VIGEM)
endif()

# --- libjpeg-turbo (opcional) -------------------------------------------------
# vcpkg install libjpeg-turbo
if (USE_TURBOJPEG)
    find_package(libjpeg-turbo CONFIG REQUIRED)
    target_link_libraries(server PRIVATE libjpeg-turbo::turbojpeg)
    target_compile_definitions(server PRIVATE HAVE_TURBOJPEG)
endif()
