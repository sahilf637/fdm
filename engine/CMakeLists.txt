add_library(fdm_engine STATIC
  src/DownloadEngine.cpp
  src/ChunkMath.cpp
  src/ChunkTask.cpp
  src/Paths.cpp
)

target_include_directories(fdm_engine PUBLIC include)
target_compile_definitions(fdm_engine PRIVATE _FILE_OFFSET_BITS=64)
target_link_libraries(fdm_engine PUBLIC CURL::libcurl)
