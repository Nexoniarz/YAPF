# YAPF thumbnails for KDE Dolphin (KF6 KIO thumbnail plugin).
#   cmake -B build -DCMAKE_BUILD_TYPE=Release
#   cmake --build build
#   sudo cmake --install build        (or see README.md for a per-user install)
cmake_minimum_required(VERSION 3.16)
project(yapfthumbnail LANGUAGES C CXX)

find_package(ECM 6.0 REQUIRED NO_MODULE)
set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH})
include(KDEInstallDirs)
include(KDECMakeSettings)
include(KDECompilerSettings NO_POLICY_SCOPE)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui)
find_package(KF6 6.0 REQUIRED COMPONENTS KIO)
find_package(Threads REQUIRED)

kcoreaddons_add_plugin(yapfthumbnail
    SOURCES yapfthumbnail.cpp ../../yapf.c
    INSTALL_NAMESPACE "kf6/thumbcreator")
set_source_files_properties(../../yapf.c PROPERTIES LANGUAGE C)
target_include_directories(yapfthumbnail PRIVATE ../..)
target_link_libraries(yapfthumbnail PRIVATE KF6::KIOGui Qt6::Gui Threads::Threads)

# Register the image/x-yapf file type so Dolphin recognises .yapf files.
install(FILES ../linux/yapf-mime.xml DESTINATION ${KDE_INSTALL_MIMEDIR} RENAME yapf.xml)
