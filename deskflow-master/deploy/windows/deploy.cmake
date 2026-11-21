# SPDX-FileCopyrightText: (C) 2024 Chris Rizzitello <sithlord48@gmail.com>
# SPDX-License-Identifier: MIT

# HACK This is set when the files is included so its the real path
# calling CMAKE_CURRENT_LIST_DIR after include would return the wrong scope var
set(MY_DIR ${CMAKE_CURRENT_LIST_DIR})

set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP TRUE)
set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION ${CMAKE_INSTALL_LIBDIR})
include(InstallRequiredSystemLibraries)

set(OS_STRING "win-${BUILD_ARCHITECTURE}")

list(APPEND CPACK_GENERATOR "ZIP")

# 只有提供完整的文件传输组件时才生成安装版。
set(DESKSEND_LOCALSEND_CLI "" CACHE FILEPATH "Path to the built Windows localsend-cli.exe for the DeskSend MSI")
if (DESKSEND_LOCALSEND_CLI)
  file(TO_CMAKE_PATH "${DESKSEND_LOCALSEND_CLI}" DESKSEND_LOCALSEND_CLI)
  if (CMAKE_VERSION VERSION_LESS 3.30)
    message(FATAL_ERROR "The DeskSend MSI requires CMake 3.30 or newer")
  endif()
  if (NOT EXISTS "${DESKSEND_LOCALSEND_CLI}" OR IS_DIRECTORY "${DESKSEND_LOCALSEND_CLI}")
    message(FATAL_ERROR "DESKSEND_LOCALSEND_CLI must point to the built localsend-cli.exe")
  endif()
  find_program(WIX_APP wix REQUIRED)
  set(CPACK_WIX_VERSION 4)
  set(CPACK_WIX_ARCHITECTURE ${BUILD_ARCHITECTURE})
  list(APPEND CPACK_GENERATOR "WIX")
endif()

configure_file(${MY_DIR}/pre-cpack.cmake.in ${CMAKE_CURRENT_BINARY_DIR}/pre-cpack.cmake @ONLY)
set(CPACK_PRE_BUILD_SCRIPTS ${CMAKE_CURRENT_BINARY_DIR}/pre-cpack.cmake)

configure_file(${MY_DIR}/cpack-options.cmake.in ${CMAKE_CURRENT_BINARY_DIR}/cpack-options.cmake @ONLY)
set(CPACK_PROJECT_CONFIG_FILE ${CMAKE_CURRENT_BINARY_DIR}/cpack-options.cmake)

set(CPACK_PACKAGE_NAME "${CMAKE_PROJECT_PROPER_NAME}")

# Menu Entry
set(CPACK_WIX_PROGRAM_MENU_FOLDER "${CMAKE_PROJECT_PROPER_NAME}")
set(CPACK_PACKAGE_EXECUTABLES "DeskSend" "${CMAKE_PROJECT_PROPER_NAME}")

# Default Install Path
set(CPACK_PACKAGE_INSTALL_DIRECTORY "${CMAKE_PROJECT_PROPER_NAME}")

# Wix Specific Values
set(CPACK_WIX_UPGRADE_GUID "136412A6-25B4-4E92-A39A-5807B1F71217")
set(CPACK_WIX_PRODUCT_ICON "${CMAKE_SOURCE_DIR}/src/apps/res/branding/desksend.ico")
set(CPACK_WIX_UI_BANNER "${MY_DIR}/wix-banner.png")
set(CPACK_WIX_UI_DIALOG "${MY_DIR}/wix-dialog.png")
set(CPACK_WIX_UI_REF "WixUI_Minimal")

# 生成面向 DeskSend MSI 的完整许可证文本（纯 ASCII，避免 WiX RTF 转换乱码）
set(_desksend_wix_license "${CMAKE_CURRENT_BINARY_DIR}/DESKSEND-LICENSE.txt")
file(READ "${CMAKE_SOURCE_DIR}/LICENSE" _desksend_gpl_license)
file(READ "${CMAKE_SOURCE_DIR}/LICENSES/LicenseRef-OpenSSL-Exception.txt" _desksend_openssl_exception)

set(_desksend_license_text
"DeskSend - Keyboard, Mouse, and File Sharing
========================================================================

DeskSend bundles and integrates the following open-source software:

1. DeskSend Desktop Application & Core (based on Deskflow)
   License: GNU General Public License, Version 2 (GPL-2.0-only)
            with OpenSSL Linking Exception (LicenseRef-OpenSSL-Exception)

2. LocalSend File Transfer Component (localsend-cli)
   License: Apache License, Version 2.0 (Apache-2.0)

Source code: https://github.com/wtyLaziness/desksend
Releases:    https://github.com/wtyLaziness/desksend-releases

The complete license texts for all bundled components are included below
and are also installed in the DeskSend installation directory
(LICENSE, LICENSE_EXCEPTION, and LocalSend-LICENSE.txt).

========================================================================
PART 1: GNU GENERAL PUBLIC LICENSE, VERSION 2
========================================================================

${_desksend_gpl_license}

========================================================================
PART 2: LICENSE EXCEPTION FOR OPENSSL
========================================================================

${_desksend_openssl_exception}
")

set(_desksend_localsend_license_file "${CMAKE_SOURCE_DIR}/../localsend-main/LICENSE")
if(EXISTS "${_desksend_localsend_license_file}")
  file(READ "${_desksend_localsend_license_file}" _desksend_localsend_license)
  string(APPEND _desksend_license_text
"
========================================================================
PART 3: APACHE LICENSE, VERSION 2.0 (LOCALSEND COMPONENT)
========================================================================

${_desksend_localsend_license}
")
elseif(DESKSEND_LOCALSEND_CLI)
  message(FATAL_ERROR "Missing LocalSend license file required for DeskSend MSI: ${_desksend_localsend_license_file}")
endif()

file(WRITE "${_desksend_wix_license}" "${_desksend_license_text}")
set(CPACK_RESOURCE_FILE_LICENSE "${_desksend_wix_license}")

# Required Extra Extenstions
list(APPEND CPACK_WIX_EXTENSIONS "WixToolset.Util.wixext" "WixToolset.Firewall.wixext")

# Make sure to also put the xmlns for the ext into the wix block on generated files
list(APPEND CPACK_WIX_CUSTOM_XMLNS "util=http://wixtoolset.org/schemas/v4/wxs/util" "firewall=http://wixtoolset.org/schemas/v4/wxs/firewall")

configure_file(
  ${MY_DIR}/wix-patch.xml.in
  ${CMAKE_CURRENT_BINARY_DIR}/wix-patch.xml @ONLY
)

# Windows 服务和防火墙规则由 MSI 管理。
set(CPACK_WIX_PATCH_FILE "${CMAKE_CURRENT_BINARY_DIR}/wix-patch.xml")
