/*
 * Copyright (C) 2010 Nokia Corporation.
 *
 * Contact: Maemo MMF Audio <mmf-audio@projects.maemo.org>
 *          or Jyri Sarha <jyri.sarha@nokia.com>
 *
 * These PulseAudio Modules are free software; you can redistribute
 * it and/or modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation
 * version 2.1 of the License.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301
 * USA.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <pulsecore/core-util.h>

#include "module-meego-parameters-userdata.h"
#include "module-meego-parameters-symdef.h"

#include <parameters.h>

#include <pulsecore/modules/meego/proplist-meego.h>
#include <pulsecore/modules/meego/module-voice-api.h>

PA_MODULE_AUTHOR("Pekka Ervasti");
PA_MODULE_DESCRIPTION("Meego parameters module");
PA_MODULE_USAGE("directory=<parameter directory> cache=<boolean>");
PA_MODULE_VERSION(PACKAGE_VERSION);

static const char* const valid_modargs[] = {
    "directory",
    "cache",
    NULL,
};

static void check_mode(pa_sink *s, struct userdata *u) {
    const char *tuning_alg;
    const char *mode, *hwid;

    mode = pa_proplist_gets(s->proplist, PA_NOKIA_PROP_AUDIO_MODE);
    hwid = pa_proplist_gets(s->proplist, PA_NOKIA_PROP_AUDIO_ACCESSORY_HWID);

    if (mode != NULL) {
        mode = pa_sprintf_malloc("%s%s", mode, (hwid?hwid:""));

        if ((tuning_alg = pa_proplist_gets(s->proplist, PA_NOKIA_PROP_AUDIO_TUNING)) != NULL) {
            if (pa_streq(tuning_alg, "update_mode"))
                update_mode(u, mode);
            else
                algorithm_reload(u, tuning_alg);
        }

        switch_mode(u, mode);
        pa_xfree((void*)mode);
    }
}

static pa_hook_result_t hw_sink_input_move_finish_cb(pa_core *c, pa_sink_input *i, struct userdata *u) {
    char *name = pa_proplist_gets(i->proplist, PA_PROP_MEDIA_NAME);

    if (i->sink && name && pa_streq(name, VOICE_MASTER_SINK_INPUT_NAME))
        check_mode(i->sink, u);

    return PA_HOOK_OK;
}

static pa_hook_result_t sink_proplist_changed_hook_callback(pa_core *c, pa_sink *s, struct userdata *u) {
    pa_sink_input *i;
    uint32_t idx;

    PA_IDXSET_FOREACH(i, s->inputs, idx) {
        char *name = pa_proplist_gets(i->proplist, PA_PROP_MEDIA_NAME);
        if (name && pa_streq(name, VOICE_MASTER_SINK_INPUT_NAME)) {
            check_mode(s, u);
            break;
        }
    }

    return PA_HOOK_OK;
}

int pa__init(pa_module *m) {
    pa_modargs *ma = NULL;
    struct userdata *u;

    u = pa_xnew0(struct userdata, 1);

    pa_assert(m);

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log("Failed to parse module arguments");
        goto fail;
    }

    m->userdata = u;
    u->core = m->core;
    u->module = m;

    u->parameters.directory = pa_xstrdup(pa_modargs_get_value(ma, "directory", "/var/lib/pulse-nokia"));

    if (pa_modargs_get_value_boolean(ma, "cache", &u->parameters.cache) < 0) {
        pa_log("cache= expects a boolean argument.");
        goto fail;
    }

    initme(u);

    u->sink_proplist_changed_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_PROPLIST_CHANGED], PA_HOOK_NORMAL, (pa_hook_cb_t) sink_proplist_changed_hook_callback, u);
    u->sink_input_move_finished_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_INPUT_MOVE_FINISH], PA_HOOK_NORMAL, (pa_hook_cb_t) hw_sink_input_move_finish_cb, u);

    pa_modargs_free(ma);

    return 0;

 fail:
    if (ma)
        pa_modargs_free(ma);

    pa_xfree(u);
    m->userdata = NULL;

    return -1;
}

void pa__done(pa_module *m) {
    struct userdata *u = m->userdata;

    assert(m);

    unloadme(u);

    pa_hook_slot_free(u->sink_proplist_changed_slot);
    pa_hook_slot_free(u->sink_input_move_finished_slot);

    if (u)
        pa_xfree(u);
}
