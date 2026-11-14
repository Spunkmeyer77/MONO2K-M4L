cmake_minimum_required(VERSION 3.19)

# macOS: one universal binary that runs on macOS 10.13 and up (Apple Silicon and Intel). Set before project().
if (APPLE)
	set(CMAKE_OSX_DEPLOYMENT_TARGET "10.13" CACHE STRING "Minimum macOS" FORCE)
	if (NOT CMAKE_OSX_ARCHITECTURES)
		set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)
	endif ()
endif ()

# the version (VERSION at the root): the object is named after it, mono2k_122~ for 1.2.2, so a device always runs the
# code packed with it (Max keeps the first object of a name it loads for the whole session: an old frozen device could
# otherwise lend its old code to a new one)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../../VERSION" M2K_VERSION)
string(STRIP "${M2K_VERSION}" M2K_VERSION)
string(REPLACE "." "" M2K_VDIGITS "${M2K_VERSION}")
set(M2K_EXT_NAME "mono2k_${M2K_VDIGITS}~")
set(mono2k_tilde_EXTERN_OUTPUT_NAME "${M2K_EXT_NAME}" CACHE STRING "" FORCE)

set(MAX_SDK_BASE "${CMAKE_CURRENT_SOURCE_DIR}/../max-sdk-base" CACHE PATH "max-sdk-base checkout")
set(C74_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../../build/externals")
include(${MAX_SDK_BASE}/script/max-pretarget.cmake)

set(CORE "${CMAKE_CURRENT_SOURCE_DIR}/../../core")
include_directories("${C74_INCLUDES}" "${CORE}/src" "${CORE}/gen")

add_library(${PROJECT_NAME} MODULE
	mono2k~.c
	m2k_engine.c
	m2k_core_glue.c
)
set_property(TARGET ${PROJECT_NAME} PROPERTY C_STANDARD 11)
target_compile_definitions(${PROJECT_NAME} PRIVATE M2K_EXT_NAME="${M2K_EXT_NAME}" M2K_VERSION="${M2K_VERSION}")
if (MSVC)
	target_compile_options(${PROJECT_NAME} PRIVATE -Wno-unused-function -Wno-unused-variable)
else ()
	target_compile_options(${PROJECT_NAME} PRIVATE -O2 -Wno-unused-function)
endif ()

include(${MAX_SDK_BASE}/script/max-posttarget.cmake)
