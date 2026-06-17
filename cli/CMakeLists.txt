add_executable(fdm-cli main.cpp)
target_link_libraries(fdm-cli PRIVATE fdm_engine CURL::libcurl)

install(TARGETS fdm-cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
