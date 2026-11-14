cmake_minimum_required(VERSION 3.19)

# macOS: one universal binary that runs on macOS 10.13 and up (Apple Silicon and Intel). Set before project().
if (APPLE)
	set(CMAKE_OSX_DEPLOYMENT_TARGET "10.13" CACHE STRING "Minimum macOS" FORCE)
	if (NOT CMAKE_OSX_ARCHITECTURES)
		set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)
	endif ()
endif ()

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
if (MSVC)
	target_compile_options(${PROJECT_NAME} PRIVATE -Wno-unused-function -Wno-unused-variable)
else ()
	target_compile_options(${PROJECT_NAME} PRIVATE -O2 -Wno-unused-function)
endif ()

include(${MAX_SDK_BASE}/script/max-posttarget.cmake)
