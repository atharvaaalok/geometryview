file(GLOB_RECURSE app_sources CONFIGURE_DEPENDS Sources/*.swift)
add_executable(Geometry MACOSX_BUNDLE ${app_sources})

set_target_properties(Geometry PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Info.plist.in"
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.geometry.viewer"
    MACOSX_BUNDLE_BUNDLE_NAME "Geometry"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    Swift_LANGUAGE_VERSION 6
)
target_compile_options(Geometry PRIVATE
    "$<$<COMPILE_LANGUAGE:Swift>:SHELL:-I ${CMAKE_SOURCE_DIR}/Engine/include>"
    "$<$<COMPILE_LANGUAGE:Swift>:-warnings-as-errors>"
    "$<$<AND:$<COMPILE_LANGUAGE:Swift>,$<CONFIG:Release>>:-O>"
)
target_link_libraries(Geometry PRIVATE engine "-framework AppKit")
add_dependencies(Geometry engine_shaders)

set(bundle_resources "$<TARGET_BUNDLE_CONTENT_DIR:Geometry>/Resources")
add_custom_command(TARGET Geometry POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${bundle_resources}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${GEO_METALLIB}" "${bundle_resources}/default.metallib"
    COMMAND codesign --force --sign - --options runtime
            --entitlements "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Geometry.entitlements"
            "$<TARGET_BUNDLE_DIR:Geometry>"
    COMMENT "Bundling resources and signing Geometry.app"
    VERBATIM
)
