/*
 * mod_audio_stream FreeSWITCH module to stream audio to websocket and receive response
 */
#include "mod_audio_stream.h"
#include "audio_streamer_glue.h"

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_audio_stream_shutdown);
SWITCH_MODULE_RUNTIME_FUNCTION(mod_audio_stream_runtime);
SWITCH_MODULE_LOAD_FUNCTION(mod_audio_stream_load);

SWITCH_MODULE_DEFINITION(mod_audio_stream, mod_audio_stream_load, mod_audio_stream_shutdown, NULL /*mod_audio_stream_runtime*/);

static void responseHandler(switch_core_session_t* session, const char* eventName, const char* json) {
    switch_event_t *event;
    
    switch_channel_t *channel = switch_core_session_get_channel(session);
    switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, eventName);
    switch_channel_event_set_data(channel, event);
    if (json) {
        switch_event_add_body(event, "%s", json);
        const char *input_string = json;
        const char *detect_result_position = strstr(input_string, "detect result");
        if (detect_result_position != NULL) {
            const char *colon_position = strchr(input_string, ':');
            if (colon_position != NULL) {
        // Extract the substring after the colon, skipping any leading spaces
                const char *result = colon_position + 1;
                while (*result == ' ' || *result == '\t') {
            // Skip leading spaces or tabs
                    result++;}
                //switch_channel_set_variable(channel,"detect",result);
                //switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "1111detect is %s\n",result);
        }
        }

    }

    switch_event_fire(&event);
}

static switch_bool_t capture_callback(switch_media_bug_t *bug, void *user_data, switch_abc_type_t type)
{
    switch_core_session_t *session = switch_core_media_bug_get_session(bug);

    switch (type) {
        case SWITCH_ABC_TYPE_INIT:
            break;

        case SWITCH_ABC_TYPE_CLOSE:
            {
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "Got SWITCH_ABC_TYPE_CLOSE.\n");
                stream_session_cleanup(session, NULL, 1);
            }
            break;

        case SWITCH_ABC_TYPE_READ:
            return stream_frame(bug);
            break;

        case SWITCH_ABC_TYPE_WRITE:
        default:
            break;
    }

    return SWITCH_TRUE;
}

static switch_status_t start_capture(switch_core_session_t *session,
                                     switch_media_bug_flag_t flags,
                                     char* wsUri,
                                     int sampling,
                                     char* metadata)
{
    switch_channel_t *channel = switch_core_session_get_channel(session);
    switch_media_bug_t *bug;
    switch_status_t status;
    switch_codec_t* read_codec;

    void *pUserData = NULL;
    int channels = (flags & SMBF_STEREO) ? 2 : 1;

    if (switch_channel_get_private(channel, MY_BUG_NAME)) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "mod_audio_stream: bug already attached!\n");
        return SWITCH_STATUS_FALSE;
    }

    read_codec = switch_core_session_get_read_codec(session);

    if (switch_channel_pre_answer(channel) != SWITCH_STATUS_SUCCESS) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "mod_audio_stream: channel must have reached pre-answer status before calling start!\n");
        return SWITCH_STATUS_FALSE;
    }

    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "calling stream_session_init.\n");
    if (SWITCH_STATUS_FALSE == stream_session_init(session, responseHandler, read_codec->implementation->actual_samples_per_second,
                                                 wsUri, sampling, channels, metadata, &pUserData)) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Error initializing mod_audio_stream session.\n");
        return SWITCH_STATUS_FALSE;
    }
    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "adding bug.\n");
    if ((status = switch_core_media_bug_add(session, MY_BUG_NAME, NULL, capture_callback, pUserData, 0, flags, &bug)) != SWITCH_STATUS_SUCCESS) {
        return status;
    }
    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "setting bug private data.\n");
    switch_channel_set_private(channel, MY_BUG_NAME, bug);

    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "exiting start_capture.\n");
    return SWITCH_STATUS_SUCCESS;
}

static switch_status_t do_stop(switch_core_session_t *session, char* text)
{
    switch_status_t status = SWITCH_STATUS_SUCCESS;

    if (text) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "mod_audio_stream: stop w/ final text %s\n", text);
    }
    else {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "mod_audio_stream: stop\n");
    }
    status = stream_session_cleanup(session, text, 0);

    return status;
}

static switch_status_t do_pauseresume(switch_core_session_t *session, int pause)
{
    switch_status_t status = SWITCH_STATUS_SUCCESS;

    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "mod_audio_stream: %s\n", pause ? "pause" : "resume");
    status = stream_session_pauseresume(session, pause);

    return status;
}

static switch_status_t send_text(switch_core_session_t *session, char* text) {
    switch_status_t status = SWITCH_STATUS_FALSE;
    switch_channel_t *channel = switch_core_session_get_channel(session);
    switch_media_bug_t *bug = switch_channel_get_private(channel, MY_BUG_NAME);

    if (bug) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "mod_audio_stream: sending text: %s.\n", text);
        status = stream_session_send_text(session, text);
    }
    else {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "mod_audio_stream: no bug, failed sending text: %s.\n", text);
    }
    return status;
}

