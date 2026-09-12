cmake_minimum_required(VERSION 3.25)

project(EnhancedNomad VERSION 1.0.0 LANGUAGES C CXX)

set(MINHOOK_SOURCE
	third_party/minhook/src/hde/hde64.c
	third_party/minhook/src/trampoline.c
	third_party/minhook/src/buffer.c
	third_party/minhook/src/hook.c
)

add_library(EnhancedNomad SHARED
	src/enhanced_nomad.cpp
	${MINHOOK_SOURCE}
)

target_compile_features(EnhancedNomad PRIVATE cxx_std_20)
target_include_directories(EnhancedNomad PRIVATE
	third_party/minhook/include
)

set(OUTPUT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../bin")

set_target_properties(EnhancedNomad PROPERTIES
	OUTPUT_NAME "EnhancedNomad"
	RUNTIME_OUTPUT_DIRECTORY "${OUTPUT_DIR}"
	RUNTIME_OUTPUT_DIRECTORY_DEBUG "${OUTPUT_DIR}"
	RUNTIME_OUTPUT_DIRECTORY_RELEASE "${OUTPUT_DIR}"
	RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${OUTPUT_DIR}"
	RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${OUTPUT_DIR}"
)

if(MSVC)
	set_source_files_properties(${MINHOOK_SOURCE} PROPERTIES COMPILE_OPTIONS "/wd4201;/wd4701;/wd4310;/wd4244")
	target_compile_options(EnhancedNomad PRIVATE /EHsc /permissive- /W4 /WX)
	set_property(TARGET EnhancedNomad PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
