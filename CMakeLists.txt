cmake_minimum_required(VERSION 3.16)
project(VoxelSpaceSDL LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

# Default single-config generators (Make/Ninja) to Release for runtime performance.
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(VOXEL_USE_SDL_IMAGE "Build with SDL2_image support" OFF)
option(VOXEL_USE_OVERLAY "Build with overlay module (SDL2_ttf)" OFF)
option(VOXEL_USE_THREADED_RENDER "Build with threaded renderer" ON)
option(VOXEL_USE_AVX2 "Build with AVX2 optimizations" ON)
option(VOXEL_WARNINGS_EXTRA "Enable extra compiler warnings" OFF)

file(GLOB VOXEL_CORE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c")
set(VOXEL_MODULE_SOURCES)

if(NOT EMSCRIPTEN)
	list(APPEND VOXEL_MODULE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/modules/dragndrop.c")
endif()

if(VOXEL_USE_OVERLAY)
	list(APPEND VOXEL_MODULE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/modules/overlay.c")
endif()

add_executable(vs ${VOXEL_CORE_SOURCES} ${VOXEL_MODULE_SOURCES})
target_include_directories(vs PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

if(EMSCRIPTEN)
	target_compile_options(vs PRIVATE
		"SHELL:-s USE_SDL=2"
	)
	target_link_options(vs PRIVATE
		"SHELL:-s USE_SDL=2"
		"SHELL:-s SINGLE_FILE=1"
		"SHELL:-s ALLOW_MEMORY_GROWTH=1"
		"SHELL:--preload-file ${CMAKE_CURRENT_SOURCE_DIR}/maps/h1w.bmp@maps/h1w.bmp"
		"SHELL:--preload-file ${CMAKE_CURRENT_SOURCE_DIR}/maps/h1.bmp@maps/h1.bmp"
	)
	set_target_properties(vs PROPERTIES SUFFIX ".html")
else()
	find_package(PkgConfig REQUIRED)
	pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
	target_link_libraries(vs PRIVATE PkgConfig::SDL2)
	find_package(OpenGL REQUIRED)
	target_compile_definitions(vs PRIVATE USE_OPENGL_RENDER)
	target_link_libraries(vs PRIVATE OpenGL::GL)
endif()

if(VOXEL_USE_SDL_IMAGE)
	target_compile_definitions(vs PRIVATE USE_SDL_IMAGE)
	if(EMSCRIPTEN)
		target_link_options(vs PRIVATE
			"SHELL:-s USE_SDL_IMAGE=2"
			"SHELL:-s SDL2_IMAGE_FORMATS=['png','bmp']"
		)
	else()
		pkg_check_modules(SDL2_IMAGE REQUIRED IMPORTED_TARGET SDL2_image)
		target_link_libraries(vs PRIVATE PkgConfig::SDL2_IMAGE)
	endif()
endif()

if(VOXEL_USE_OVERLAY)
	target_compile_definitions(vs PRIVATE USE_SDL_TTF)
	if(EMSCRIPTEN)
		target_link_options(vs PRIVATE
			"SHELL:-s USE_SDL_TTF=2"
			"SHELL:--preload-file ${CMAKE_CURRENT_SOURCE_DIR}/out/Roboto-Regular.ttf@Roboto-Regular.ttf"
		)
	else()
		pkg_check_modules(SDL2_TTF REQUIRED IMPORTED_TARGET SDL2_ttf)
		target_link_libraries(vs PRIVATE PkgConfig::SDL2_TTF)
	endif()
endif()

if(VOXEL_USE_THREADED_RENDER)
	target_compile_definitions(vs PRIVATE USE_THREADED_RENDER)
endif()

if(VOXEL_USE_AVX2 AND NOT EMSCRIPTEN)
	target_compile_definitions(vs PRIVATE USE_AVX2)
	if(MSVC)
		target_compile_options(vs PRIVATE /arch:AVX2)
	else()
		target_compile_options(vs PRIVATE -mavx2)
	endif()
endif()

if(VOXEL_WARNINGS_EXTRA)
	if(MSVC)
		target_compile_options(vs PRIVATE /W4)
	else()
		target_compile_options(vs PRIVATE -Wextra)
	endif()
endif()

set(VOXEL_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set_target_properties(vs PROPERTIES
	RUNTIME_OUTPUT_DIRECTORY "${VOXEL_RUNTIME_OUTPUT_DIRECTORY}"
	RUNTIME_OUTPUT_DIRECTORY_DEBUG "${VOXEL_RUNTIME_OUTPUT_DIRECTORY}"
	RUNTIME_OUTPUT_DIRECTORY_RELEASE "${VOXEL_RUNTIME_OUTPUT_DIRECTORY}"
	RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${VOXEL_RUNTIME_OUTPUT_DIRECTORY}"
	RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${VOXEL_RUNTIME_OUTPUT_DIRECTORY}"
)

add_custom_command(TARGET vs POST_BUILD
	COMMAND "${CMAKE_COMMAND}" -E copy_directory
		"${CMAKE_CURRENT_SOURCE_DIR}/maps"
		"$<TARGET_FILE_DIR:vs>/maps"
	COMMENT "Copying map assets beside the executable"
)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/out/Roboto-Regular.ttf")
	add_custom_command(TARGET vs POST_BUILD
		COMMAND "${CMAKE_COMMAND}" -E copy_if_different
			"${CMAKE_CURRENT_SOURCE_DIR}/out/Roboto-Regular.ttf"
			"$<TARGET_FILE_DIR:vs>/Roboto-Regular.ttf"
	)
endif()
