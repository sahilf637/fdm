# Files installed only by `cmake --install` / the .deb build -- inert in a plain
# dev `cmake --build`. Puts the desktop entry, icons, and the browser
# native-messaging-host manifests in their system locations so the published
# extension connects to the installed host with no per-user setup script.

# Extension IDs the native host is allowed to talk to. Override at configure
# time once the add-ons are published, e.g.
#   -DFDM_FIREFOX_EXTENSION_ID={d324a09c-fb86-4cff-918a-54a1a1e4bf1a}
#   -DFDM_CHROME_EXTENSION_ID=<id from the Chrome Web Store>
set(FDM_FIREFOX_EXTENSION_ID "{d324a09c-fb86-4cff-918a-54a1a1e4bf1a}"
    CACHE STRING "Gecko ID of the published Firefox add-on (allowed_extensions).")
set(FDM_CHROME_EXTENSION_ID ""
    CACHE STRING "Chrome Web Store ID of the published extension (allowed_origins).")

# Final installed path of the host binary (baked into the manifests below).
set(FDM_NATIVE_HOST_PATH "${CMAKE_INSTALL_FULL_BINDIR}/fdm-native-host")

# Desktop entry + themed icons (Icon=fdm resolves against hicolor).
install(FILES fdm.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
foreach(sz 16 32 48 128 256)
  install(FILES ${CMAKE_SOURCE_DIR}/gui/icons/fdm-${sz}.png
          DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/${sz}x${sz}/apps
          RENAME fdm.png)
endforeach()

# Firefox host manifest -> /usr/lib/mozilla/native-messaging-hosts (NOT multiarch).
configure_file(native-host/firefox.json.in
               ${CMAKE_CURRENT_BINARY_DIR}/com.fdm.native_host.firefox.json @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/com.fdm.native_host.firefox.json
        DESTINATION lib/mozilla/native-messaging-hosts
        RENAME com.fdm.native_host.json)

# Chrome/Chromium host manifest -> /etc/opt/chrome and /etc/chromium. Only emit
# it once a real Web Store ID is known, so we never ship a dead placeholder.
if(FDM_CHROME_EXTENSION_ID)
  configure_file(native-host/chrome.json.in
                 ${CMAKE_CURRENT_BINARY_DIR}/com.fdm.native_host.chrome.json @ONLY)
  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/com.fdm.native_host.chrome.json
          DESTINATION /etc/opt/chrome/native-messaging-hosts
          RENAME com.fdm.native_host.json)
  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/com.fdm.native_host.chrome.json
          DESTINATION /etc/chromium/native-messaging-hosts
          RENAME com.fdm.native_host.json)
else()
  message(STATUS
    "FDM: FDM_CHROME_EXTENSION_ID unset -- Chrome/Chromium host manifest skipped.")
endif()
