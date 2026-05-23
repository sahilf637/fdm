add_executable(fdm-cli main.cpp)
target_link_libraries(fdm-cli PRIVATE fdm_engine CURL::libcurl)