#define STREAM_API_SYNTAX "<uuid> [start | stop | send_text | pause | resume | graceful-shutdown ] [wss-url | path] [mono | mixed | stereo] [8000 | 16000] [metadata]"
SWITCH_STANDARD_API(stream_function)
{
    char *mycmd = NULL, *argv[6] = { 0 };
    int argc = 0;

    switch_status_t status = SWITCH_STATUS_FALSE;

    if (!zstr(cmd) && (mycmd = strdup(cmd))) {
        argc = switch_separate_string(mycmd, ' ', argv, (sizeof(argv) / sizeof(argv[0])));
    }
    assert(cmd);
    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "mod_audio_stream cmd: %s\n", cmd ? cmd : "");

    if (zstr(cmd) || argc < 2 || (0 == strcmp(argv[1], "start") && argc < 4)) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Error with command %s %s %s.\n", cmd, argv[0], argv[1]);
        stream->write_function(stream, "-USAGE: %s\n", STREAM_API_SYNTAX);
        goto done;
    } else {
        switch_core_session_t *lsession = NULL;
        if ((lsession = switch_core_session_locate(argv[0]))) {
            if (!strcasecmp(argv[1], "stop")) {
                if(argc > 2 && (is_valid_utf8(argv[2]) != SWITCH_STATUS_SUCCESS)) {
                    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                                      "%s contains invalid utf8 characters\n", argv[2]);
                    switch_core_session_rwunlock(lsession);
                    goto done;
                }
                status = do_stop(lsession, argc > 2 ? argv[2] : NULL);
            } else if (!strcasecmp(argv[1], "pause")) {
                status = do_pauseresume(lsession, 1);
            } else if (!strcasecmp(argv[1], "resume")) {
                status = do_pauseresume(lsession, 0);
            } else if (!strcasecmp(argv[1], "send_text")) {
                if (argc < 3) {
                    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                                      "send_text requires an argument specifying text to send\n");
                    switch_core_session_rwunlock(lsession);
                    goto done;
                }
                if(is_valid_utf8(argv[2]) != SWITCH_STATUS_SUCCESS) {
                    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                                      "%s contains invalid utf8 characters\n", argv[2]);
                    switch_core_session_rwunlock(lsession);
                    goto done;
                }
                status = send_text(lsession, argv[2]);
            } else if (!strcasecmp(argv[1], "start")) {
                //switch_channel_t *channel = switch_core_session_get_channel(lsession);
                char wsUri[MAX_WS_URI];
                int sampling = 8000;
                switch_media_bug_flag_t flags = SMBF_READ_STREAM;
                char *metadata = argc > 5 ? argv[5] : NULL;
                if(metadata && (is_valid_utf8(argv[2]) != SWITCH_STATUS_SUCCESS)) {
                    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                                      "%s contains invalid utf8 characters\n", argv[2]);
                    switch_core_session_rwunlock(lsession);
                    goto done;
                }
                if (0 == strcmp(argv[3], "mixed")) {
                    flags |= SMBF_WRITE_STREAM;
                } else if (0 == strcmp(argv[3], "stereo")) {
                    flags |= SMBF_WRITE_STREAM;
                    flags |= SMBF_STEREO;
                } else if (0 != strcmp(argv[3], "mono")) {
                    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                                      "invalid mix type: %s, must be mono, mixed, or stereo\n", argv[3]);
                    switch_core_session_rwunlock(lsession);
                    goto done;
                }
                if (0 == strcmp(argv[4], "16k")) {
                    sampling = 16000;
                } else if (0 == strcmp(argv[4], "8k")) {
                    sampling = 8000;
                } else {
                    sampling = atoi(argv[4]);
                }
                if (!validate_ws_uri(argv[2], &wsUri[0])) {
                    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                                      "invalid websocket uri: %s\n", argv[2]);
                } else if (sampling % 8000 != 0) {
                    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                                      "invalid sample rate: %s\n", argv[4]);
                } else {
                    status = start_capture(lsession, flags, wsUri, sampling, metadata);
                }
            } else {
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                                  "unsupported mod_audio_stream cmd: %s\n", argv[1]);
            }
            switch_core_session_rwunlock(lsession);
        } else {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Error locating session %s\n",
                              argv[0]);
        }
    }

    if (status == SWITCH_STATUS_SUCCESS) {
        stream->write_function(stream, "+OK Success\n");
    } else {
        stream->write_function(stream, "-ERR Operation Failed\n");
    }

done:
    switch_safe_free(mycmd);
    return SWITCH_STATUS_SUCCESS;
}

