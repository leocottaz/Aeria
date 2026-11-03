// Faux Setup.exe : écrit ses arguments dans un fichier journal, puis quitte. Ne fait rien d'autre.
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

int main(int argc, char** argv) {
    char path[MAX_PATH];
    GetTempPathA(MAX_PATH, path);
    strcat(path, "aeria-fake-setup.log");
    FILE* f = fopen(path, "a");
    if (f) {
        fprintf(f, "LANCE:");
        for (int i = 1; i < argc; i++) fprintf(f, " %s", argv[i]);
        fprintf(f, "\n");
        fclose(f);
    }
    return 0;
}
