# Port Linux : GTK3 + WebKitGTK (webkit2gtk-4.1) + libcurl. Dépendances Debian/Ubuntu :
#   libgtk-3-dev libwebkit2gtk-4.1-dev libcurl4-openssl-dev   (Arch : gtk3 webkit2gtk-4.1 curl)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK3 REQUIRED IMPORTED_TARGET gtk+-3.0)
pkg_check_modules(WEBKIT REQUIRED IMPORTED_TARGET webkit2gtk-4.1)
pkg_check_modules(CURL REQUIRED IMPORTED_TARGET libcurl)
pkg_check_modules(X11 REQUIRED IMPORTED_TARGET x11)   # raccourci global (XGrabKey)

find_package(Threads REQUIRED)

add_executable(Aeria
        src/linux/main_linux.cpp
        src/linux/dock_linux.cpp            # états, animations, souris, raccourci
        src/linux/bridge_linux.cpp          # routeur des commandes de la page
        src/linux/util_linux.cpp            # dossiers (XDG)
        src/linux/api_key_linux.cpp         # clés API (fichiers 0600)
        src/linux/anthropic_http_linux.cpp  # mode Clé API et Gratuit (libcurl)
        src/linux/claude_code_linux.cpp     # mode Abonnement Pro (fork/exec)
        src/linux/autostart_linux.cpp       # ~/.config/autostart
        src/linux/tray_linux.cpp            # notifications D-Bus
        src/linux/media_linux.cpp           # lecteur : MPRIS2
        src/linux/appnotifs_linux.cpp       # notifications des autres applis : pas encore prises en charge
        src/linux/pc_linux.cpp              # fichiers, routines, volume
        src/linux/screen_linux.cpp          # capture d'écran (X11) pour l'assistante
        src/features/reminders.cpp          # échéances des rappels et du minuteur (partagé avec Windows)
        src/features/updater_common.cpp     # version et vérification des mises à jour (partagé avec Windows)
        src/features/voice.cpp              # commande vocale « Aeria » (partagé avec Windows)
        src/linux/voice_linux.cpp           # micro (parec / arecord) et téléchargement du modèle
        src/linux/updater_linux.cpp         # pas d'installation automatique sous Linux
        src/common/util_common.cpp)
target_include_directories(Aeria PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(Aeria PRIVATE AERIA_VERSION_STR="${AERIA_VERSION_NUM}")
include(cmake/whisper.cmake)
target_link_libraries(Aeria PRIVATE whisper PkgConfig::GTK3 PkgConfig::WEBKIT PkgConfig::CURL PkgConfig::X11 Threads::Threads)

# L'interface se trouve à côté de l'exécutable.
add_custom_target(aeria_assets ALL
        COMMAND ${CMAKE_COMMAND} -E remove_directory $<TARGET_FILE_DIR:Aeria>/ui
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/ui $<TARGET_FILE_DIR:Aeria>/ui
        COMMENT "Copie de l'interface")
add_dependencies(Aeria aeria_assets)

# Deux paquets, deux dispositions (choisies à la configuration) :
#   défaut                  → archive portable : `cpack -G TGZ` → Aeria-<version>-linux-x86_64.tar.gz
#   -DAERIA_DEB_LAYOUT=ON  → paquet .deb : `cpack -G DEB` → aeria_<version>_amd64.deb
#                             /opt/aeria + /usr/bin/aeria + entrée de menu + icône
option(AERIA_DEB_LAYOUT "Disposition du paquet .deb (/opt, /usr/bin, menu) au lieu de l'archive portable" OFF)

if (AERIA_DEB_LAYOUT)
    set(APP_DIR opt/aeria)
else ()
    set(APP_DIR .)
endif ()
install(TARGETS Aeria RUNTIME DESTINATION ${APP_DIR})
install(DIRECTORY ${CMAKE_SOURCE_DIR}/ui DESTINATION ${APP_DIR})

if (AERIA_DEB_LAYOUT)
    install(FILES ${CMAKE_SOURCE_DIR}/installer/linux/aeria.desktop DESTINATION usr/share/applications)
    install(FILES ${CMAKE_SOURCE_DIR}/installer/linux/aeria.svg
            DESTINATION usr/share/icons/hicolor/scalable/apps)
    # La commande `aeria` : le lien est suivi par /proc/self/exe, donc ui/ est trouvé dans /opt.
    install(CODE "
        file(MAKE_DIRECTORY \"\$ENV{DESTDIR}/usr/bin\")
        file(CREATE_LINK /opt/aeria/Aeria \"\$ENV{DESTDIR}/usr/bin/aeria\" SYMBOLIC)
    ")

    # Version Debian : « v0.1.0 » → « 0.1.0 » ; un nom de branche ou « dev » devient « 0.0.0~dev ».
    string(REGEX REPLACE "^v" "" _deb_version "${AERIA_VERSION}")
    if (NOT _deb_version MATCHES "^[0-9]")
        set(_deb_version "0.0.0~${_deb_version}")
    endif ()

    set(CPACK_GENERATOR DEB)
    set(CPACK_PACKAGING_INSTALL_PREFIX /)
    set(CPACK_PACKAGE_NAME aeria)
    set(CPACK_PACKAGE_VERSION "${_deb_version}")
    set(CPACK_PACKAGE_CONTACT "Aeria")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Dock en verre : rappels, to-do, minuteur, notes, musique et Claude")
    set(CPACK_DEBIAN_PACKAGE_SECTION utils)
    set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "xdg-utils, wireplumber | pulseaudio-utils, pulseaudio-utils | alsa-utils")
    set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
    # Écrite explicitement : sans dpkg (Arch, Fedora…), CPack se rabattrait sur i386.
    if (CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")
        set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE amd64)
    elseif (CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
        set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE arm64)
    endif ()
    # Dépendances : calculées d'après les bibliothèques liées quand dpkg-shlibdeps existe (Debian, Ubuntu) ;
    # sinon liste écrite à la main (noms Debian/Ubuntu, avec la variante « t64 » d'Ubuntu 24.04).
    find_program(DPKG_SHLIBDEPS dpkg-shlibdeps)
    if (DPKG_SHLIBDEPS)
        set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
    else ()
        set(CPACK_DEBIAN_PACKAGE_DEPENDS "libgtk-3-0 | libgtk-3-0t64, libwebkit2gtk-4.1-0, libcurl4 | libcurl4t64, libx11-6")
    endif ()
else ()
    set(CPACK_PACKAGE_NAME Aeria)
    set(CPACK_PACKAGE_FILE_NAME "Aeria-${AERIA_VERSION}-linux-${CMAKE_SYSTEM_PROCESSOR}")
    set(CPACK_GENERATOR TGZ)
    set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)
endif ()
include(CPack)
