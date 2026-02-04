cmake_minimum_required(VERSION 3.10)
project(SDR_GUI)

set(CMAKE_CXX_STANDARD 17)

# --- 1. Find Packages ---
find_package(UHD REQUIRED)
find_package(Qt5 COMPONENTS Widgets Charts Core REQUIRED)

# --- 2. Fix for UHD Include Paths ---
# This line is crucial for the "apt" version of UHD
include_directories(${UHD_INCLUDE_DIRS})

# --- 3. Create Executable ---
add_executable(usrp_viz main.cpp)

# --- 4. Link Libraries ---
# We use ${UHD_LIBRARIES} instead of UHD::uhd to be safe
target_link_libraries(usrp_viz 
    ${UHD_LIBRARIES}
    Qt5::Widgets 
    Qt5::Charts 
    Qt5::Core
)