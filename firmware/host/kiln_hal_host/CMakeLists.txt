# SPDX-FileCopyrightText: 2026 Bitcrush Testing
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Host-only, and outside firmware/components/ for that reason: ESP-IDF treats
# every directory under components/ as part of the image, so a host adapter
# living there is a build failure waiting for whoever next runs idf.py.  Keeping
# it here makes the exclusion structural rather than a list someone has to
# remember to maintain -- and it is the honest arrangement anyway, since nothing
# here is ever linked into a target image (TR-10).
#
# These are the per-port fakes architecture 5.2 lists against each port, shared
# by the test suites and by any host-side harness.

add_library(kiln_hal_host STATIC src/hal_host.cpp)
target_include_directories(kiln_hal_host PUBLIC include)
target_link_libraries(kiln_hal_host PUBLIC kiln_ports)
set_target_properties(kiln_hal_host PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
