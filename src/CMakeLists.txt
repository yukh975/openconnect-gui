set(CMAKE_VERBOSE_MAKEFILE ON)

set_property(SOURCE ${PROJECT_NAME}.cpp PROPERTY SKIP_AUTOGEN ON)
set_property(SOURCE timestamp.cpp PROPERTY SKIP_AUTOGEN ON)

set(${PROJECT_NAME}_SRCS
    openconnect-gui.qrc
    cert.cpp
    common.h
    config.h.in
    cryptdata.cpp
    dialog/MyCertMsgBox.cpp
    dialog/MyInputDialog.cpp
    dialog/MyMsgBox.cpp
    dialog/editdialog.cpp
    dialog/editdialog.ui
    dialog/NewProfileDialog.cpp
    dialog/NewProfileDialog.ui
    dialog/logdialog.cpp
    dialog/logdialog.ui
    dialog/VpnProtocolModel.cpp
    dialog/mainwindow.cpp
    dialog/mainwindow.ui
    gtdb.cpp
    key.cpp
    keypair.cpp
    main.cpp
    server_storage.cpp

    timestamp.cpp

    vpninfo.cpp

    logger.cpp

    FileLogger.cpp
    OcSettings.h
)

set_source_files_properties(
    ${PROJECT_NAME}.cpp
    timestamp.cpp
    PROPERTIES
        GENERATED true
)

configure_file(
    config.h.in
    config.h
    @ONLY
)

if(MINGW)
    list(APPEND ${PROJECT_NAME}_RESOURCES
        ${PROJECT_NAME}.rc.in
        ${PROJECT_NAME}.rc
    )

    set_source_files_properties(
       ${PROJECT_NAME}.rc
        PROPERTIES
            GENERATED true
    )
endif()
if(APPLE)
    list(APPEND ${PROJECT_NAME}_RESOURCES
        Resources/mono_lock.icns
        ${OPENCONNECT_VPNC_SCRIPT}
    )
    set_source_files_properties(
        Resources/mono_lock.icns
        ${OPENCONNECT_VPNC_SCRIPT}
            PROPERTIES MACOSX_PACKAGE_LOCATION Resources
    )
endif()

if(APPLE)
    list(APPEND ${PROJECT_NAME}_SRCS
        macos/MacHelper.cpp
        macos/MacHelper.h
        macos/ocg-helper-protocol.h
    )
endif()

add_executable(${PROJECT_NAME}
    ${${PROJECT_NAME}_SRCS}
    ${${PROJECT_NAME}_RESOURCES}
)

if(APPLE)
    # The privileged helper and the tun script talking to it, see
    # macos/ocg-helper-protocol.h. Both go next to the main executable.
    enable_language(C)
    add_executable(ocg-helper macos/ocg-helper.c)
    target_link_libraries(ocg-helper "-framework CoreFoundation" "-framework SystemConfiguration")
    add_executable(ocg-tun-client macos/ocg-tun-client.c)
    add_dependencies(${PROJECT_NAME} ocg-helper ocg-tun-client)
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:ocg-helper> $<TARGET_FILE:ocg-tun-client> $<TARGET_FILE_DIR:${PROJECT_NAME}>
        COMMENT "Deploying the privileged helper..."
        VERBATIM
    )
endif()

if(MINGW)
    add_definitions(-DSPDLOG_WCHAR_FILENAMES)

    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_BINARY_DIR}/external/vpnc-script.js $<TARGET_FILE_DIR:${PROJECT_NAME}>
        COMMENT "Deploying vpnc-script..."
        VERBATIM
    )

    add_dependencies(${PROJECT_NAME}
        vpnc-scripts-${vpnc-scripts-TAG}
    )

    set_target_properties(${PROJECT_NAME} PROPERTIES
        WIN32_EXECUTABLE true
    )

    target_link_libraries (${PROJECT_NAME}
        # system libraries
        wsock32
    )
endif()

#code signing
if(SIGN_EXE STREQUAL "true")
    add_custom_command(
        TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND sh -c "${CMAKE_SOURCE_DIR}/contrib/sign.sh \"$<TARGET_FILE:${PROJECT_NAME}>\""
        COMMENT "Signing executable ..."
        VERBATIM
    )
endif()

if(WIN32 AND NOT CMAKE_CROSSCOMPILING)
    add_dependencies(${PROJECT_NAME}
        openconnect-devel-${openconnect-TAG}
        openconnect-${openconnect-TAG}
    )

    target_link_libraries (${PROJECT_NAME}
        # 3rd party dependencies
        openconnect::gmp
        openconnect::gnutls
        openconnect::hogweed
        openconnect::nettle
        openconnect::openconnect
        openconnect::p11-kit
        openconnect::stoken
        openconnect::xml2
    )
