file(GLOB_RECURSE app_sources CONFIGURE_DEPENDS Sources/*.swift)
add_executable(GeometryView MACOSX_BUNDLE ${app_sources})

set_target_properties(GeometryView PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Info.plist.in"
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.aalok.geometryview"
    MACOSX_BUNDLE_BUNDLE_NAME "GeometryView"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    Swift_LANGUAGE_VERSION 6
)
target_compile_options(GeometryView PRIVATE
    "$<$<COMPILE_LANGUAGE:Swift>:SHELL:-I ${CMAKE_SOURCE_DIR}/Engine/include>"
    "$<$<COMPILE_LANGUAGE:Swift>:-warnings-as-errors>"
    "$<$<AND:$<COMPILE_LANGUAGE:Swift>,$<CONFIG:Release>>:-O>"
)
target_link_libraries(GeometryView PRIVATE engine "-framework AppKit")
target_link_options(GeometryView PRIVATE
    "$<$<CONFIG:Release>:SHELL:-Xlinker -dead_strip>"
)
add_dependencies(GeometryView engine_shaders geometryview-import geometryview-occt-worker)

# The icon, made from its 1024 px master.
set(app_icon "${CMAKE_CURRENT_BINARY_DIR}/AppIcon.icns")
add_custom_command(OUTPUT "${app_icon}"
    COMMAND "${CMAKE_SOURCE_DIR}/Tools/make_icons.sh" "${app_icon}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/Resources/AppIcon.png" "${CMAKE_SOURCE_DIR}/Tools/make_icons.sh"
    COMMENT "Making the app icon"
    VERBATIM
)
add_custom_target(app_icon DEPENDS "${app_icon}")
add_dependencies(GeometryView app_icon)
# A new icon relinks the app, as the chrome's icons below do, so the bundle gets it and is signed again.
set_property(TARGET GeometryView APPEND PROPERTY LINK_DEPENDS "${app_icon}")
# So do new entitlements: the bundle is signed with them after it links.
set_property(TARGET GeometryView APPEND PROPERTY LINK_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/Resources/GeometryView.entitlements"
    "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Helper.entitlements")

file(GLOB font_files CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/ThirdParty/ibm-plex-mono/*.ttf")
# The chrome's icons, shared with the web viewer (App/Sources/Design/Icons.swift reads them). A
# change to them relinks the app, so the bundle is copied and signed again.
set(chrome_icons "${CMAKE_SOURCE_DIR}/Web/viewer/icons.js")
set_property(TARGET GeometryView APPEND PROPERTY LINK_DEPENDS "${chrome_icons}")
set(bundle_resources "$<TARGET_BUNDLE_CONTENT_DIR:GeometryView>/Resources")
add_custom_command(TARGET GeometryView POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${bundle_resources}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${GEO_METALLIB}" "${bundle_resources}/default.metallib"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${app_icon}" "${bundle_resources}/AppIcon.icns"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${bundle_resources}/Fonts"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${font_files} "${bundle_resources}/Fonts/"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${chrome_icons}" "${bundle_resources}/icons.js"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:geometryview-import>"
            "$<TARGET_BUNDLE_CONTENT_DIR:GeometryView>/MacOS/geometryview-import"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:geometryview-occt-worker>"
            "$<TARGET_BUNDLE_CONTENT_DIR:GeometryView>/MacOS/geometryview-occt-worker"
    COMMAND codesign --force --sign - --options runtime
            --entitlements "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Helper.entitlements"
            "$<TARGET_BUNDLE_CONTENT_DIR:GeometryView>/MacOS/geometryview-import"
            "$<TARGET_BUNDLE_CONTENT_DIR:GeometryView>/MacOS/geometryview-occt-worker"
    COMMAND codesign --force --sign - --options runtime
            --entitlements "${CMAKE_CURRENT_SOURCE_DIR}/Resources/GeometryView.entitlements"
            "$<TARGET_BUNDLE_DIR:GeometryView>"
    COMMENT "Bundling resources and signing GeometryView.app"
    VERBATIM
)

# cmake --build --preset release --target install-app puts the app in /Applications, where
# Spotlight, Launchpad, the Dock and Finder's Open With find it, and registers it, so STEP and STL
# files open in it. A copy already there is mirrored: files a newer build dropped go too.
set(GEO_APP_INSTALL_DIR "/Applications" CACHE PATH "Where install-app puts GeometryView.app")
if(GEO_APP_INSTALL_DIR STREQUAL "")
    message(FATAL_ERROR "GEO_APP_INSTALL_DIR must name a folder")
endif()
add_custom_target(install-app
    COMMAND rsync -a --delete "$<TARGET_BUNDLE_DIR:GeometryView>/"
            "${GEO_APP_INSTALL_DIR}/GeometryView.app/"
    COMMAND /System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister
            -f "${GEO_APP_INSTALL_DIR}/GeometryView.app"
    COMMAND ${CMAKE_COMMAND} -E echo "Installed ${GEO_APP_INSTALL_DIR}/GeometryView.app"
    DEPENDS GeometryView
    VERBATIM
)