SWITCH_STANDARD_APP(audio_stream_function)
{
    switch_status_t status = SWITCH_STATUS_FALSE;
    char wsUri[MAX_WS_URI] = "ws://localhost:8000";
    int sampling = 8000;
    switch_media_bug_flag_t flags = SMBF_READ_STREAM;
    char *metadata = switch_core_session_get_uuid(session);
    status = start_capture(session, flags, wsUri, sampling, metadata);
    if (status == SWITCH_STATUS_SUCCESS) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
        "websocket success info: %s\n", metadata);
    } else {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
        "websocket fail info: %s\n", metadata);
    }
}
SWITCH_STANDARD_APP(wait_for_detector_answer_function)
{
    int sleep_time;
    int count_time;
    char *sleep_data= NULL;
    count_time = 0;
    switch_channel_t *channel = switch_core_session_get_channel(session);

    sleep_data = switch_core_session_strdup(session, data);
    // Check if data is empty or NULL
    if (sleep_data == NULL || strcmp(sleep_data, "") == 0) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "No sleep_time provided\n");
        switch_channel_hangup(channel, SWITCH_CAUSE_NORMAL_CLEARING);
    }

    sleep_time = atoi(sleep_data);
    // Check if sleep_time is valid
    if (sleep_time <= 0) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Invalid sleep_time\n");
        switch_channel_hangup(channel, SWITCH_CAUSE_NORMAL_CLEARING);
    }
    else{
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Waiting for call answer time is %d\n", sleep_time);


	}
    while (!switch_channel_test_flag(channel, CF_ANSWERED) && switch_channel_ready(channel)) {
		switch_ivr_sleep(session, 100, SWITCH_TRUE, NULL);
        if (switch_channel_get_variable(channel, "detect")!= NULL){

            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Waiting for answer det_result is %s\n", switch_channel_get_variable(channel, "detect"));
            if (strcmp(switch_channel_get_variable(channel, "detect"), "6") == 0){
                count_time += 200;
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Waiting for call answer time is %d\n", count_time);
                if (count_time >= sleep_time) {
                    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Total waiting time exceeded sleep time\n");
                    switch_channel_hangup(channel, SWITCH_CAUSE_NORMAL_CLEARING);
                    break;
                }
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Call result is ringring\n" );
                switch_ivr_sleep(session, 100, SWITCH_TRUE, NULL);
		    }
		    else if (strcmp(switch_channel_get_variable(channel, "detect"), "9") == 0)
		    {
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Call result is unknow\n" );
                switch_ivr_sleep(session, 100, SWITCH_TRUE, NULL);
		    }
	        else{
			    switch_channel_hangup(channel, SWITCH_CAUSE_NORMAL_CLEARING);
			    break;
		    }


        }
	}

    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Waiting for answer time is %d\n", sleep_time);

}




SWITCH_MODULE_LOAD_FUNCTION(mod_audio_stream_load)
{
    switch_api_interface_t *api_interface;
    switch_application_interface_t *app_interface = NULL;

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_audio_stream API loading..\n");

    /* connect my internal structure to the blank pointer passed to me */
    *module_interface = switch_loadable_module_create_module_interface(pool, modname);

    /* create/register custom event message types */
    if (switch_event_reserve_subclass(EVENT_JSON) != SWITCH_STATUS_SUCCESS ||
        switch_event_reserve_subclass(EVENT_CONNECT) != SWITCH_STATUS_SUCCESS ||
        switch_event_reserve_subclass(EVENT_ERROR) != SWITCH_STATUS_SUCCESS ||
        switch_event_reserve_subclass(EVENT_DISCONNECT) != SWITCH_STATUS_SUCCESS) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Couldn't register an event subclass for mod_audio_stream API.\n");
        return SWITCH_STATUS_TERM;
    }
    SWITCH_ADD_API(api_interface, "uuid_audio_stream", "audio_stream API", stream_function, STREAM_API_SYNTAX);
    switch_console_set_complete("add uuid_audio_stream start wss-url metadata");
    switch_console_set_complete("add uuid_audio_stream start wss-url");
    switch_console_set_complete("add uuid_audio_stream stop");
    SWITCH_ADD_APP(app_interface, "audio_stream", "Send media to robot recording server", "Send media to robot recording server",
	audio_stream_function, "[path]", SAF_NONE);
    SWITCH_ADD_APP(app_interface, "wait_for_detector_answer", "Wait for call to be answered", "Wait for call to be answered.", wait_for_detector_answer_function, "", SAF_SUPPORT_NOMEDIA);



    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_audio_stream API successfully loaded\n");

    /* indicate that the module should continue to be loaded */
    return SWITCH_STATUS_SUCCESS;
}

/*
  Called when the system shuts down
  Macro expands to: switch_status_t mod_audio_stream_shutdown() */
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_audio_stream_shutdown)
{
    switch_event_free_subclass(EVENT_JSON);
    switch_event_free_subclass(EVENT_CONNECT);
    switch_event_free_subclass(EVENT_DISCONNECT);
    switch_event_free_subclass(EVENT_ERROR);

    return SWITCH_STATUS_SUCCESS;
}
