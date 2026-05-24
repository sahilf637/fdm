set(CMAKE_AUTOMOC ON)

add_library(fdm_store STATIC
  src/Database.cpp
  src/DownloadManager.cpp
  src/DownloadListModel.cpp
  include/fdm/store/Database.h
  include/fdm/store/DownloadManager.h
  include/fdm/store/DownloadListModel.h
)

target_include_directories(fdm_store PUBLIC include)
target_link_libraries(fdm_store
  PUBLIC
    fdm_engine
    Qt6::Core
    Qt6::Sql
)
