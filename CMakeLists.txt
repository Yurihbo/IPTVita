cmake_minimum_required(VERSION 3.10)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE FILEPATH "VitaSDK toolchain")
  else()
    message(FATAL_ERROR "Set VITASDK to the installed VitaSDK directory.")
  endif()
endif()

project(IPTVita C)
include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)
find_package(CURL REQUIRED)
find_package(OpenSSL REQUIRED)

set(VITA_APP_NAME "IPTVita")
set(VITA_TITLEID "IPTVI0001")
set(VITA_VERSION "00.56")

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -O2)

add_executable(IPTVita.elf
  src/main.c
  src/ui.c
  src/input.c
  src/sources.c
  src/keyboard.c
  src/playlist.c
  src/network.c
)

target_include_directories(IPTVita.elf PRIVATE src)
file(GLOB VITA_EXTRA_STUBS
  "$ENV{VITASDK}/arm-vita-eabi/lib/libSceSharedFb*_stub.a"
  "$ENV{VITASDK}/arm-vita-eabi/lib/libSceAppMgr_stub.a"
)

target_link_libraries(IPTVita.elf
  vita2d
  SceCtrl_stub
  SceTouch_stub
  ScePgf_stub
  SceIme_stub
  SceIofilemgr_stub
  SceNet_stub
  SceNetCtl_stub
  SceHttp_stub
  SceSsl_stub
  SceDisplay_stub
  SceGxm_stub
  SceSysmodule_stub
  SceCommonDialog_stub
  ${VITA_EXTRA_STUBS}
  ${CURL_LIBRARIES}
  ${OPENSSL_LIBRARIES}
  z
  pthread
  m
)

vita_create_self(IPTVita.self IPTVita.elf)
vita_create_vpk(IPTVita.vpk ${VITA_TITLEID} IPTVita.self
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
  FILE LICENSE LICENSE.txt
  FILE assets/cacert.pem cacert.pem
)
