# SPDX-FileCopyrightText: (C) 2024 Chris Rizzitello <sithlord48@gmail.com>
# SPDX-License-Identifier: MIT

# HACK This is set when the files is included so its the real path
# calling CMAKE_CURRENT_LIST_DIR after include would return the wrong scope var
set(MY_DIR ${CMAKE_CURRENT_LIST_DIR})
set(OSX_BUNDLE ${BUILD_OSX_BUNDLE})

set(OS_STRING "macos-${BUILD_ARCHITECTURE}")

if (OSX_BUNDLE)
  if (APPLE_CODESIGN_DEV)
    set(_codesign_args "--options runtime --timestamp --sign \"${APPLE_CODESIGN_DEV}\"")
  else()
    set(_codesign_args "--sign -")
  endif()
  install(CODE "
    set(_bundle \"\${CMAKE_INSTALL_PREFIX}/${CMAKE_PROJECT_PROPER_NAME}.app\")
    execute_process(
      COMMAND \"${DEPLOYQT}\" \"\${_bundle}\"
      RESULT_VARIABLE _deployqt_res
    )
    if(NOT \"\${_deployqt_res}\" STREQUAL \"0\")
      message(FATAL_ERROR \"macdeployqt failed: \${_deployqt_res}\")
    endif()

    execute_process(
      COMMAND codesign --force --deep ${_codesign_args} \"\${_bundle}\"
      RESULT_VARIABLE _codesign_res
    )
    if(NOT \"\${_codesign_res}\" STREQUAL \"0\")
      message(FATAL_ERROR \"Bundle signing failed: \${_codesign_res}\")
    endif()

    execute_process(
      COMMAND codesign --verify --deep --strict \"\${_bundle}\"
      RESULT_VARIABLE _verify_res
    )
    if(NOT \"\${_verify_res}\" STREQUAL \"0\")
      message(FATAL_ERROR \"Bundle signature verification failed: \${_verify_res}\")
    endif()
  ")
  set(CPACK_PACKAGE_ICON "${MY_DIR}/dmg-volume.icns")
  set(CPACK_DMG_BACKGROUND_IMAGE "${MY_DIR}/dmg-background.tiff")
  set(CPACK_DMG_DS_STORE_SETUP_SCRIPT "${MY_DIR}/generate_ds_store.applescript")
  set(CPACK_DMG_VOLUME_NAME "${CMAKE_PROJECT_PROPER_NAME}")
  set(CPACK_DMG_SLA_USE_RESOURCE_FILE_LICENSE ON)
  set(CPACK_GENERATOR "DragNDrop")
endif()
