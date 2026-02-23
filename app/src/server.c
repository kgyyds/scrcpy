#include "server.h"

#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include "util/env.h"
#include "util/file.h"
#include "util/log.h"
#include "util/net_intr.h"
#include "util/process.h"
#include "util/str.h"

#define SC_SERVER_FILENAME "scrcpy-server"

#define SC_SOCKET_NAME_PREFIX "scrcpy_"

static const char *
log_level_to_server_string(enum sc_log_level level) {
    switch (level) {
        case SC_LOG_LEVEL_VERBOSE:
            return "verbose";
        case SC_LOG_LEVEL_DEBUG:
            return "debug";
        case SC_LOG_LEVEL_INFO:
            return "info";
        case SC_LOG_LEVEL_WARN:
            return "warn";
        case SC_LOG_LEVEL_ERROR:
            return "error";
        default:
            assert(!"unexpected log level");
            return NULL;
    }
}

/*
 * This function has been removed as it is no longer needed in TCP listen mode.
 * The server now uses a simple flag-based stopping mechanism.
 */


static const char *
sc_server_get_codec_name(enum sc_codec codec) {
    switch (codec) {
        case SC_CODEC_H264:
            return "h264";
        case SC_CODEC_H265:
            return "h265";
        case SC_CODEC_AV1:
            return "av1";
        case SC_CODEC_OPUS:
            return "opus";
        case SC_CODEC_AAC:
            return "aac";
        case SC_CODEC_FLAC:
            return "flac";
        case SC_CODEC_RAW:
            return "raw";
        default:
            assert(!"unexpected codec");
            return NULL;
    }
}

static const char *
sc_server_get_camera_facing_name(enum sc_camera_facing camera_facing) {
    switch (camera_facing) {
        case SC_CAMERA_FACING_FRONT:
            return "front";
        case SC_CAMERA_FACING_BACK:
            return "back";
        case SC_CAMERA_FACING_EXTERNAL:
            return "external";
        default:
            assert(!"unexpected camera facing");
            return NULL;
    }
}

static const char *
sc_server_get_audio_source_name(enum sc_audio_source audio_source) {
    switch (audio_source) {
        case SC_AUDIO_SOURCE_OUTPUT:
            return "output";
        case SC_AUDIO_SOURCE_MIC:
            return "mic";
        case SC_AUDIO_SOURCE_PLAYBACK:
            return "playback";
        case SC_AUDIO_SOURCE_MIC_UNPROCESSED:
            return "mic-unprocessed";
        case SC_AUDIO_SOURCE_MIC_CAMCORDER:
            return "mic-camcorder";
        case SC_AUDIO_SOURCE_MIC_VOICE_RECOGNITION:
            return "mic-voice-recognition";
        case SC_AUDIO_SOURCE_MIC_VOICE_COMMUNICATION:
            return "mic-voice-communication";
        case SC_AUDIO_SOURCE_VOICE_CALL:
            return "voice-call";
        case SC_AUDIO_SOURCE_VOICE_CALL_UPLINK:
            return "voice-call-uplink";
        case SC_AUDIO_SOURCE_VOICE_CALL_DOWNLINK:
            return "voice-call-downlink";
        case SC_AUDIO_SOURCE_VOICE_PERFORMANCE:
            return "voice-performance";
        default:
            assert(!"unexpected audio source");
            return NULL;
    }
}

static const char *
sc_server_get_display_ime_policy_name(enum sc_display_ime_policy policy) {
    switch (policy) {
        case SC_DISPLAY_IME_POLICY_LOCAL:
            return "local";
        case SC_DISPLAY_IME_POLICY_FALLBACK:
            return "fallback";
        case SC_DISPLAY_IME_POLICY_HIDE:
            return "hide";
        default:
            assert(!"unexpected display IME policy");
            return NULL;
    }
}

