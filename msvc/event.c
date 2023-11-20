/***************************************************************************
 *   Copyright (C) 2007, Gilles Casse <gcasse@oralux.org>                  *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 3 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/

#include "speech.h"

#ifdef USE_ASYNC
// This source file is only used for asynchronious modes

#include <windows.h>
#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

#include "speak_lib.h"
#include "event.h"
#include "wave.h"
#include "debug.h"

static int poll_start_req_val = 0;
static int poll_stop_req_val = 0;
static CONDITION_VARIABLE poll_start_req;
static CONDITION_VARIABLE poll_stop_req;
static CONDITION_VARIABLE poll_stop_ack;
static CRITICAL_SECTION poll_lock;
static HANDLE my_thread;
static int thread_inited;

static t_espeak_callback *my_callback = NULL;
static int my_event_is_running = 0;

enum {
    MIN_TIMEOUT_IN_MS = 10,
    ACTIVITY_TIMEOUT = 50, // in ms, check that the stream is active
    MAX_ACTIVITY_CHECK = 6
};


typedef struct t_node {
    void *data;
    struct t_node *next;
} node;

static node *head = NULL;
static node *tail = NULL;
static int node_counter = 0;

static espeak_ERROR push(void *data);

static void *pop();

static void init();

static DWORD polling_thread(LPVOID);

void event_set_callback(t_espeak_callback *SynthCallback) {
    my_callback = SynthCallback;
}

void event_init() {
    ENTER("event_init");
    my_event_is_running = 0;

    // security
    InitializeConditionVariable(&poll_start_req);
    InitializeConditionVariable(&poll_stop_req);
    InitializeConditionVariable(&poll_stop_ack);
    InitializeCriticalSection(&poll_lock);

    init();
    my_thread = CreateThread(
            NULL, // default security attributes
            0,    // default stack size
            (LPTHREAD_START_ROUTINE) polling_thread,
            NULL, // no thread function arguments
            0,    // default creation flags
            NULL);// receive thread identifier
    thread_inited = (my_thread != NULL);
    assert(my_thread != NULL);
}

static void event_display(espeak_EVENT *event) {
    ENTER("event_display");

#ifdef DEBUG_ENABLED
    if (event == NULL) {
        SHOW("event_display > event=%s\n", "NULL");
    } else {
        static const char *label[] = {
                "LIST_TERMINATED",
                "WORD",
                "SENTENCE",
                "MARK",
                "PLAY",
                "END",
                "MSG_TERMINATED",
                "PHONEME",
                "SAMPLERATE",
                "??"
        };

        SHOW("event_display > event=0x%x\n", event);
        SHOW("event_display >   type=%s\n", label[event->type]);
        SHOW("event_display >   uid=%d\n", event->unique_identifier);
        SHOW("event_display >   text_position=%d\n", event->text_position);
        SHOW("event_display >   length=%d\n", event->length);
        SHOW("event_display >   audio_position=%d\n", event->audio_position);
        SHOW("event_display >   sample=%d\n", event->sample);
        SHOW("event_display >   user_data=0x%x\n", event->user_data);
    }
#endif
}

static espeak_EVENT *event_copy(espeak_EVENT *event) {
    espeak_EVENT *a_event =
            (espeak_EVENT *) malloc(sizeof(espeak_EVENT));

    ENTER("event_copy");

    if (event == NULL) {
        return NULL;
    }

    if (a_event) {
        memcpy(a_event, event, sizeof(espeak_EVENT));
        switch (event->type) {
            case espeakEVENT_MARK:
            case espeakEVENT_PLAY:
                if (event->id.name) {
                    a_event->id.name = strdup(event->id.name);
                }
                break;
            default:
                break;
        }
    }

    event_display(a_event);

    return a_event;
}

// Call the user supplied callback
//
// Note: the current sequence is:
//
// * First call with: event->type = espeakEVENT_SENTENCE
// * 0, 1 or several calls: event->type = espeakEVENT_WORD
// * Last call: event->type = espeakEVENT_MSG_TERMINATED
//
static void event_notify(espeak_EVENT *event) {
    static unsigned int a_old_uid = 0;
    espeak_EVENT events[2];

    ENTER("event_notify");

    memcpy(&events[0], event,
           sizeof(espeak_EVENT));     // the event parameter in the callback function should be an array of eventd
    memcpy(&events[1], event, sizeof(espeak_EVENT));
    events[1].type = espeakEVENT_LIST_TERMINATED;           // ... terminated by an event type=0

    if (event && my_callback) {
        event_display(event);

        switch (event->type) {
            case espeakEVENT_SENTENCE:
                my_callback(NULL, 0, events);
                a_old_uid = event->unique_identifier;
                break;

            case espeakEVENT_MSG_TERMINATED:
            case espeakEVENT_MARK:
            case espeakEVENT_WORD:
            case espeakEVENT_END:
            case espeakEVENT_PHONEME: {
// jonsd - I'm not sure what this is for. gilles says it's for when Gnome Speech reads a file of blank lines
                if (a_old_uid != event->unique_identifier) {
                    espeak_EVENT_TYPE a_new_type = events[0].type;
                    events[0].type = espeakEVENT_SENTENCE;
                    my_callback(NULL, 0, events);
                    events[0].type = a_new_type;
                    espeakSleep(50);
                }
                my_callback(NULL, 0, events);
                a_old_uid = event->unique_identifier;
            }
                break;

            default:
            case espeakEVENT_LIST_TERMINATED:
            case espeakEVENT_PLAY:
                break;
        }
    }
}

static int event_delete(espeak_EVENT *event) {
    ENTER("event_delete");

    event_display(event);

    if (event == NULL) {
        return 0;
    }

    switch (event->type) {
        case espeakEVENT_MSG_TERMINATED:
            event_notify(event);
            break;

        case espeakEVENT_MARK:
        case espeakEVENT_PLAY:
            if (event->id.name) {
                free((void *) (event->id.name));
            }
            break;

        default:
            break;
    }

    free(event);
    return 1;
}

espeak_ERROR event_declare(espeak_EVENT *event) {
    espeak_EVENT *a_event = NULL;
    espeak_ERROR a_error = EE_OK;
    ENTER("event_declare");
    event_display(event);
    if (event==NULL) {
        return EE_INTERNAL_ERROR;
    }
    SHOW_TIME("event_declare > locked\n");
    EnterCriticalSection(&poll_lock);
    a_event = event_copy(event);
    a_error = push(a_event);
    if (a_error != EE_OK) {
        event_delete(a_event);
    }
    poll_start_req_val++;
    LeaveCriticalSection(&poll_lock);
    WakeConditionVariable(&poll_start_req);
    return a_error;
}

espeak_ERROR event_clear_all() {
    int a_event_is_running = 0;

    ENTER("event_clear_all");

    SHOW_TIME("event_stop > locked\n");
    EnterCriticalSection(&poll_lock);
    if (my_event_is_running) {
        poll_stop_req_val++;
        LeaveCriticalSection(&poll_lock);
        WakeConditionVariable(&poll_stop_req);
        a_event_is_running = 1;
    } else {
        LeaveCriticalSection(&poll_lock);
        init(); // clear pending events
    }
    if (a_event_is_running) {
        EnterCriticalSection(&poll_lock);
        while(poll_stop_req_val > 0){
            SleepConditionVariableCS(&poll_stop_ack, &poll_lock, INFINITE);
        }
        LeaveCriticalSection(&poll_lock);
    }
    SHOW_TIME("LEAVE event_stop\n");
    return EE_OK;
}

static int sleep_until_timeout_or_stop_request(uint32_t time_in_ms) {
    int stop_request = 0;

    ENTER("sleep_until_timeout_or_stop_request");

    EnterCriticalSection(&poll_lock);
    while (poll_stop_req_val <= 0) {
        SleepConditionVariableCS(
                &poll_stop_req, &poll_lock, time_in_ms);
        if(GetLastError() == ERROR_TIMEOUT){
            break;
        }
    }
    stop_request = poll_stop_req_val;
    LeaveCriticalSection(&poll_lock);
    return stop_request;
}

//>
//<get_remaining_time
// Asked for the time interval required for reaching the sample.
// If the stream is opened but the audio samples are not played,
// a timeout is started.

static int get_remaining_time(
        uint32_t sample, uint32_t *time_in_ms,
        int *stop_is_required) {
    int err = 0;
    int i = 0;
    *stop_is_required = 0;

    ENTER("get_remaining_time");

    for (i = 0; i < MAX_ACTIVITY_CHECK && (*stop_is_required == 0); i++) {
        err = wave_get_remaining_time(sample, time_in_ms);
        if (err || wave_is_busy(NULL) || (*time_in_ms == 0)) {
            // if err, stream not available: quit
            // if wave is busy, time_in_ms is known: quit
            // if wave is not busy but remaining time == 0, event is reached: quit
            break;
        }
        *stop_is_required = sleep_until_timeout_or_stop_request(ACTIVITY_TIMEOUT);
    }
    return err;
}


static DWORD polling_thread(LPVOID arg) {
    (void)arg;
    ENTER("polling_thread");
    while (1) {
        int stop_request = 0;
        EnterCriticalSection(&poll_lock);
        // running flag
        my_event_is_running = 0;
        // waif for start request
        while (poll_start_req_val <= 0) {
            SleepConditionVariableCS(&poll_start_req, &poll_lock, INFINITE);
        }
        poll_start_req_val--;
        // running flag
        my_event_is_running = 1;
        // check stop request
        stop_request = poll_stop_req_val;
        poll_stop_req_val = 0;
        LeaveCriticalSection(&poll_lock);

        // process event
        while (head && (stop_request <= 0)) {
            espeak_EVENT *event = NULL;
            uint32_t time_in_ms = 0;
            int err = EE_OK;
            // purge start request
            EnterCriticalSection(&poll_lock);
            poll_start_req_val = 0;
            LeaveCriticalSection(&poll_lock);

            event = (espeak_EVENT *) (head->data);
            assert(event);

            err = get_remaining_time(
                    (uint32_t) event->sample,
                    &time_in_ms, &stop_request);


            if (stop_request > 0) {
                break;
            } else if (err != 0) {
                EnterCriticalSection(&poll_lock);
                event_delete((espeak_EVENT *) pop());
                LeaveCriticalSection(&poll_lock);
            } else if (time_in_ms == 0) {
                // the event is already reached.
                if (my_callback) {
                    event_notify(event);
                    // the user_data (and the type) are cleaned to be sure
                    // that MSG_TERMINATED is called twice (at delete time too).
                    event->type = espeakEVENT_LIST_TERMINATED;
                    event->user_data = NULL;
                }

                EnterCriticalSection(&poll_lock);
                event_delete((espeak_EVENT *) pop());
                stop_request = poll_stop_req_val;
                poll_stop_req_val = 0;
                LeaveCriticalSection(&poll_lock);
            } else {
                // The event will be notified soon: sleep until timeout or stop request
                stop_request = sleep_until_timeout_or_stop_request(time_in_ms);
            }
        }

        EnterCriticalSection(&poll_lock);
        my_event_is_running = 0;
        if (stop_request <= 0) {
            stop_request = poll_stop_req_val;
            poll_stop_req_val = 0;
        }
        LeaveCriticalSection(&poll_lock);

        if (stop_request > 0) {
            // no mutex required since the stop command is synchronous
            // and waiting for my_sem_stop_is_acknowledged
            init();
            // acknowledge the stop request
            EnterCriticalSection(&poll_lock);
            poll_stop_req_val--;
            LeaveCriticalSection(&poll_lock);
            WakeConditionVariable(&poll_stop_ack);
        }
    }
}

enum {
    MAX_NODE_COUNTER = 1000
};

// return 1 if ok, 0 otherwise
static espeak_ERROR push(void *the_data) {
    node *n = NULL;

    ENTER("event > push");

    assert((!head && !tail) || (head && tail));

    if (the_data == NULL) {
        SHOW("event > push > event=0x%x\n", NULL);
        return EE_INTERNAL_ERROR;
    }

    if (node_counter >= MAX_NODE_COUNTER) {
        SHOW("event > push > %s\n", "EE_BUFFER_FULL");
        return EE_BUFFER_FULL;
    }

    n = (node *) malloc(sizeof(node));
    if (n == NULL) {
        return EE_INTERNAL_ERROR;
    }

    if (head == NULL) {
        head = n;
        tail = n;
    } else {
        tail->next = n;
        tail = n;
    }

    tail->next = NULL;
    tail->data = the_data;

    node_counter++;
    SHOW("event > push > counter=%d (uid=%d)\n", node_counter, ((espeak_EVENT *) the_data)->unique_identifier);

    return EE_OK;
}

static void *pop() {
    void *the_data = NULL;

    ENTER("event > pop");

    assert((!head && !tail) || (head && tail));

    if (head != NULL) {
        node *n = head;
        the_data = n->data;
        head = n->next;
        free(n);
        node_counter--;
        SHOW("event > pop > event=0x%x (counter=%d, uid=%d)\n",
             the_data, node_counter,
             ((espeak_EVENT *) the_data)->unique_identifier);
    }

    if (head == NULL) {
        tail = NULL;
    }
    return the_data;
}

static void init() {
    ENTER("event > init");
    while (event_delete((espeak_EVENT *) pop())) {}
    node_counter = 0;
}

void event_terminate() {
    ENTER("event_terminate");
    if (thread_inited) {
        poll_start_req_val++;
        poll_stop_req_val++;
        WakeAllConditionVariable(&poll_start_req);
        WakeAllConditionVariable(&poll_stop_req);
        TerminateThread(my_thread, 0);
        CloseHandle(my_thread);
        init(); // purge event
        thread_inited = 0;
    }
}

#endif

