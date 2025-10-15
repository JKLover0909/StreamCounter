cmake_minimum_required(VERSION 3.15)
project(StreamCounter LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(OpenCV REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBUSB REQUIRED libusb-1.0)

add_executable(usb_enum src/usb_enum.cpp)
target_include_directories(usb_enum PRIVATE ${LIBUSB_INCLUDE_DIRS})
target_link_libraries(usb_enum PRIVATE ${LIBUSB_LIBRARIES})

add_executable(camera_capture src/camera_capture.cpp)
target_link_libraries(camera_capture PRIVATE ${OpenCV_LIBS})