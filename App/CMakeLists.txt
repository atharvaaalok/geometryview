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

file(GLOB font_files CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/ThirdParty/ibm-plex-mono/*.ttf")
set(bundle_resources "$<TARGET_BUNDLE_CONTENT_DIR:GeometryView>/Resources")
add_custom_command(TARGET GeometryView POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${bundle_resources}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${GEO_METALLIB}" "${bundle_resources}/default.metallib"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${bundle_resources}/Fonts"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${font_files} "${bundle_resources}/Fonts/"
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
