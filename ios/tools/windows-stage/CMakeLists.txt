# Builds theft4_stage, the game-folder preparation tool, as a standalone Windows
# program. It compiles the same installer sources as the iOS game-tools graph
# (ios/CMakeLists.txt, REXGLUE_RUNTIME_ONLY) against the ReXGlue core and
# filesystem libraries, whose Windows backends the SDK already provides.
cmake_minimum_required(VERSION 3.25)
project(Theft4WindowsStage C CXX)

if(NOT WIN32)
    message(FATAL_ERROR "This project builds the Windows stager; Apple hosts use ios/CMakeLists.txt")
endif()

# Static CRT so the program runs without a Visual C++ redistributable install.
# The policy default reaches dependencies whose minimum CMake predates it.
set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

get_filename_component(THEFT4_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(REXGLUE_SDK "${THEFT4_ROOT}/glue/rexglue-sdk-main")

# The same SDK subset the iOS game tools build: core, filesystem and system
# headers, without renderer, UI or host tools.
set(REXGLUE_CORE_ONLY ON CACHE BOOL "" FORCE)
set(REXGLUE_RUNTIME_ONLY ON CACHE BOOL "" FORCE)
add_subdirectory("${REXGLUE_SDK}" rexglue)

# iOS links the system libz; Windows has none, so the build supplies a static one.
find_package(ZLIB REQUIRED)

# The SDK applies these as directory-scoped options, which do not reach here.
add_compile_options(
    -fno-strict-aliasing
    -ffp-model=strict
    $<$<COMPILE_LANGUAGE:CXX>:-fno-char8_t>
)

set(THEFT4_GAME_INSTALL_DIR "${REXGLUE_SDK}/gta4-recomp/src/install")
set(THEFT4_CRYPTO_DIR "${REXGLUE_SDK}/thirdparty/crypto")

add_library(theft4_game_source STATIC "${THEFT4_GAME_INSTALL_DIR}/gta4_source_inspector.cpp")
target_include_directories(theft4_game_source PUBLIC "${THEFT4_GAME_INSTALL_DIR}")
target_include_directories(theft4_game_source PRIVATE "${THEFT4_CRYPTO_DIR}")
target_link_libraries(theft4_game_source PUBLIC rex::filesystem PRIVATE xxHash::xxhash)
target_compile_features(theft4_game_source PRIVATE cxx_std_23)

add_library(theft4_game_install STATIC
    "${THEFT4_GAME_INSTALL_DIR}/gta4_installer.cpp"
    "${THEFT4_GAME_INSTALL_DIR}/gta4_rpf_extractor.cpp"
    "${THEFT4_CRYPTO_DIR}/rijndael-alg-fst.c")
target_link_libraries(theft4_game_install PUBLIC theft4_game_source PRIVATE xxHash::xxhash ZLIB::ZLIB)
target_include_directories(theft4_game_install PRIVATE "${THEFT4_CRYPTO_DIR}")
# The installer looks for aes_key.bin beside the program first. This relative
# fallback (the working directory) also keeps the builder's path out of the
# binary.
target_compile_definitions(theft4_game_install PRIVATE GTA4_RPF_AES_KEY_SOURCE="aes_key.bin")
target_compile_features(theft4_game_install PRIVATE cxx_std_23)

add_executable(theft4_stage "${THEFT4_ROOT}/ios/tools/stage_game.cpp")
target_link_libraries(theft4_stage PRIVATE theft4_game_install)
target_compile_features(theft4_stage PRIVATE cxx_std_23)
