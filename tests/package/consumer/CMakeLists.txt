cmake_minimum_required(VERSION 3.16)
project(DAGEPackageConsumer LANGUAGES CXX)

find_package(DAGE CONFIG REQUIRED)
add_executable(dage_package_consumer main.cpp)
target_compile_features(dage_package_consumer PRIVATE cxx_std_17)
target_link_libraries(dage_package_consumer PRIVATE DAGE::dage)
enable_testing()
add_test(NAME installed_dage_consumer COMMAND dage_package_consumer)
