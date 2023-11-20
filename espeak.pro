
# !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
# only for msvc 2010
# !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!

TEMPLATE = app
CONFIG += console
CONFIG -= app_bundle
CONFIG -= qt

SOURCES += \
        compiledict.c \
        debug.c \
        dictionary.c \
        espeak_command.c \
        espeak_libtest.c \
        intonation.c \
        klatt.c \
        numbers.c \
        phonemelist.c \
        readclause.c \
        setlengths.c \
        sonic.c \
        speak_lib.c \
        synth_mbrola.c \
        synthdata.c \
        synthesize.c \
        tr_languages.c \
        translate.c \
        voices.c \
        wavegen.c \
        msvc/event.c \
        msvc/fifo.c \
        msvc/wave.c

HEADERS += \
        debug.h \
        espeak_command.h \
        event.h \
        fifo.h \
        klatt.h \
        phoneme.h \
        portaudio.h \
        sintab.h \
        sonic.h \
        speak_lib.h \
        translate.h \
        voice.h \
        wave.h \
        msvc/speech.h

LIBS += -L$${PWD}/portaudio -lportaudio -lAdvapi32

DEFINES += USE_PORTAUDIO USE_ASYNC DEBUG_ENABLED

INCLUDEPATH += msvc
