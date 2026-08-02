# Out-of-tree build of USD's hdEmbree reference render delegate against an
# installed USD SDK (e.g. the conda-forge openusd package, which ships
# pxrConfig.cmake but omits this plugin). Run fetch_hdembree.sh first to
# populate src/, then:
#
#   cmake -B build -G Ninja \
#     -DCMAKE_PREFIX_PATH="$CONDA_PREFIX" -DCMAKE_INSTALL_PREFIX="$CONDA_PREFIX"
#   cmake --build build && cmake --install build
#
# Installing into the USD prefix drops the plugin next to hdStorm under
# plugin/usd/, where the stock plugInfo.json ("Includes": ["*/resources/"])
# discovers it — no environment variables needed.
cmake_minimum_required(VERSION 3.20)
project(hdEmbreeOutOfTree LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/rendererPlugin.cpp)
  message(FATAL_ERROR "src/ is not populated — run ./fetch_hdembree.sh first")
endif()

# pxrTargets.cmake references imported targets (e.g. OpenGL::GL in garch's
# link interface) that the consuming project must create before including
# it — the same pre-find the hdmitsuba build does.
find_package(OpenGL REQUIRED)
find_package(pxr REQUIRED)
# Homebrew (`brew install embree`) and conda-forge both export the `embree`
# CMake target. USD >= 24.08 requires embree 4.
find_package(embree 4 REQUIRED)
find_package(TBB QUIET)

# Recursive: the plugin keeps sources in subdirectories too (pxrIES/ holds
# the IES light-profile support — PxrIESFile — whose omission surfaces only
# at dlopen time as an unresolved symbol, because the macOS linker defers
# undefined symbols). fetch_hdembree.sh removes testenv/, so everything
# under src/ belongs in the library.
file(GLOB_RECURSE HDEMBREE_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
add_library(hdEmbree SHARED ${HDEMBREE_SOURCES})

# The sources include their own headers by in-tree path
# ("pxr/imaging/plugin/hdEmbree/mesh.h"), which resolves against the
# OpenUSD repository root. Recreate that layout with a symlink so src/ can
# stay flat.
set(_shim ${CMAKE_CURRENT_BINARY_DIR}/pxr-include/pxr/imaging/plugin)
file(MAKE_DIRECTORY ${_shim})
if(NOT EXISTS ${_shim}/hdEmbree)
  file(CREATE_LINK ${CMAKE_CURRENT_SOURCE_DIR}/src ${_shim}/hdEmbree
       SYMBOLIC)
endif()
target_include_directories(hdEmbree PRIVATE
  ${CMAKE_CURRENT_BINARY_DIR}/pxr-include
)

# The defines pxr_plugin() would add for an in-tree build.
target_compile_definitions(hdEmbree PRIVATE
  MFB_PACKAGE_NAME=hdEmbree
  MFB_ALT_PACKAGE_NAME=hdEmbree
  MFB_PACKAGE_MODULE=hdEmbree
  HDEMBREE_EXPORTS=1
  NOMINMAX
)

target_include_directories(hdEmbree PRIVATE ${PXR_INCLUDE_DIRS})
# The in-tree plugin's dependency list (plus arch/sdf, which arrive
# transitively anyway). Deliberately NOT ${PXR_LIBRARIES}: linking every
# exported target pulls the full export closure (MaterialX, Vulkan, ...)
# into CMake's imported-target resolution, requiring find_package calls for
# packages the plugin never uses.
target_link_libraries(hdEmbree PRIVATE
  arch
  gf
  hd
  hf
  plug
  sdf
  tf
  vt
  work
  embree
)
if(TBB_FOUND)
  target_link_libraries(hdEmbree PRIVATE TBB::tbb)
endif()

# Match the installed-plugin naming convention (e.g. hdStorm.so /
# hdStorm.dylib — no "lib" prefix), which plugInfo.json's LibraryPath
# references. The install rpath lets the loaded plugin resolve its
# @rpath-installed dependencies (conda USD libraries, Homebrew embree)
# when dlopen'd by the Plug system.
set_target_properties(hdEmbree PROPERTIES
  PREFIX ""
  INSTALL_RPATH "${CMAKE_INSTALL_PREFIX}/lib;/opt/homebrew/lib;/usr/local/lib"
)

# The in-tree plugInfo.json carries configure-time placeholders; fill them
# the same way the USD build does for installed plugins (values mirrored
# from an installed hdStorm).
set(PLUG_INFO_ROOT "..")
set(PLUG_INFO_LIBRARY_PATH "../hdEmbree${CMAKE_SHARED_LIBRARY_SUFFIX}")
set(PLUG_INFO_RESOURCE_PATH "resources")
configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/src/plugInfo.json
  ${CMAKE_CURRENT_BINARY_DIR}/plugInfo.json
  @ONLY
)

install(TARGETS hdEmbree LIBRARY DESTINATION plugin/usd)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/plugInfo.json
        DESTINATION plugin/usd/hdEmbree/resources)
