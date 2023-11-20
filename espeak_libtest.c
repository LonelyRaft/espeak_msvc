#include "speak_lib.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifdef _MSC_VER

#include <windows.h>

#endif // _MSC_VER

#define BUF_SIZE (1024)

static int espeak_synth_callback(
        short *wav, int num,
        espeak_EVENT *event) {
    (void) wav;
    (void) num;
    (void) event;
    return 0;
}

int main(int argc, char *argv[]) {
    int rate = -1;
    FILE *ftext = fopen("../readtext.txt", "r");
    if (ftext == NULL) {
        return -1;
    }
    fseek(ftext, 0, SEEK_SET);
    rate = espeak_Initialize(
            AUDIO_OUTPUT_PLAYBACK, 10000, "../", 0);
    if (rate < 0) {
        return -1;
    }
    espeak_SetSynthCallback(espeak_synth_callback);
    espeak_SetParameter(espeakVOLUME, 200, 0);
    espeak_SetParameter(espeakRATE, 175, 0);
    espeak_SetVoiceByName("zh+f2");
    {
        char buffer[BUF_SIZE] = {0};
        int length = fscanf(ftext, " %[^\n]", buffer);
        while (length > 0) {
            length = (int)strlen((char*)buffer);
#ifdef _MSC_VER
            {
                wchar_t wbuffer[BUF_SIZE] = {0};
                length = MultiByteToWideChar(
                        CP_UTF8, 0, buffer, length,
                        wbuffer, BUF_SIZE - 1);
                length *= sizeof(wchar_t);
                espeak_Synth(wbuffer, length, 0, POS_CHARACTER,
                         0, espeakCHARS_WCHAR, NULL, NULL);
            }
#else
            espeak_Synth(buffer, length, 0, POS_CHARACTER,
                         0, espeakCHARS_UTF8, NULL, NULL);
#endif // _MSC_VER
            espeak_Synchronize();
            length = fscanf(ftext, " %[^\n]", buffer);
        }
    }
    espeak_Terminate();
    fclose(ftext);
    return 0;
}
