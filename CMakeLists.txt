cmake_minimum_required(VERSION 3.16)
project(VaxMaps VERSION 0.9 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets)

qt_add_executable(VaxMaps WIN32
    src/main.cpp
    src/mainwindow.cpp src/mainwindow.h
    src/canvas.cpp src/canvas.h
    src/items.cpp src/items.h
    src/mapitem.cpp src/mapitem.h
    src/project.cpp src/project.h
    src/calibration.cpp src/calibration.h
    src/ui.cpp src/ui.h
    src/grid.cpp src/grid.h
    src/lang.cpp src/lang.h
    src/selftest.cpp
)
qt_add_resources(VaxMaps "icon" PREFIX "/" BASE icon FILES icon/vaxmaps.png)
if(WIN32)
    target_sources(VaxMaps PRIVATE icon/vaxmaps.rc)   # the .exe icon, also used for .vaxmap files by the installer
    set_property(SOURCE icon/vaxmaps.rc APPEND PROPERTY OBJECT_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/icon/vaxmaps.ico)
    target_include_directories(VaxMaps PRIVATE icon)
endif()
target_link_libraries(VaxMaps PRIVATE Qt6::Widgets Qt6::GuiPrivate Qt6::CorePrivate)   # QZipReader/QZipWriter for packages: QtGui private up to Qt 6.6, QtCore private after

# MinGW: link libgcc/libstdc++/winpthread in so the .exe runs without extra DLLs.
if(MINGW)
    target_link_options(VaxMaps PRIVATE -static)
endif()
