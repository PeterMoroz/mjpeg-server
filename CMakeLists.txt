project(MJPEGServer)
cmake_minimum_required(VERSION 2.8)

################# compiler config #################
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CXX_EXTENSIONS OFF)

################# source config #################
file(GLOB SRC_LIST "*.cpp")

################# create executable app #################
add_executable(${PROJECT_NAME} ${SRC_LIST})

################# link connect #################
# pthread
target_link_libraries(${PROJECT_NAME} pthread)

################# install application #################
install(TARGETS MJPEGServer RUNTIME DESTINATION bin)
