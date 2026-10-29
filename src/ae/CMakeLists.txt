# After Effects plug-in: Chromatron.aex (Windows) / Chromatron.plugin (macOS).
# Requires the After Effects SDK (https://developer.adobe.com/after-effects/), which cannot
# be redistributed: pass its root folder (the one containing Examples/) as AE_SDK_DIR.

set(AE_SDK_DIR "" CACHE PATH "After Effects SDK root (contains Examples/Headers)")
if(NOT EXISTS "${AE_SDK_DIR}/Examples/Headers/AE_Effect.h")
    message(FATAL_ERROR "CHROMATRON_BUILD_AE needs AE_SDK_DIR pointing at the After Effects SDK")
endif()

set(AE_HEADERS ${AE_SDK_DIR}/Examples/Headers)
set(AE_RESOURCES ${AE_SDK_DIR}/Examples/Resources)
set(AE_INCLUDES ${AE_HEADERS} ${AE_HEADERS}/SP ${AE_HEADERS}/adobesdk ${AE_SDK_DIR}/Examples/Util ${AE_RESOURCES})
set(PIPL_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/ChromatronAE_PiPL.r)

if(WIN32)
    # .r -> preprocess -> PiPLTool -> .rc, as in the SDK's Visual Studio projects.
    set(PIPL_RR ${CMAKE_CURRENT_BINARY_DIR}/ChromatronAE_PiPL.rr)
    set(PIPL_RRC ${CMAKE_CURRENT_BINARY_DIR}/ChromatronAE_PiPL.rrc)
    set(PIPL_RC ${CMAKE_CURRENT_BINARY_DIR}/ChromatronAE_PiPL.rc)
    set(PIPL_INCLUDE_FLAGS)
    foreach(dir ${AE_INCLUDES})
        list(APPEND PIPL_INCLUDE_FLAGS "/I${dir}")
    endforeach()
    find_program(AE_PIPLTOOL PiPLtool PATHS ${AE_RESOURCES} NO_DEFAULT_PATH REQUIRED)
    add_custom_command(
        OUTPUT ${PIPL_RC}
        COMMAND ${CMAKE_CXX_COMPILER} /nologo /EP /P /Fi${PIPL_RR} ${PIPL_INCLUDE_FLAGS} /Tc${PIPL_SOURCE}
        COMMAND ${AE_PIPLTOOL} ${PIPL_RR} ${PIPL_RRC}
        COMMAND ${CMAKE_CXX_COMPILER} /nologo /EP /P /DMSWindows /Fi${PIPL_RC} /Tc${PIPL_RRC}
        DEPENDS ${PIPL_SOURCE}
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        COMMENT "Compiling After Effects PiPL"
        VERBATIM)
    add_library(chromatron_ae MODULE ChromatronAE.cpp ${PIPL_RC})
    target_compile_definitions(chromatron_ae PRIVATE MSWindows WIN32 _WINDOWS NOMINMAX _CRT_SECURE_NO_WARNINGS)
    set_target_properties(chromatron_ae PROPERTIES OUTPUT_NAME Chromatron SUFFIX ".aex"
                                                   LIBRARY_OUTPUT_DIRECTORY $<1:${CMAKE_BINARY_DIR}/AfterEffects>)
    set(AE_INSTALL_DIR "C:/Program Files/Adobe/Common/Plug-ins/7.0/MediaCore" CACHE PATH "AE plug-in folder")
    install(TARGETS chromatron_ae LIBRARY DESTINATION ${AE_INSTALL_DIR})

elseif(APPLE)
    set(PIPL_RSRC ${CMAKE_CURRENT_BINARY_DIR}/Chromatron.rsrc)
    set(REZ_INCLUDE_FLAGS)
    foreach(dir ${AE_INCLUDES})
        list(APPEND REZ_INCLUDE_FLAGS -i ${dir})
    endforeach()
    add_custom_command(
        OUTPUT ${PIPL_RSRC}
        COMMAND xcrun Rez -useDF -d __MACH__ ${REZ_INCLUDE_FLAGS} -o ${PIPL_RSRC} ${PIPL_SOURCE}
        DEPENDS ${PIPL_SOURCE}
        COMMENT "Compiling After Effects PiPL"
        VERBATIM)
    set_source_files_properties(${PIPL_RSRC} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    set(PKGINFO ${CMAKE_CURRENT_BINARY_DIR}/PkgInfo)
    file(WRITE ${PKGINFO} "eFKTFXTC")
    set_source_files_properties(${PKGINFO} PROPERTIES MACOSX_PACKAGE_LOCATION .)
    add_library(chromatron_ae MODULE ChromatronAE.cpp ${PIPL_RSRC} ${PKGINFO})
    set_target_properties(chromatron_ae PROPERTIES
        BUNDLE TRUE
        BUNDLE_EXTENSION plugin
        OUTPUT_NAME Chromatron
        MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in
        LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/AfterEffects)
    target_link_options(chromatron_ae PRIVATE -Wl,-exported_symbol,_EffectMain
                                              -Wl,-exported_symbol,_PluginDataEntryFunction2)
    set(AE_INSTALL_DIR "/Library/Application Support/Adobe/Common/Plug-ins/7.0/MediaCore" CACHE PATH
        "AE plug-in folder")
    install(TARGETS chromatron_ae LIBRARY DESTINATION ${AE_INSTALL_DIR})
else()
    message(FATAL_ERROR "After Effects plug-ins can only be built on Windows or macOS")
endif()

target_include_directories(chromatron_ae PRIVATE ${AE_INCLUDES})
target_link_libraries(chromatron_ae PRIVATE chromatron_core)
# ChromatronAE.cpp checks kMajor / kMinor / kBug against the project version at compile time.
target_compile_definitions(chromatron_ae PRIVATE CHROMATRON_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
                                                 CHROMATRON_VERSION_MINOR=${PROJECT_VERSION_MINOR}
                                                 CHROMATRON_VERSION_PATCH=${PROJECT_VERSION_PATCH})

# Premiere Pro GPU filter (xGPUFilterEntry in the same module). Optional: without the Premiere Pro
# SDK the plug-in still works in Premiere, rendering on the CPU.
set(PR_SDK_DIR "" CACHE PATH "Premiere Pro SDK root (contains Examples/Headers): enables the Premiere GPU filter")
if(PR_SDK_DIR)
    if(NOT EXISTS "${PR_SDK_DIR}/Examples/Headers/PrSDKGPUFilter.h")
        message(FATAL_ERROR "PR_SDK_DIR must point at the Premiere Pro SDK (Examples/Headers/PrSDKGPUFilter.h)")
    endif()
    target_sources(chromatron_ae PRIVATE ChromatronPremiereGPU.cpp)
    set_source_files_properties(ChromatronPremiereGPU.cpp PROPERTIES
        INCLUDE_DIRECTORIES "${PR_SDK_DIR}/Examples/Headers;${PR_SDK_DIR}/Examples/Projects/GPUVideoFilter/Utils"
        COMPILE_DEFINITIONS $<IF:$<PLATFORM_ID:Windows>,PRWIN_ENV,PRMAC_ENV>)
    if(APPLE)
        target_link_options(chromatron_ae PRIVATE -Wl,-exported_symbol,_xGPUFilterEntry)
    endif()
    message(STATUS "Premiere Pro GPU filter: ${PR_SDK_DIR}")
endif()
