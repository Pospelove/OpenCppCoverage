cmake_minimum_required(VERSION 3.15)
project(OpenCppCoverage)
include(CMake/pmm.cmake)
include(CMake/FindDiaSDK.cmake)
set(CMAKE_CXX_STANDARD 17)
include_directories(${CMAKE_SOURCE_DIR})
set(vcpkg_ref 61e2cac730cc8b16c9373c6c463a335a21a1496a)

pmm(VCPKG
  REVISION ${vcpkg_ref}
  REQUIRES boost-log boost-program-options boost-uuid boost-iostreams boost-locale boost-thread poco protobuf gtest
)
find_package(Boost REQUIRED COMPONENTS log program_options iostreams locale thread)
find_package(Protobuf REQUIRED)

file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/CMake/ctemplate_patch/CMakeLists.txt DESTINATION ${_VCPKG_ROOT_DIR}/ports/ctemplate)
pmm(VCPKG
  REVISION ${vcpkg_ref}
  REQUIRES ctemplate
)

find_program(protoc_path NAMES protoc.exe HINTS ${_VCPKG_ROOT_DIR}/installded/${VCPKG_TARGET_TRIPLET}/tools/protobuf REQUIRED)

set(exporter_bin ${CMAKE_SOURCE_DIR}\\\\Exporter\\\\Binary)
add_custom_target(protoc COMMAND
  ${protoc_path}
  -I=\"${exporter_bin}\"
  --cpp_out=\"${exporter_bin}\"
  \"${exporter_bin}\\\\CoverageData.proto\"
)

# Components
set(libs Tools CppCoverage Plugin FileFilter Exporter)
foreach(lib ${libs})
  file(GLOB_RECURSE src ${CMAKE_CURRENT_LIST_DIR}/${lib}/*.[h,c]pp)
  add_library(${lib} STATIC ${src})
endforeach()

# ProtoBuff compiler output
add_library(coveragedata_pb STATIC
  ${CMAKE_CURRENT_LIST_DIR}/Exporter/Binary/CoverageData.pb.cc
)
target_include_directories(coveragedata_pb PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/Exporter/Binary
  ${Protobuf_INCLUDE_DIRS}
)

# OpenCppCoverage
file(GLOB_RECURSE opencppcov_src ${CMAKE_CURRENT_LIST_DIR}/OpenCppCoverage/*.[h,c]pp)
add_executable(OpenCppCoverage ${opencppcov_src})

add_custom_target(copy_template
  COMMAND ${CMAKE_COMMAND} -E remove_directory \"$<TARGET_FILE_DIR:CppCoverage>/Plugins\"
  COMMAND ${CMAKE_COMMAND} -E make_directory \"$<TARGET_FILE_DIR:CppCoverage>/Plugins/Exporter\"
  COMMAND ${CMAKE_COMMAND} -E copy_directory \"${CMAKE_CURRENT_LIST_DIR}/Exporter/Html/Template\" \"$<TARGET_FILE_DIR:CppCoverage>/Template\"
)

add_custom_target(copy_msdia_dll
  COMMAND ${CMAKE_COMMAND} -E copy \"${CMAKE_CURRENT_LIST_DIR}/CppCoverage/Dia/x86/msdia140.dll\" \"$<TARGET_FILE_DIR:CppCoverage>\"
)

set(all ${libs})
list(APPEND all OpenCppCoverage)

foreach(tgt ${all})
  target_link_libraries(${tgt} PRIVATE ${Boost_LIBRARIES})
  target_include_directories(${tgt} PRIVATE ${Boost_INCLUDE_DIRS})

  target_link_libraries(${tgt} PRIVATE ${DIASDK_LIBRARIES})
  target_include_directories(${tgt} PRIVATE ${DIASDK_INCLUDE_DIRS})

  target_compile_definitions(${tgt} PRIVATE
    CPPCOVERAGE_EXPORTS
    TOOLS_EXPORTS
    PLUGIN_EXPORTS
    FILEFILTER_EXPORTS
    EXPORTER_EXPORTS
    UNICODE
    _UNICODE
    NOMINMAX
    OPENCPPCOVERAGE_VERSION=\"0.9.8.0\"
  )

  target_include_directories(${tgt} PRIVATE ${CMAKE_CURRENT_LIST_DIR}/${tgt})

  target_link_libraries(${tgt} PRIVATE Dbghelp.lib bcrypt.lib)

  target_link_libraries(${tgt} PRIVATE
    ${Protobuf_LIBRARY} ${Protobuf_LIBRARY_DEBUG} coveragedata_pb
  )

  target_include_directories(${tgt} PRIVATE ${Protobuf_INCLUDE_DIRS})

  target_link_libraries(${tgt} PRIVATE
    optimized ${_VCPKG_ROOT_DIR}/installed/${VCPKG_TARGET_TRIPLET}/lib/libctemplate.lib
    debug ${_VCPKG_ROOT_DIR}/installed/${VCPKG_TARGET_TRIPLET}/debug/lib/libctemplate.lib
  )
endforeach()

target_link_libraries(OpenCppCoverage PRIVATE ${libs})
add_dependencies(OpenCppCoverage copy_template copy_msdia_dll)

add_dependencies(coveragedata_pb protoc)
add_dependencies(Exporter coveragedata_pb)

# Make release
set(output_file ${CMAKE_BINARY_DIR}/OpenCppCoverage.zip)
set(input_files ${CMAKE_BINARY_DIR}/$<CONFIG>)
add_custom_target(pack COMMAND
  ${CMAKE_COMMAND} -E tar "cf" "${output_file}" --format=zip -- ${input_files}
)
add_dependencies(pack OpenCppCoverage)
