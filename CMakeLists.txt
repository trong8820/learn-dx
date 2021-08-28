cmake_minimum_required (VERSION 3.16)

project ("learn-dx" VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED OFF)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_UNITY_BUILD ON)

# 3rdparty include
include_directories (
	${CMAKE_SOURCE_DIR}/3rdparty/glfw-3.3.4/include
)

# 3rdparty sources
file(GLOB_RECURSE 3RDPARTY_SOURCE_FILES
	${CMAKE_SOURCE_DIR}/3rdparty/*.c
	${CMAKE_SOURCE_DIR}/3rdparty/*.cc
	${CMAKE_SOURCE_DIR}/3rdparty/*.cpp
)

# 3rdparty libs
link_directories(
	${CMAKE_SOURCE_DIR}/3rdparty/glfw-3.3.4/lib/windows
)

# 3rdparty link
link_libraries(glfw3 d3d12.lib dxgi.lib dxguid.lib windowsapp.lib)

# Projects
include_directories(
	 ${CMAKE_SOURCE_DIR}/include
)

set(SOURCE_FILES
	${CMAKE_CURRENT_SOURCE_DIR}/src/entry.cpp
)

# List projects
add_executable(${PROJECT_NAME}_01 ${3RDPARTY_SOURCE_FILES} ${SOURCE_FILES} ${CMAKE_SOURCE_DIR}/src/learn_dx_01.cpp)