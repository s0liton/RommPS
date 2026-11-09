# The Steam Deck app: RommPS's screens (platform/ps5/app/rommps/examples/rommps)
# and PS5_VKHomebrewUI's kit, on SDL3 and the kit's OpenGL renderer. Built in
# the image of docker/ (tools/deck-build.sh):
#
#   cmake -S platform/steamdeck/app -B build/steamdeck/app -G Ninja && cmake --build build/steamdeck/app
#
# The program goes to build/steamdeck/app/rommdeck, the kit's fonts and sounds
# beside it in assets/.
#
# GPL-3.0-or-later, as RommPS and the kit are.
cmake_minimum_required(VERSION 3.20)
project(rommdeck C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE Release)
endif()

get_filename_component(REPO ${CMAKE_CURRENT_LIST_DIR}/../../.. ABSOLUTE)
set(RPS ${REPO}/platform/ps5/app/rommps)

# The kit, at the revision the PS5 app pins (its setup script exports it)
execute_process(
	COMMAND bash ${RPS}/ps5/ui/setup-kit.sh
	OUTPUT_VARIABLE KIT
	OUTPUT_STRIP_TRAILING_WHITESPACE
	RESULT_VARIABLE kit_result)
if(NOT kit_result EQUAL 0 OR NOT EXISTS ${KIT}/src/gfx/gl_renderer.cpp)
	message(FATAL_ERROR "ps5/ui/setup-kit.sh could not export the pinned PS5_VKHomebrewUI")
endif()
message(STATUS "UI kit: ${KIT}")

# The Deck's copy of the kit, with kit-deck.patch: the 16:10 canvas
# (HUI_VIRTUAL_HEIGHT), the Deck's button glyphs (HUI_DECK_GLYPHS) and
# DrawList::on_canvas for the touch screen's hit zones. The PS5's export
# stays as it came.
set(KIT_PATCH ${CMAKE_CURRENT_LIST_DIR}/kit-deck.patch)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${KIT_PATCH} ${KIT}/.revision)
file(READ ${KIT}/.revision kit_revision)
file(SHA256 ${KIT_PATCH} kit_patch_hash)
set(KIT_COPY ${CMAKE_BINARY_DIR}/kit)
set(kit_stamp "${kit_revision} ${kit_patch_hash}")
if(EXISTS ${KIT_COPY}/.stamp)
	file(READ ${KIT_COPY}/.stamp kit_copy_stamp)
endif()
if(NOT "${kit_copy_stamp}" STREQUAL "${kit_stamp}")
	file(REMOVE_RECURSE ${KIT_COPY})
	file(COPY ${KIT}/src DESTINATION ${KIT_COPY})
	execute_process(COMMAND patch -p1 --quiet -i ${KIT_PATCH} WORKING_DIRECTORY ${KIT_COPY} RESULT_VARIABLE patch_result)
	if(NOT patch_result EQUAL 0)
		message(FATAL_ERROR "kit-deck.patch doesn't apply to the kit at ${kit_revision}")
	endif()
	file(WRITE ${KIT_COPY}/.stamp "${kit_stamp}")
endif()

file(GLOB_RECURSE kit_sources ${KIT_COPY}/src/*.cpp ${KIT_COPY}/src/*.c)
# Not its console entry point and platform layer (src/ here stands in), and
# not its Vulkan backend: the Deck draws with the OpenGL one
list(FILTER kit_sources EXCLUDE REGEX "/src/(main\\.cpp|platform/|runtime/|gfx/vk/)")
set_source_files_properties(${kit_sources} PROPERTIES COMPILE_OPTIONS "-w")

file(GLOB screen_sources ${RPS}/examples/rommps/kit/*.cpp)
set_source_files_properties(${screen_sources} PROPERTIES
	COMPILE_OPTIONS "-include;${CMAKE_CURRENT_LIST_DIR}/src/deck_prelude.h")

add_executable(rommdeck
	src/main.cpp
	src/deck_kit.cpp
	src/deck_touch.cpp
	src/stb_image.cpp
	${kit_sources}
	${screen_sources}
	${REPO}/platform/ps5/app/src/rommps_api.cpp
	${REPO}/third_party/cJSON.c
	${REPO}/third_party/qrcodegen.c)
target_include_directories(rommdeck PRIVATE
	src
	${KIT_COPY}/src
	${RPS}/examples/rommps/kit
	${REPO}/platform/ps5/app/src
	${REPO}/third_party
	${RPS}/external/tinygltf)
target_compile_definitions(rommdeck PRIVATE ROMMPS_DECK GL_GLEXT_PROTOTYPES HUI_VIRTUAL_HEIGHT=1200.0f HUI_DECK_GLYPHS)
if(DEFINED ENV{BUILD_ID})
	target_compile_definitions(rommdeck PRIVATE ROMMPS_BUILD="$ENV{BUILD_ID}")
endif()

find_package(SDL3 REQUIRED CONFIG COMPONENTS SDL3-static)
find_package(OpenGL REQUIRED COMPONENTS OpenGL)
target_link_libraries(rommdeck PRIVATE SDL3::SDL3-static OpenGL::OpenGL Threads::Threads -static-libstdc++ -static-libgcc)
find_package(Threads REQUIRED)

# The kit's fonts and sounds, and RommPS's menu sounds, beside the program
add_custom_command(TARGET rommdeck POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:rommdeck>/assets
	COMMAND ${CMAKE_COMMAND} -E copy_directory ${KIT}/assets/fonts $<TARGET_FILE_DIR:rommdeck>/assets/fonts
	COMMAND ${CMAKE_COMMAND} -E copy_directory ${KIT}/assets/audio $<TARGET_FILE_DIR:rommdeck>/assets/audio
	# RommPS's menu sounds (used with permission, platform/ps5/app/rommps/ps5/SOUNDS-LICENCE.txt)
	COMMAND ${CMAKE_COMMAND} -E copy_directory ${REPO}/platform/ps5/app/assets/primary-menu-sounds $<TARGET_FILE_DIR:rommdeck>/assets/sounds)
