#include "file_pusher.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "util/log.h"

#define DEFAULT_PUSH_TARGET "/sdcard/Download/"

static void
sc_file_pusher_request_destroy(struct sc_file_pusher_request *req) {
    free(req->file);
}

bool
sc_file_pusher_init(struct sc_file_pusher *fp, const char *serial,
                    const char *push_target) {
    (void) serial;  // unused in TCP listen mode

    sc_vecdeque_init(&fp->queue);

    bool ok = sc_mutex_init(&fp->mutex);
    if (!ok) {
        return false;
    }

    ok = sc_cond_init(&fp->event_cond);
    if (!ok) {
        sc_mutex_destroy(&fp->mutex);
        return false;
    }

    ok = sc_intr_init(&fp->intr);
    if (!ok) {
        sc_cond_destroy(&fp->event_cond);
        sc_mutex_destroy(&fp->mutex);
        return false;
    }

    // Serial is not used in TCP listen mode
    fp->serial = NULL;

    // lazy initialization
    fp->initialized = false;

    fp->stopped = false;

    fp->push_target = push_target ? push_target : DEFAULT_PUSH_TARGET;

    LOGW("File pusher is disabled (ADB not available)");

    return true;
}

void
sc_file_pusher_destroy(struct sc_file_pusher *fp) {
    sc_cond_destroy(&fp->event_cond);
    sc_mutex_destroy(&fp->mutex);
    sc_intr_destroy(&fp->intr);
    free(fp->serial);

    while (!sc_vecdeque_is_empty(&fp->queue)) {
        struct sc_file_pusher_request *req = sc_vecdeque_popref(&fp->queue);
        assert(req);
        sc_file_pusher_request_destroy(req);
    }
}

bool
sc_file_pusher_request(struct sc_file_pusher *fp,
                       enum sc_file_pusher_action action, char *file) {
    (void) fp;
    (void) action;
    (void) file;

    LOGE("File pusher is not available in TCP listen mode");
    LOGE("ADB is required for file pushing and APK installation");

    // Free the file since the caller transfers ownership
    free(file);

    return false;  // Always fail since ADB is not available
}

static int
run_file_pusher(void *data) {
    (void) data;
    // File pusher thread does nothing in TCP listen mode
    LOGD("File pusher thread started (no-op, ADB not available)");
    return 0;
}

bool
sc_file_pusher_start(struct sc_file_pusher *fp) {
    LOGD("Starting file_pusher thread (no-op)");

    bool ok = sc_thread_create(&fp->thread, run_file_pusher, "scrcpy-file", fp);
    if (!ok) {
        LOGE("Could not start file_pusher thread");
        return false;
    }

    return true;
}

void
sc_file_pusher_stop(struct sc_file_pusher *fp) {
    if (fp->initialized) {
        sc_mutex_lock(&fp->mutex);
        fp->stopped = true;
        sc_cond_signal(&fp->event_cond);
        sc_intr_interrupt(&fp->intr);
        sc_mutex_unlock(&fp->mutex);
    }
}

void
sc_file_pusher_join(struct sc_file_pusher *fp) {
    if (fp->initialized) {
        sc_thread_join(&fp->thread, NULL);
    }
}