bool
sc_server_init(struct sc_server *server, const struct sc_server_params *params,
              const struct sc_server_callbacks *cbs, void *cbs_userdata) {
    // The allocated data in params (const char *) must remain valid until the
    // end of the program
    server->params = *params;

    bool ok = sc_mutex_init(&server->mutex);
    if (!ok) {
        return false;
    }

    ok = sc_cond_init(&server->cond_stopped);
    if (!ok) {
        sc_mutex_destroy(&server->mutex);
        return false;
    }

    ok = sc_intr_init(&server->intr);
    if (!ok) {
        sc_cond_destroy(&server->cond_stopped);
        sc_mutex_destroy(&server->mutex);
        return false;
    }

    server->serial = NULL;
    server->device_socket_name = NULL;
    server->stopped = false;

    server->video_listen_socket = SC_SOCKET_NONE;
    server->control_listen_socket = SC_SOCKET_NONE;
    server->video_socket = SC_SOCKET_NONE;
    server->audio_socket = SC_SOCKET_NONE;
    server->control_socket = SC_SOCKET_NONE;

    assert(cbs);
    assert(cbs->on_connection_failed);
    assert(cbs->on_connected);
    assert(cbs->on_disconnected);

    server->cbs = cbs;
    server->cbs_userdata = cbs_userdata;

    return true;
}

static bool
device_read_info(struct sc_intr *intr, sc_socket device_socket,
                 struct sc_server_info *info) {
    uint8_t buf[SC_DEVICE_NAME_FIELD_LENGTH];
    ssize_t r = net_recv_all_intr(intr, device_socket, buf, sizeof(buf));
    if (r < SC_DEVICE_NAME_FIELD_LENGTH) {
        LOGE("Could not retrieve device information");
        return false;
    }
    // in case the client sends garbage
    buf[SC_DEVICE_NAME_FIELD_LENGTH - 1] = '\0';
    memcpy(info->device_name, (char *) buf, sizeof(info->device_name));

    return true;
}

