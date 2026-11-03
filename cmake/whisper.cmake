# whisper.cpp (reconnaissance vocale hors ligne), compilé avec le projet : voir features/voice.cpp.
# Seul le calcul sur processeur est gardé (pas de GPU, pas d'OpenMP) ; les exemples, tests et
# backends inutiles ont été retirés de third_party/whisper.cpp.
set(WHISPER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_SERVER OFF CACHE BOOL "" FORCE)
set(WHISPER_CCACHE OFF CACHE BOOL "" FORCE)
set(GGML_CCACHE OFF CACHE BOOL "" FORCE)
set(GGML_OPENMP OFF CACHE BOOL "" FORCE)
# Pas de -march=native : l'exécutable doit tourner sur d'autres machines que celle qui le compile.
# AVX2 + FMA + F16C = tout processeur x86-64 depuis 2013.
set(GGML_NATIVE OFF CACHE BOOL "" FORCE)
if (CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")
    set(GGML_AVX2 ON CACHE BOOL "" FORCE)
    set(GGML_FMA ON CACHE BOOL "" FORCE)
    set(GGML_F16C ON CACHE BOOL "" FORCE)
endif ()
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../third_party/whisper.cpp ${CMAKE_BINARY_DIR}/whisper EXCLUDE_FROM_ALL)
