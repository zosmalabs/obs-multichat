cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" ON)
option(ENABLE_QT "Use Qt functionality" ON)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)
find_package(CURL REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE CURL::libcurl)

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

if(ENABLE_QT)
  find_package(Qt6 REQUIRED COMPONENTS Widgets Core Network Multimedia)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets Qt6::Network Qt6::Multimedia)
  target_compile_options(
    ${CMAKE_PROJECT_NAME}
    PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
  )
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
  )

  if(OS_WINDOWS)
    # OBS does not guarantee that Qt Multimedia is included with its Qt runtime.
    install(FILES "$<TARGET_FILE:Qt6::Multimedia>" DESTINATION bin/64bit)
    get_target_property(_qt_multimedia_dll Qt6::Multimedia IMPORTED_LOCATION_RELEASE)
    if(_qt_multimedia_dll)
      get_filename_component(_qt_multimedia_bin "${_qt_multimedia_dll}" DIRECTORY)
      file(GLOB _qt_multimedia_plugins "${_qt_multimedia_bin}/../plugins/multimedia/*.dll")
      if(_qt_multimedia_plugins)
        install(FILES ${_qt_multimedia_plugins} DESTINATION bin/64bit/multimedia)
      endif()
    endif()
  endif()
endif()

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    src/plugin-main.cpp
    src/hidden-browser.h
    src/capture-script.h
    src/kick-client.cpp
    src/kick-client.h
    src/youtube-client.cpp
    src/youtube-client.h
    resources/notification.qrc
)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})