else()
    target_link_libraries (${PROJECT_NAME}
        ${OPENCONNECT_LIBRARIES}
        ${GNUTLS_LIBRARY}
    )
endif()

if(APPLE)
    configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/Resources/${PROJECT_NAME}.plist.in
        ${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}.plist
        @ONLY
    )
    set_target_properties(${PROJECT_NAME} PROPERTIES
        MACOSX_BUNDLE true
        OUTPUT_NAME ${PRODUCT_NAME_SHORT}
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}.plist"
        MACOSX_BUNDLE_BUNDLE_NAME ${PRODUCT_NAME_SHORT}
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${PROJECT_VERSION_PATCH}"
        MACOSX_BUNDLE_COPYRIGHT "${PRODUCT_NAME_COPYRIGHT_SHORT}"
        MACOSX_BUNDLE_GUI_IDENTIFIER net.openconnect-vpn.gui
        MACOSX_BUNDLE_ICON_FILE mono_lock.icns
        MACOSX_BUNDLE_INFO_STRING ${PRODUCT_NAME_LONG}
    )
endif()


# common stuff to link...
target_link_libraries (${PROJECT_NAME}
    spdlog::spdlog
    qt-solutions::qtsingleapplication

    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Network
    Qt6::StateMachine
)

if(UNIX)
    target_link_libraries(${PROJECT_NAME}
        Threads::Threads
    )
    if(APPLE)
        target_link_libraries (${PROJECT_NAME}
            ${SECURITY_LIBRARY}
        )
    endif()
endif()

# install executable
install(TARGETS ${PROJECT_NAME}
    RUNTIME DESTINATION . COMPONENT App
    BUNDLE DESTINATION . COMPONENT App
    LIBRARY DESTINATION . COMPONENT App
)

# fixup the bundle
get_target_property(QT_LIBRARY_DIR Qt6::Core LOCATION)
get_filename_component(QT_LIBRARY_DIR ${QT_LIBRARY_DIR} PATH)
list(APPEND libSearchDirs ${QT_LIBRARY_DIR})

if(MINGW)
    set(APPS "\${CMAKE_INSTALL_PREFIX}/${PROJECT_NAME}.exe")
    list(APPEND libSearchDirs ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})

    # Qt Platform Plugin
    install(FILES
        ${_qwindows_dll}
        DESTINATION plugins/platforms
        COMPONENT App
    )
    # Qt schannel plugin
    install(FILES
        ${_schannel_dll}
        DESTINATION plugins/tls
        COMPONENT App
    )
    install(FILES
        ${CMAKE_SOURCE_DIR}/nsis/qt.conf
        DESTINATION .
        COMPONENT App
    )

    set(additionalLib ${_qwindows_dll})
    if(Qt6Core_VERSION VERSION_GREATER_EQUAL "6.0")
        install(FILES
            ${_qwinstyle_dylib}
            DESTINATION plugins/styles
            COMPONENT App
        )
    endif()
elseif(APPLE)
    set(APPS "\${CMAKE_INSTALL_PREFIX}/${PROJECT_NAME}.app")
    list(APPEND libSearchDirs "")

    # Qt Platform Plugin
    install(FILES
        ${_qcocoa_dylib}
        DESTINATION ${PROJECT_NAME}.app/Contents/PlugIns/platforms
        COMPONENT App
    )
    set(additionalLib \${CMAKE_INSTALL_PREFIX}/${PROJECT_NAME}.app/Contents/PlugIns/platforms/libqcocoa.dylib)
    if(Qt6Core_VERSION VERSION_GREATER_EQUAL "6.0")
        install(FILES
            ${_qmacstyle_dylib}
            DESTINATION ${PROJECT_NAME}.app/Contents/PlugIns/styles
            COMPONENT App
        )
        set(additionalLib
            ${additionalLib}
            \${CMAKE_INSTALL_PREFIX}/${PROJECT_NAME}.app/Contents/PlugIns/styles/libqmacstyle.dylib
        )
    endif()
    install(FILES
        ${CMAKE_SOURCE_DIR}/bundle/qt.conf
        DESTINATION ${PROJECT_NAME}.app/Contents/Resources
        COMPONENT App
    )
else()
    message(STATUS "Nothing to bundle...")
endif()

if(MINGW)
    #wintun
    install(FILES
        ${CMAKE_BINARY_DIR}/bin/wintun.dll
        DESTINATION .
        COMPONENT App
    )
endif()

install(CODE "
    include(BundleUtilities)
    if(APPLE)
        set(BU_CHMOD_BUNDLE_ITEMS on)
    endif()
    fixup_bundle(\"${APPS}\" \"${additionalLib}\" \"${libSearchDirs}\")
    " DESTINATION . COMPONENT App
)
