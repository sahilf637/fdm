set(CMAKE_AUTOMOC ON)

add_library(fdm_store STATIC
  src/Database.cpp
  src/DownloadManager.cpp
  src/DownloadListModel.cpp
  src/SidecarFetcher.cpp
  src/SidecarFetcher.h
  include/fdm/store/Database.h
  include/fdm/store/DownloadManager.h
  include/fdm/store/DownloadListModel.h
)

target_include_directories(fdm_store PUBLIC include)
target_include_directories(fdm_store PRIVATE src)
target_link_libraries(fdm_store
  PUBLIC
    fdm_engine
    Qt6::Core
    Qt6::Sql
    Qt6::Network
    Qt6::Concurrent
)
