# Copyright 2026 Google LLC
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# usd_imaging_mitsuba/CMakeLists.txt
#
# UsdImaging-side plugin (Hydra 2.0): a keyless API schema adapter that
# publishes custom `mitsuba:*` USD attributes into the stage scene index.
# This library is discovered through PXR_PLUGINPATH_NAME by any USD imaging
# application (usdview, usdrecord, the bundled render engine, ...).

add_library(usdImagingMitsuba SHARED
  adapter.cc
)

target_link_libraries(usdImagingMitsuba
  PRIVATE
    tf
    vt
    sdf
    usd
    usdGeom
    usdLux
    usdShade
    hd
    usdImaging
)

set_target_properties(usdImagingMitsuba PROPERTIES
  INSTALL_RPATH "${PLATFORM_ORIGIN}/../../lib"
)

# Copy plugInfo.json to the build directory for runtime plugin discovery.
set(PLUG_INFO_LIBRARY_PATH "libusdImagingMitsuba${CMAKE_SHARED_LIBRARY_SUFFIX}")
configure_file(plugInfo.json.in plugInfo.json @ONLY)

# --- Installation Rules ---
install(TARGETS usdImagingMitsuba
  LIBRARY DESTINATION plugin/usd_imaging_mitsuba
  ARCHIVE DESTINATION plugin/usd_imaging_mitsuba
  RUNTIME DESTINATION plugin/usd_imaging_mitsuba
)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/plugInfo.json
  DESTINATION plugin/usd_imaging_mitsuba
)
