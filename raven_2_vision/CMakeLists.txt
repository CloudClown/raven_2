cmake_minimum_required(VERSION 2.4.6)
include($ENV{ROS_ROOT}/core/rosbuild/rosbuild.cmake)

# Set the build type.  Options are:
#  Coverage       : w/ debug symbols, w/o optimization, w/ code-coverage
#  Debug          : w/ debug symbols, w/o optimization
#  Release        : w/o debug symbols, w/ optimization
#  RelWithDebInfo : w/ debug symbols, w/ optimization
#  MinSizeRel     : w/o debug symbols, w/ optimization, stripped binaries
#set(ROS_BUILD_TYPE RelWithDebInfo)

rosbuild_init()

#set the default path for built executables to the "bin" directory
set(EXECUTABLE_OUTPUT_PATH ${PROJECT_SOURCE_DIR}/bin)
#set the default path for built libraries to the "lib" directory
set(LIBRARY_OUTPUT_PATH ${PROJECT_SOURCE_DIR}/lib)

#uncomment if you have defined messages
#rosbuild_genmsg()
#uncomment if you have defined services
#rosbuild_gensrv()

add_definitions("-DRAVEN_VISION_DATA_DIR=${CMAKE_SOURCE_DIR}/data")

#initialize boost directory search
rosbuild_add_boost_directories()

#common commands for building c++ executables and libraries
rosbuild_add_library(${PROJECT_NAME} src/raven_vision.cpp src/config.cpp)
target_link_libraries(${PROJECT_NAME} boost_program_options)
#rosbuild_add_boost_directories()
#rosbuild_link_boost(${PROJECT_NAME} thread)

rosbuild_add_executable(test_capture src/test_capture.cpp)
target_link_libraries(test_capture ${PROJECT_NAME})

rosbuild_add_executable(intrinsic_calibration src/intrinsic_calibration.cpp)
rosbuild_add_executable(extrinsic_calibration src/extrinsic_calibration.cpp)

target_link_libraries(extrinsic_calibration ${PROJECT_NAME})

rosbuild_add_executable(chessboard_tracker src/chessboard_tracker_node.cpp)
target_link_libraries(chessboard_tracker ${PROJECT_NAME})

rosbuild_add_executable(stereo_error_characterizer src/stereo_error_characterizer_node.cpp)
rosbuild_link_boost(stereo_error_characterizer thread system signals)
target_link_libraries(stereo_error_characterizer ${PROJECT_NAME})
