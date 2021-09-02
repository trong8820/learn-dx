cmake_minimum_required (VERSION 3.16)

project ("learn-dx" VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED OFF)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_UNITY_BUILD ON)

add_compile_definitions(SPNG_STATIC NOMINMAX)

# 3rdparty include
include_directories (
	${CMAKE_SOURCE_DIR}/3rdparty/glfw-3.3.4/include
	${CMAKE_SOURCE_DIR}/3rdparty/libspng-0.6.3/include
	${CMAKE_SOURCE_DIR}/3rdparty/zlib-1.2.11/windows/include
)

# 3rdparty sources
#file(GLOB_RECURSE 3RDPARTY_SOURCE_FILES
#	${CMAKE_SOURCE_DIR}/3rdparty/*.c
#	${CMAKE_SOURCE_DIR}/3rdparty/*.cc
#	${CMAKE_SOURCE_DIR}/3rdparty/*.cpp
#)

# 3rdparty libs
link_directories(
	${CMAKE_SOURCE_DIR}/3rdparty/glfw-3.3.4/lib/windows
	${CMAKE_SOURCE_DIR}/3rdparty/libspng-0.6.3/lib/windows
	${CMAKE_SOURCE_DIR}/3rdparty/zlib-1.2.11/windows/lib
)

# 3rdparty link
link_libraries(glfw3 spng_static.lib zlibstaticd.lib d3d12.lib d3dcompiler.lib dxgi.lib dxguid.lib windowsapp.lib)

# Projects
include_directories(
	 ${CMAKE_SOURCE_DIR}/include
)

set(SOURCE_FILES
	${CMAKE_CURRENT_SOURCE_DIR}/src/entry.cpp
)

# List projects
#add_executable(${PROJECT_NAME}_01 ${3RDPARTY_SOURCE_FILES} ${SOURCE_FILES} ${CMAKE_SOURCE_DIR}/src/learn_dx_01.cpp)
#add_executable(${PROJECT_NAME}_02 ${3RDPARTY_SOURCE_FILES} ${SOURCE_FILES} ${CMAKE_SOURCE_DIR}/src/learn_dx_02.cpp)
#add_executable(${PROJECT_NAME}_03 ${3RDPARTY_SOURCE_FILES} ${SOURCE_FILES} ${CMAKE_SOURCE_DIR}/src/learn_dx_03.cpp)
#add_executable(${PROJECT_NAME}_04 ${3RDPARTY_SOURCE_FILES} ${SOURCE_FILES} ${CMAKE_SOURCE_DIR}/src/learn_dx_04.cpp)
#add_executable(${PROJECT_NAME}_05 ${3RDPARTY_SOURCE_FILES} ${SOURCE_FILES} ${CMAKE_SOURCE_DIR}/src/learn_dx_05.cpp)
#add_executable(${PROJECT_NAME}_06 ${3RDPARTY_SOURCE_FILES} ${SOURCE_FILES} ${CMAKE_SOURCE_DIR}/src/learn_dx_06.cpp)
add_executable(${PROJECT_NAME}_07 ${3RDPARTY_SOURCE_FILES} ${SOURCE_FILES} ${CMAKE_SOURCE_DIR}/src/learn_dx_07.cpp)

#add_custom_command(TARGET  ${PROJECT_NAME}_05 PRE_BUILD
#				   COMMAND ${CMAKE_COMMAND} -E copy_directory
#				   ${CMAKE_SOURCE_DIR}/data $<TARGET_FILE_DIR:${PROJECT_NAME}_05>/data
#)