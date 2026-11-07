cmake_minimum_required(VERSION 3.25)
project(Aeria C CXX)

set(AERIA_VERSION "dev" CACHE STRING "Version affichée dans le nom du paquet (ex. v0.1.0)")
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Numéro compilé dans l'exécutable (« v0.1.0 » → « 0.1.0 »). Un build local (« dev ») donne 0.0.0 :
# la vérification des mises à jour est alors désactivée (features/updater.h).
string(REGEX REPLACE "^v" "" AERIA_VERSION_NUM "${AERIA_VERSION}")
if (NOT AERIA_VERSION_NUM MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    set(AERIA_VERSION_NUM "0.0.0")
endif ()

# Linux : cible séparée (GTK3 + WebKitGTK), voir cmake/linux.cmake.
if (NOT WIN32)
    include(cmake/linux.cmake)
    return()
endif ()

enable_language(RC)

# Un dossier = un domaine. Voir docs/ARCHITECTURE.md pour le rôle de chaque module.
add_executable(Aeria WIN32
        src/main.cpp                    # instance unique, initialisation, boucle de messages

        # app/ : la fenêtre du dock et son intégration à Windows
        src/app/dock.cpp                # fenêtre, états, animations, souris, raccourci, menu
        src/app/drop_edge.cpp           # bande du bord haut qui reconnaît un glisser de fichiers
        src/app/tray.cpp                # icône de notification, bulles
        src/app/autostart.cpp           # lancement au démarrage
        src/app/aeria.rc     # icône de l'exécutable

        # web/ : WebView2 et protocole avec la page
        src/web/webview_host.cpp        # WebView2
        src/web/bridge.cpp              # routeur des commandes de la page

        # ai/ : connexion à Claude et aux modèles gratuits
        src/ai/anthropic_http.cpp       # mode Clé API et Gratuit (HTTPS)
        src/ai/claude_code.cpp          # mode Abonnement Pro (claude -p)
        src/ai/api_key.cpp              # clés chiffrées (DPAPI)

        # features/ : fonctionnalités qui ont besoin du natif
        src/features/reminders.cpp      # échéances des rappels et du minuteur
        src/features/media.cpp          # lecteur : musique en cours via Windows
        src/features/appnotifs.cpp      # notifications des autres applis (UserNotificationListener)
        src/features/pc.cpp             # recherche de fichiers, routines, volume
        src/features/screen.cpp         # capture d'écran (sans le dock) pour l'assistante
        src/features/updater.cpp        # mise à jour : télécharge, vérifie et lance Setup.exe
        src/features/updater_common.cpp # version, tag, SHA256SUMS (partagé avec Linux)
        src/features/voice.cpp          # commande vocale « Aeria » (partagé avec Linux)
        src/features/voice_win.cpp      # micro (waveIn) et téléchargement du modèle

        # common/ : utilitaires partagés
        src/common/util.cpp
        src/common/util_common.cpp)
set_source_files_properties(src/app/aeria.rc PROPERTIES OBJECT_DEPENDS ${CMAKE_SOURCE_DIR}/src/app/aeria.ico)

target_include_directories(Aeria PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/third_party/webview2/include)
target_compile_definitions(Aeria PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00
        AERIA_VERSION_STR="${AERIA_VERSION_NUM}")
include(cmake/whisper.cmake)
target_link_libraries(Aeria PRIVATE whisper winmm bcrypt dwmapi winhttp crypt32 shell32 ole32 oleaut32 user32 gdi32 advapi32 shcore runtimeobject comdlg32 uuid gdiplus)

if (MINGW)
    target_compile_options(Aeria PRIVATE -municode -Wno-unknown-pragmas)
    target_link_options(Aeria PRIVATE -municode -static-libgcc -static-libstdc++ $<$<CONFIG:Release>:-s>)
endif ()

# L'interface (HTML/CSS/JS) et le loader WebView2 doivent être à côté de l'exécutable.
add_custom_target(aeria_assets ALL
        COMMAND ${CMAKE_COMMAND} -E remove_directory $<TARGET_FILE_DIR:Aeria>/ui
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/ui $<TARGET_FILE_DIR:Aeria>/ui
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/third_party/webview2/x64/WebView2Loader.dll $<TARGET_FILE_DIR:Aeria>
        COMMENT "Copie de l'interface et de WebView2Loader.dll")
add_dependencies(Aeria aeria_assets)

# Paquet : `cpack -G ZIP` → Aeria-<version>-windows-x64.zip
install(TARGETS Aeria RUNTIME DESTINATION .)
install(DIRECTORY ${CMAKE_SOURCE_DIR}/ui DESTINATION .)
install(FILES ${CMAKE_SOURCE_DIR}/third_party/webview2/x64/WebView2Loader.dll DESTINATION .)
set(CPACK_PACKAGE_NAME Aeria)
set(CPACK_PACKAGE_FILE_NAME "Aeria-${AERIA_VERSION}-windows-x64")
set(CPACK_GENERATOR ZIP)
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)

if (MINGW)
    get_filename_component(_MINGW_BIN ${CMAKE_CXX_COMPILER} DIRECTORY)
    install(FILES ${_MINGW_BIN}/libwinpthread-1.dll DESTINATION .)
endif ()
include(CPack)

if (MINGW)
    # winpthread ne se lie pas en statique avec cette toolchain : on livre la DLL à côté.
    get_filename_component(MINGW_BIN ${CMAKE_CXX_COMPILER} DIRECTORY)
    add_custom_command(TARGET aeria_assets POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${MINGW_BIN}/libwinpthread-1.dll $<TARGET_FILE_DIR:Aeria>)
endif ()
