cmake_minimum_required(VERSION 3.16)
project(VaxMaps VERSION 0.5.1 LANGUAGES CXX)

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
target_link_libraries(VaxMaps PRIVATE Qt6::Widgets)

# MinGW: link libgcc/libstdc++/winpthread in so the .exe runs without extra DLLs.
if(MINGW)
    target_link_options(VaxMaps PRIVATE -static)
endif()
