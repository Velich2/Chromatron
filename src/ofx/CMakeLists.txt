# OpenFX plug-in bundle: Chromatron.ofx.bundle/Contents/<arch>/Chromatron.ofx

set(OFX_SDK_DIR "" CACHE PATH "Path to an OpenFX checkout (empty = download OFX_Release_1.5.1)")
if(NOT OFX_SDK_DIR)
    include(FetchContent)
    FetchContent_Declare(openfx
        GIT_REPOSITORY https://github.com/AcademySoftwareFoundation/openfx.git
        GIT_TAG OFX_Release_1.5.1
        GIT_SHALLOW TRUE)
    FetchContent_GetProperties(openfx)
    if(NOT openfx_POPULATED)
        FetchContent_Populate(openfx)  # sources only; we compile the Support library ourselves
    endif()
    set(OFX_SDK_DIR ${openfx_SOURCE_DIR})
endif()

file(GLOB OFX_SUPPORT_SOURCES ${OFX_SDK_DIR}/Support/Library/*.cpp)

# Support sources go straight into the module: OfxGetPlugin / OfxGetNumberOfPlugins
# must be exported, which a static library would not guarantee on Windows.
add_library(chromatron_ofx MODULE ChromatronOFX.cpp ${OFX_SUPPORT_SOURCES})
target_include_directories(chromatron_ofx PRIVATE ${OFX_SDK_DIR}/include ${OFX_SDK_DIR}/Support/include
                                                  ${OFX_SDK_DIR}/Support/Library)
target_link_libraries(chromatron_ofx PRIVATE chromatron_core)
target_compile_definitions(chromatron_ofx PRIVATE $<$<BOOL:${WIN32}>:NOMINMAX _CRT_SECURE_NO_WARNINGS>)
# OfxExport is plain `extern` outside Windows, so the entry points need default visibility.
set_target_properties(chromatron_ofx PROPERTIES CXX_VISIBILITY_PRESET default VISIBILITY_INLINES_HIDDEN OFF)

if(WIN32)
    set(OFX_ARCH Win64)
elseif(APPLE)
    set(OFX_ARCH MacOS)
else()
    set(OFX_ARCH Linux-x86-64)
endif()
set(OFX_BUNDLE ${CMAKE_BINARY_DIR}/Chromatron.ofx.bundle)

set_target_properties(chromatron_ofx PROPERTIES
    OUTPUT_NAME Chromatron
    PREFIX ""
    SUFFIX ".ofx"
    LIBRARY_OUTPUT_DIRECTORY $<1:${OFX_BUNDLE}/Contents/${OFX_ARCH}>)

if(APPLE)
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in ${OFX_BUNDLE}/Contents/Info.plist @ONLY)
    set_target_properties(chromatron_ofx PROPERTIES LINK_FLAGS "-Wl,-exported_symbol,_OfxGetPlugin -Wl,-exported_symbol,_OfxGetNumberOfPlugins")
elseif(UNIX)
    target_link_options(chromatron_ofx PRIVATE -Wl,--exclude-libs,ALL -static-libstdc++ -static-libgcc)
endif()

if(WIN32)
    set(OFX_INSTALL_DIR "C:/Program Files/Common Files/OFX/Plugins" CACHE PATH "OFX plug-in folder")
elseif(APPLE)
    set(OFX_INSTALL_DIR "/Library/OFX/Plugins" CACHE PATH "OFX plug-in folder")
else()
    set(OFX_INSTALL_DIR "/usr/OFX/Plugins" CACHE PATH "OFX plug-in folder")
endif()
install(DIRECTORY ${OFX_BUNDLE} DESTINATION ${OFX_INSTALL_DIR} USE_SOURCE_PERMISSIONS)
