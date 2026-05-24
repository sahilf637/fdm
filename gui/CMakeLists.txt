set(CMAKE_AUTOMOC ON)

add_executable(fdm-gui
  main.cpp
  MainWindow.cpp
  MainWindow.h
  NewDownloadDialog.cpp
  NewDownloadDialog.h
  DownloadDetailsWindow.cpp
  DownloadDetailsWindow.h
)
target_link_libraries(fdm-gui PRIVATE fdm_store Qt6::Widgets)

# Copy the launcher wrapper next to the binary so users can run the GUI from
# any shell (including snap-confined ones that pollute LD_LIBRARY_PATH).
add_custom_command(
  TARGET fdm-gui POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy
    ${CMAKE_CURRENT_SOURCE_DIR}/fdm-gui.sh
    $<TARGET_FILE_DIR:fdm-gui>/fdm-gui.sh
  COMMAND chmod +x $<TARGET_FILE_DIR:fdm-gui>/fdm-gui.sh
  VERBATIM
)