static bool
sc_server_connect_to(struct sc_server *server, struct sc_server_info *info) {
    const char *serial = server->serial;
    assert(serial);

    bool video = server->params.video;
    bool audio = server->params.audio;
    bool control = server->params.control;

    sc_socket video_socket = SC_SOCKET_NONE;
    sc_socket audio_socket = SC_SOCKET_NONE;
    sc_socket control_socket = SC_SOCKET_NONE;

    // TCP listen mode: PC listens for device connections
    if (server->params.tcp_listen) {
        LOGI("TCP listen mode: waiting for device connection...");

        sc_socket video_listen_socket = SC_SOCKET_NONE;
        sc_socket control_listen_socket = SC_SOCKET_NONE;

        // Parse listen address
        uint32_t listen_addr = INADDR_ANY;  // 0.0.0.0 - all interfaces
        if (server->params.listen_address) {
            if (!net_parse_ipv4(server->params.listen_address, &listen_addr)) {
                LOGE("Invalid listen address: %s", server->params.listen_address);
                goto fail;
            }
        }

        // Create and bind video/audio listening socket
        if (video || audio) {
            uint16_t port = server->params.listen_video_port;
            if (!port) {
                LOGE("Video/audio stream enabled but listen_video_port not set");
                goto fail;
            }
            video_listen_socket = net_socket();
            if (video_listen_socket == SC_SOCKET_NONE) {
                LOGE("Could not create video listen socket");
                goto fail;
            }
            bool ok = net_listen_intr(&server->intr, video_listen_socket,
                                      listen_addr, port, 1);
            if (!ok) {
                LOGE("Could not listen on video port %" PRIu16, port);
                net_close(video_listen_socket);
                goto fail;
            }
            LOGI("Listening on video port %" PRIu16 " (address: %s)",
                 port, server->params.listen_address ? server->params.listen_address : "0.0.0.0");
            video_socket = net_accept_intr(&server->intr, video_listen_socket);
            net_close(video_listen_socket);  // Close listening socket after accepting
            if (video_socket == SC_SOCKET_NONE) {
                LOGE("Could not accept video connection");
                goto fail;
            }
            LOGI("Device connected for video/audio stream");
            audio_socket = video_socket;  // Audio shares the same socket as video
        }

        // Create and bind control listening socket
        if (control) {
            uint16_t port = server->params.listen_control_port;
            if (!port) {
                LOGE("Control stream enabled but listen_control_port not set");
                goto fail;
            }
            control_listen_socket = net_socket();
            if (control_listen_socket == SC_SOCKET_NONE) {
                LOGE("Could not create control listen socket");
                goto fail;
            }
            bool ok = net_listen_intr(&server->intr, control_listen_socket,
                                      listen_addr, port, 1);
            if (!ok) {
                LOGE("Could not listen on control port %" PRIu16, port);
                net_close(control_listen_socket);
                goto fail;
            }
            LOGI("Listening on control port %" PRIu16 " (address: %s)",
                 port, server->params.listen_address ? server->params.listen_address : "0.0.0.0");
            control_socket = net_accept_intr(&server->intr, control_listen_socket);
            net_close(control_listen_socket);  // Close listening socket after accepting
            if (control_socket == SC_SOCKET_NONE) {
                LOGE("Could not accept control connection");
                goto fail;
            }
            LOGI("Device connected for control stream");
        }

        // Disable Nagle's algorithm for the control socket
        if (control_socket != SC_SOCKET_NONE) {
            bool ok = net_set_tcp_nodelay(control_socket, true);
            (void) ok;  // error already logged
        }

        // Assign accepted sockets to server structure
        server->video_socket = video_socket;
        server->audio_socket = audio_socket;
        server->control_socket = control_socket;

        // The device sends its info
        sc_socket first_socket = video ? video_socket
                               : audio ? audio_socket
                                       : control_socket;
        if (!device_read_info(&server->intr, first_socket, info)) {
            LOGE("Could not retrieve device info");
            goto fail;
        }

        LOGI("Connected to device: %s", info->device_name);
        return true;
    }

    // Legacy listen-only mode (client-listen-video-port mode)
    if (server->params.listen_only || server->params.client_listen_video_port || server->params.client_listen_control_port) {
        LOGD("Client listening for device connection");

        sc_socket video_server_socket = SC_SOCKET_NONE;
        sc_socket audio_server_socket = SC_SOCKET_NONE;
        sc_socket control_server_socket = SC_SOCKET_NONE;

        if (video) {
            uint16_t port = server->params.client_listen_video_port;
            if (!port) {
                LOGE("Video stream enabled but client_listen_video_port not set");
                goto fail;
            }
            video_server_socket = net_socket();
            if (video_server_socket == SC_SOCKET_NONE) {
                LOGE("Could not create video server socket");
                goto fail;
            }
            bool ok = net_listen_intr(&server->intr, video_server_socket, IPV4_LOCALHOST, port, 1);
            if (!ok) {
                LOGE("Could not listen on video port %" PRIu16, port);
                net_close(video_server_socket);
                goto fail;
            }
            LOGI("Listening on video port %" PRIu16, port);
            video_socket = net_accept_intr(&server->intr, video_server_socket);
            net_close(video_server_socket); // Close listening socket after accepting
            if (video_socket == SC_SOCKET_NONE) {
                LOGE("Could not accept video connection");
                goto fail;
            }
        }

        if (audio) {
            // For now, audio uses the control port if no dedicated audio port is provided
            uint16_t port = server->params.client_listen_control_port;
            if (!port) {
                LOGE("Audio stream enabled but client_listen_control_port not set for audio");
                goto fail;
            }
            audio_server_socket = net_socket();
            if (audio_server_socket == SC_SOCKET_NONE) {
                LOGE("Could not create audio server socket");
                goto fail;
            }
            bool ok = net_listen_intr(&server->intr, audio_server_socket, IPV4_LOCALHOST, port, 1);
            if (!ok) {
                LOGE("Could not listen on audio port %" PRIu16, port);
                net_close(audio_server_socket);
                goto fail;
            }
            LOGI("Listening on audio port %" PRIu16, port);
            audio_socket = net_accept_intr(&server->intr, audio_server_socket);
            net_close(audio_server_socket); // Close listening socket after accepting
            if (audio_socket == SC_SOCKET_NONE) {
                LOGE("Could not accept audio connection");
                goto fail;
            }
        }

        if (control) {
            uint16_t port = server->params.client_listen_control_port;
            if (!port) {
                LOGE("Control stream enabled but client_listen_control_port not set");
                goto fail;
            }
            control_server_socket = net_socket();
            if (control_server_socket == SC_SOCKET_NONE) {
                LOGE("Could not create control server socket");
                goto fail;
            }
            bool ok = net_listen_intr(&server->intr, control_server_socket, IPV4_LOCALHOST, port, 1);
            if (!ok) {
                LOGE("Could not listen on control port %" PRIu16, port);
                net_close(control_server_socket);
                goto fail;
            }
            LOGI("Listening on control port %" PRIu16, port);
            control_socket = net_accept_intr(&server->intr, control_server_socket);
            net_close(control_server_socket); // Close listening socket after accepting
            if (control_socket == SC_SOCKET_NONE) {
                LOGE("Could not accept control connection");
                goto fail;
            }
        }

        // Assign accepted sockets to server structure
        server->video_socket = video_socket;
        server->audio_socket = audio_socket;
        server->control_socket = control_socket;

        // The server sends its infos
        sc_socket first_socket = video ? video_socket
                               : audio ? audio_socket
                                       : control_socket;
        if (!device_read_info(&server->intr, first_socket, info)) {
            LOGE("Could not retrieve server info");
            goto fail;
        }

        return true;
    }

    // ADB tunnel mode has been removed. This code path should never be reached.
    LOGE("ADB tunnel mode is no longer supported");
    goto fail;

fail:
    if (video_socket != SC_SOCKET_NONE) {
        if (!net_close(video_socket)) {
            LOGW("Could not close video socket");
        }
    }

    if (audio_socket != SC_SOCKET_NONE) {
        if (!net_close(audio_socket)) {
            LOGW("Could not close audio socket");
        }
    }

    if (control_socket != SC_SOCKET_NONE) {
        if (!net_close(control_socket)) {
            LOGW("Could not close control socket");
        }
    }

    return false;
}

