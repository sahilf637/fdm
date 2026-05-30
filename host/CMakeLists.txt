add_executable(fdm-native-host
  main.cpp
)
target_include_directories(fdm-native-host PRIVATE ${CMAKE_SOURCE_DIR}/ipc)
target_link_libraries(fdm-native-host PRIVATE Qt6::Core Qt6::Network)