static void
sc_server_on_terminated(void *userdata) {
    struct sc_server *server = userdata;

    sc_intr_interrupt(&server->intr);
    server->cbs->on_disconnected(server, server->cbs_userdata);
    LOGD("Server terminated");
}

static int
run_server(void *data) {
    struct sc_server *server = data;

    const struct sc_server_params *params = &server->params;

    // TCP listen mode: PC listens for device connections, no adb required
    if (params->tcp_listen) {
        LOGI("TCP listen mode: PC will listen on configured ports");
        LOGI("Device should connect to PC's IP address on the configured ports");

        // Set a dummy serial (not used in TCP listen mode)
        server->serial = strdup("tcp-listen");
        if (!server->serial) {
            LOG_OOM();
            goto error_connection_failed;
        }

        // Set a dummy device_socket_name
        int r = asprintf(&server->device_socket_name, SC_SOCKET_NAME_PREFIX "%08x",
                         params->scid);
        if (r == -1) {
            LOG_OOM();
            goto error_connection_failed;
        }

        // Wait for device connection
        bool ok = sc_server_connect_to(server, &server->info);
        if (!ok) {
            goto error_connection_failed;
        }

        // Now connected
        server->cbs->on_connected(server, server->cbs_userdata);

        // Wait for server_stop()
        sc_mutex_lock(&server->mutex);
        while (!server->stopped) {
            sc_cond_wait(&server->cond_stopped, &server->mutex);
        }
        sc_mutex_unlock(&server->mutex);

        // Interrupt sockets to wake up socket blocking calls on the server
        if (server->video_socket != SC_SOCKET_NONE) {
            net_interrupt(server->video_socket);
        }
        if (server->audio_socket != SC_SOCKET_NONE) {
            net_interrupt(server->audio_socket);
        }
        if (server->control_socket != SC_SOCKET_NONE) {
            net_interrupt(server->control_socket);
        }

        return 0;
    }

    // Legacy listen-only mode (deprecated, kept for compatibility)
    if (params->listen_only) {
        LOGI("Legacy listen-only mode: waiting for device connection...");
        // Validate required parameters
        if (!params->client_listen_video_port) {
            LOGE("Listen-only mode requires --client-listen-video-port");
            goto error_connection_failed;
        }
        if (!params->server_host) {
            LOGE("Listen-only mode requires --server-host");
            goto error_connection_failed;
        }

        // Set a dummy serial (not used in listen-only mode)
        server->serial = strdup("listen-only");
        if (!server->serial) {
            LOG_OOM();
            goto error_connection_failed;
        }

        // Set a dummy device_socket_name
        int r = asprintf(&server->device_socket_name, SC_SOCKET_NAME_PREFIX "%08x",
                         params->scid);
        if (r == -1) {
            LOG_OOM();
            goto error_connection_failed;
        }

        // Directly wait for device connection
        bool ok = sc_server_connect_to(server, &server->info);
        if (!ok) {
            goto error_connection_failed;
        }

        // Now connected
        server->cbs->on_connected(server, server->cbs_userdata);

        // Wait for server_stop()
        sc_mutex_lock(&server->mutex);
        while (!server->stopped) {
            sc_cond_wait(&server->cond_stopped, &server->mutex);
        }
        sc_mutex_unlock(&server->mutex);

        // Interrupt sockets to wake up socket blocking calls on the server
        if (server->video_socket != SC_SOCKET_NONE) {
            net_interrupt(server->video_socket);
        }
        if (server->audio_socket != SC_SOCKET_NONE) {
            net_interrupt(server->audio_socket);
        }
        if (server->control_socket != SC_SOCKET_NONE) {
            net_interrupt(server->control_socket);
        }

        return 0;
    }

    // ADB mode has been removed. TCP listen mode is now the only supported mode.
    LOGE("ADB mode has been removed. Please use --tcp-listen mode.");
    LOGE("Usage: scrcpy --tcp-listen --listen-video-port=27183 --listen-control-port=27184");
    goto error_connection_failed;

error_connection_failed:
    server->cbs->on_connection_failed(server, server->cbs_userdata);
    return -1;
}

bool
sc_server_start(struct sc_server *server) {
    bool ok =
        sc_thread_create(&server->thread, run_server, "scrcpy-server", server);
    if (!ok) {
        LOGE("Could not create server thread");
        return false;
    }

    return true;
}

void
sc_server_stop(struct sc_server *server) {
    sc_mutex_lock(&server->mutex);
    server->stopped = true;
    sc_cond_signal(&server->cond_stopped);
    sc_intr_interrupt(&server->intr);
    sc_mutex_unlock(&server->mutex);
}

void
sc_server_join(struct sc_server *server) {
    sc_thread_join(&server->thread, NULL);
}

void
sc_server_destroy(struct sc_server *server) {
    if (server->video_socket != SC_SOCKET_NONE) {
        net_close(server->video_socket);
    }
    if (server->audio_socket != SC_SOCKET_NONE) {
        net_close(server->audio_socket);
    }
    if (server->control_socket != SC_SOCKET_NONE) {
        net_close(server->control_socket);
    }

    free(server->serial);
    free(server->device_socket_name);
    sc_intr_destroy(&server->intr);
    sc_cond_destroy(&server->cond_stopped);
    sc_mutex_destroy(&server->mutex);
}
