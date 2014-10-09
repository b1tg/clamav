/*
 *  Support for matcher using PCRE
 *
 *  Copyright (C) 2007-2013 Sourcefire, Inc.
 *  Copyright (C) 2014 Cisco Systems, Inc.
 *  All Rights Reserved.
 *
 *  Authors: Kevin Lin
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 *  MA 02110-1301, USA.
 */

#if HAVE_CONFIG_H
#include "clamav-config.h"
#endif

#include "clamav.h"
#include "cltypes.h"
#include "dconf.h"
#include "events.h"
#include "others.h"
#include "matcher.h"
#include "matcher-ac.h"
#include "matcher-pcre.h"
#include "mpool.h"
#include "regex_pcre.h"

#if HAVE_PCRE
/* DEBUGGING */
//#define MATCHER_PCRE_DEBUG
#ifdef MATCHER_PCRE_DEBUG
#  define pm_dbgmsg(...) cli_dbgmsg( __VA_ARGS__)
#else
#  define pm_dbgmsg(...)
#endif
#undef MATCHER_PCRE_DEBUG

/* PERFORMANCE MACROS AND FUNCTIONS */
#define MAX_TRACKED_PCRE 64
#define PCRE_EVENTS_PER_SIG 2
#define MAX_PCRE_SIGEVENT_ID MAX_TRACKED_PCRE*PCRE_EVENTS_PER_SIG

cli_events_t *p_sigevents = NULL;
unsigned int p_sigid = 0;

static void pcre_perf_events_init(struct cli_pcre_meta *pm)
{
    int ret;
    char *pcre_name = NULL;

    if (!p_sigevents) {
        p_sigevents = cli_events_new(MAX_PCRE_SIGEVENT_ID);
        if (!p_sigevents) {
            cli_errmsg("pcre_perf: no memory for events table\n");
            return;
        }
    }

    if (p_sigid > MAX_PCRE_SIGEVENT_ID - PCRE_EVENTS_PER_SIG - 1) {
        cli_errmsg("pcre_perf: events table full. Increase MAX_TRACKED_PCRE\n");
        return;
    }

    /* set the name */
    pcre_name = pm->pdata.expression;

    pm_dbgmsg("pcre_perf: adding sig ids starting %u for %s\n", p_sigid, pcre_name);

    /* register time event */
    pm->sigtime_id = p_sigid;
    ret = cli_event_define(p_sigevents, p_sigid++, pcre_name, ev_time, multiple_sum);
    if (ret) {
        cli_errmsg("pcre_perf: cli_event_define() error for time event id %d\n", pm->sigtime_id);
        pm->sigtime_id = MAX_PCRE_SIGEVENT_ID+1;
        return;
    }

    /* register match count */
    pm->sigmatch_id = p_sigid;
    ret = cli_event_define(p_sigevents, p_sigid++, pcre_name, ev_int, multiple_sum);
    if (ret) {
        cli_errmsg("pcre_perf: cli_event_define() error for matches event id %d\n", pm->sigmatch_id);
        pm->sigmatch_id = MAX_PCRE_SIGEVENT_ID+1;
        return;
    }
}

struct sigperf_elem {
    const char * name;
    uint64_t usecs;
    unsigned long run_count;
    unsigned long match_count;
};

static int sigelem_comp(const void * a, const void * b)
{
    const struct sigperf_elem *ela = a;
    const struct sigperf_elem *elb = b;
    return elb->usecs/elb->run_count - ela->usecs/ela->run_count;
}

void cli_pcre_perf_print()
{
    struct sigperf_elem stats[MAX_TRACKED_PCRE], *elem = stats;
    int i, elems = 0, max_name_len = 0, name_len;

    if (!p_sigid || !p_sigevents) {
        cli_warnmsg("cli_pcre_perf_print: statistics requested but no PCREs were loaded!\n");
        return;
    }

    memset(stats, 0, sizeof(stats));
    for (i=0;i<MAX_TRACKED_PCRE;i++) {
        union ev_val val;
        uint32_t count;
        const char * name = cli_event_get_name(p_sigevents, i*PCRE_EVENTS_PER_SIG);
        cli_event_get(p_sigevents, i*PCRE_EVENTS_PER_SIG, &val, &count);
        if (!count) {
            if (name)
                cli_dbgmsg("No event triggered for %s\n", name);
            continue;
        }
        if (name)
            name_len = strlen(name);
        else
            name_len = 0;
        if (name_len > max_name_len)
            max_name_len = name_len;
        elem->name = name?name:"\"noname\"";
        elem->usecs = val.v_int;
        elem->run_count = count;
        cli_event_get(p_sigevents, i*PCRE_EVENTS_PER_SIG+1, &val, &count);
        elem->match_count = count;
        elem++;
        elems++;
    }

    cli_qsort(stats, elems, sizeof(struct sigperf_elem), sigelem_comp);

    elem = stats;
    /* name runs matches microsecs avg */
    cli_infomsg (NULL, "%-*s %*s %*s %*s %*s\n", max_name_len, "PCRE Expression",
                 8, "#runs", 8, "#matches", 12, "usecs total", 9, "usecs avg");
    cli_infomsg (NULL, "%-*s %*s %*s %*s %*s\n", max_name_len, "===============",
                 8, "=====", 8, "========", 12, "===========", 9, "=========");
    while (elem->run_count) {
        cli_infomsg (NULL, "%-*s %*lu %*lu %*llu %*.2f\n", max_name_len, elem->name,
                     8, elem->run_count, 8, elem->match_count,
                     12, elem->usecs, 9, (double)elem->usecs/elem->run_count);
        elem++;
    }
}


void cli_pcre_perf_events_destroy()
{
    cli_events_free(p_sigevents);
    p_sigid = 0;
}


/* PCRE MATCHER FUNCTIONS */
int cli_pcre_addpatt(struct cli_matcher *root, const char *trigger, const char *pattern, const char *cflags, const char *offset, const uint32_t *lsigid, unsigned int options)
{
    struct cli_pcre_meta **newmetatable = NULL, *pm = NULL;
    uint32_t pcre_count;
    const char *opt;
    int ret = CL_SUCCESS, rssigs;

    if (!root || !trigger || !pattern || !offset) {
        cli_errmsg("cli_pcre_addpatt: NULL root or NULL trigger or NULL pattern or NULL offset\n");
        return CL_ENULLARG;
    }

    /* TODO: trigger and regex checking (backreference limitations?) (control pattern limitations?) */
    /* cli_ac_chklsig will fail a empty trigger; empty patterns can cause an infinite loop */
    if (*trigger == '\0' || *pattern == '\0') {
        cli_errmsg("cli_pcre_addpatt: trigger or pattern cannot be an empty string\n");
        return CL_EMALFDB;
    }
    if (cflags && *cflags == '\0') {
        cflags = NULL;
    }

    if (lsigid)
        pm_dbgmsg("cli_pcre_addpatt: Adding /%s/%s%s triggered on (%s) as subsig %d for lsigid %d\n", 
                  pattern, cflags ? " with flags " : "", cflags ? cflags : "", trigger, lsigid[1], lsigid[0]);
    else
        pm_dbgmsg("cli_pcre_addpatt: Adding /%s/%s%s triggered on (%s) [no lsigid]\n",
                  pattern, cflags ? " with flags " : "", cflags ? cflags : "", trigger);

    /* validate the lsig trigger */
    rssigs = cli_ac_chklsig(trigger, trigger + strlen(trigger), NULL, NULL, NULL, 1);
    if((strcmp(trigger, PCRE_BYPASS)) && (rssigs == -1)) {
        cli_errmsg("cli_pcre_addpatt: regex subsig /%s/ is missing a valid logical trigger\n", pattern);
        return CL_EMALFDB;
    }

    if (lsigid) {
        if (rssigs > lsigid[1]) {
            cli_errmsg("cli_pcre_addpatt: regex subsig %d logical trigger refers to subsequent subsig %d\n", lsigid[1], rssigs);
            return CL_EMALFDB;
        }
        if (rssigs == lsigid[1]) {
            cli_errmsg("cli_pcre_addpatt: regex subsig %d logical trigger is self-referential\n", lsigid[1]);
            return CL_EMALFDB;
        }
    }
    else {
        cli_dbgmsg("cli_pcre_addpatt: regex subsig is missing lsigid data\n");
    }

    /* allocating entries */
    pm = (struct cli_pcre_meta *)mpool_calloc(root->mempool, 1, sizeof(*pm));
    if (!pm) {
        cli_errmsg("cli_pcre_addpatt: Unable to allocate memory for new pcre meta\n");
        return CL_EMEM;
    }

    pm->trigger = strdup(trigger);
    if (!pm->trigger) {
        cli_errmsg("cli_pcre_addpatt: Unable to allocate memory for trigger string\n");
        cli_pcre_freemeta(pm);
        mpool_free(root->mempool, pm);
        return CL_EMEM;
    }

    pm->pdata.expression = strdup(pattern);
    if (!pm->pdata.expression) {
        cli_errmsg("cli_pcre_addpatt: Unable to allocate memory for expression\n");
        cli_pcre_freemeta(pm);
        mpool_free(root->mempool, pm);
        return CL_EMEM;
    }

    if (lsigid) {
        pm->lsigid[0] = 1;
        pm->lsigid[1] = lsigid[0];
        pm->lsigid[2] = lsigid[1];
    }
    else {
        /* sigtool */
        pm->lsigid[0] = 0;
    }

    /* offset parsing and usage, similar to cli_ac_addsig */
    /* relative and type-specific offsets handled during scan */
    ret = cli_caloff(offset, NULL, root->type, pm->offdata, &(pm->offset_min), &(pm->offset_max));
    if (ret != CL_SUCCESS) {
        cli_errmsg("cli_pcre_addpatt: cannot calculate offset data: %s for pattern: %s\n", offset, pattern);
        cli_pcre_freemeta(pm);
        mpool_free(root->mempool, pm);
        return ret;
    }
    if(pm->offdata[0] != CLI_OFF_ANY) {
        if(pm->offdata[0] == CLI_OFF_ABSOLUTE)
            root->pcre_absoff_num++;
        else
            root->pcre_reloff_num++;
    }

    /* parse and add options, also totally not from snort */
    if (cflags) {
        opt = cflags;

        /* cli_pcre_addoptions handles pcre specific options */
        while (cli_pcre_addoptions(&(pm->pdata), &opt, 0) != CL_SUCCESS) {
            /* handle matcher specific options here */
            switch (*opt) {
            case 'g':  pm->flags |= CLI_PCRE_GLOBAL;            break;
            case 'e':  pm->flags |= CLI_PCRE_ENCOMPASS;         break;
            default:
                cli_errmsg("cli_pcre_addpatt: unknown/extra pcre option encountered %c\n", *opt);
                cli_pcre_freemeta(pm);
                mpool_free(root->mempool, pm);
                return CL_EMALFDB;
            }
            opt++;
        }

        if (pm->flags) {
            pm_dbgmsg("Matcher:  %s%s\n",
                      pm->flags & CLI_PCRE_GLOBAL ? "CLAMAV_PCRE_GLOBAL " : "",
                      pm->flags & CLI_PCRE_ENCOMPASS ? "CLAMAV_ENCOMPASS " : "");
        }
        else
            pm_dbgmsg("Matcher:  NONE\n");

        if (pm->pdata.options) {
            pm_dbgmsg("Compiler: %s%s%s%s%s%s%s\n",
                      pm->pdata.options & PCRE_CASELESS ? "PCRE_CASELESS " : "",
                      pm->pdata.options & PCRE_DOTALL ? "PCRE_DOTALL " : "",
                      pm->pdata.options & PCRE_MULTILINE ? "PCRE_MULTILINE " : "",
                      pm->pdata.options & PCRE_EXTENDED ? "PCRE_EXTENDED " : "",

                      pm->pdata.options & PCRE_ANCHORED ? "PCRE_ANCHORED " : "",
                      pm->pdata.options & PCRE_DOLLAR_ENDONLY ? "PCRE_DOLLAR_ENDONLY " : "",
                      pm->pdata.options & PCRE_UNGREEDY ? "PCRE_UNGREEDY " : "");
        }
        else
            pm_dbgmsg("Compiler: NONE\n");
    }

    /* add metadata to the performance tracker */
    if (options & CL_DB_PCRE_STATS)
        pcre_perf_events_init(pm);

    /* add pcre data to root after reallocation */
    pcre_count = root->pcre_metas+1;
    newmetatable = (struct cli_pcre_meta **)mpool_realloc(root->mempool, root->pcre_metatable,
                                         pcre_count * sizeof(struct cli_pcre_meta *));
    if (!newmetatable) {
        cli_errmsg("cli_pcre_addpatt: Unable to allocate memory for new pcre meta table\n");
        cli_pcre_freemeta(pm);
        mpool_free(root->mempool, pm);
        return CL_EMEM;
    }

    newmetatable[pcre_count-1] = pm;
    root->pcre_metatable = newmetatable;

    root->pcre_metas = pcre_count;

    return CL_SUCCESS;
}

int cli_pcre_build(struct cli_matcher *root, long long unsigned match_limit, long long unsigned recmatch_limit, const struct cli_dconf *dconf)
{
    unsigned int i;
    int ret;
    struct cli_pcre_meta *pm = NULL;
    int disable_all = !(dconf->pcre & PCRE_CONF_SUPPORT);

    for (i = 0; i < root->pcre_metas; ++i) {
        pm = root->pcre_metatable[i];
        if (!pm) {
            cli_errmsg("cli_pcre_build: metadata for pcre %d is missing\n", i);
            return CL_ENULLARG;
        }

        /* for safety, disable all pcre */
        if (disable_all) {
            pm->flags |= CLI_PCRE_DISABLED;
            continue;
        }

        if (pm->flags & CLI_PCRE_DISABLED) {
            cli_dbgmsg("cli_pcre_build: Skip compiling regex: %s (disabled)\n", pm->pdata.expression);
            continue;
        }

        /* disable global */
        if ((pm->flags & CLI_PCRE_GLOBAL) & !(dconf->pcre & PCRE_CONF_GLOBAL)) {
            cli_dbgmsg("cli_pcre_build: disabling global option for regex /%s/\n", pm->pdata.expression);
            pm->flags &= ~(CLI_PCRE_GLOBAL);
        }

        /* options override through metadata manipulation */
#ifdef PCRE_NEVER_UTF
        pm->pdata.options |= PCRE_NEVER_UTF; /* implemented in 8.33, disables (?UTF*) potential security vuln */
#endif
        //pm->pdata.options |= PCRE_UCP;/* implemented in 8.20 */
        //pm->pdata.options |= PCRE_AUTO_CALLOUT; /* used with CALLOUT(-BACK) function */

        if (dconf->pcre & PCRE_CONF_OPTIONS) {
            /* compile the regex, no options override *wink* */
            pm_dbgmsg("cli_pcre_build: Compiling regex: /%s/\n", pm->pdata.expression);
            ret = cli_pcre_compile(&(pm->pdata), match_limit, recmatch_limit, 0, 0);
        }
        else {
            /* compile the regex, options overrided and disabled */
            pm_dbgmsg("cli_pcre_build: Compiling regex: /%s/ (without options)\n", pm->pdata.expression);
            ret = cli_pcre_compile(&(pm->pdata), match_limit, recmatch_limit, 0, 1);
        }
        if (ret != CL_SUCCESS) {
            cli_errmsg("cli_pcre_build: failed to build pcre regex\n");
            pm->flags |= CLI_PCRE_DISABLED; /* disable the pcre, currently will terminate execution */
            return ret;
        }
    }

    return CL_SUCCESS;
}

int cli_pcre_recaloff(struct cli_matcher *root, struct cli_pcre_off *data, struct cli_target_info *info, cli_ctx *ctx)
{
    /* TANGENT: maintain relative offset data in cli_ac_data? */
    int ret;
    unsigned int i;
    struct cli_pcre_meta *pm;
    uint32_t endoff;

    if (!data) {
        return CL_ENULLARG;
    }

    if (!(ctx->dconf->pcre & PCRE_CONF_SUPPORT) || !root || !root->pcre_metatable || !info) {
        data->shift = NULL;
        data->offset = NULL;
        return CL_SUCCESS;
    }

    /* allocate data structures */
    data->shift = (uint32_t *) cli_calloc(root->pcre_metas, sizeof(uint32_t));
    if (!data->shift) {
        cli_errmsg("cli_pcre_initoff: cannot allocate memory for data->shift\n");
        return CL_EMEM;
    }
    data->offset = (uint32_t *) cli_calloc(root->pcre_metas, sizeof(uint32_t));
    if (!data->offset) {
        cli_errmsg("cli_pcre_initoff: cannot allocate memory for data->offset\n");
        free(data->shift);
        return CL_EMEM;
    }

    /* iterate across all pcre metadata and recalc offsets */
    for (i = 0; i < root->pcre_metas; ++i) {
        pm = root->pcre_metatable[i];

        /* skip broken pcres, not getting executed anyways */
        if (pm->flags & CLI_PCRE_DISABLED) {
            data->offset[i] = CLI_OFF_NONE;
            data->shift[i] = CLI_OFF_NONE;
            continue;
        }

        if (pm->offdata[0] == CLI_OFF_ANY) {
            data->offset[i] = 0;
            data->shift[i] = 0;
        }
        else if (pm->offdata[0] == CLI_OFF_ABSOLUTE) {
            data->offset[i] = pm->offdata[1];
            data->shift[i] = pm->offdata[2];
        }
        else if (pm->offdata[0] == CLI_OFF_EOF_MINUS) {
            data->offset[i] = pm->offdata[1];
            data->shift[i] = pm->offdata[2];
        }
        else {
            ret = cli_caloff(NULL, info, root->type, pm->offdata, &data->offset[i], &endoff);
            if (ret != CL_SUCCESS) {
                cli_errmsg("cli_pcre_recaloff: cannot recalculate relative offset for signature\n");
                free(data->shift);
                free(data->offset);
                return ret;
            }
            data->shift[i] = endoff-(data->offset[i]);
        }
    }

    return CL_SUCCESS;
}

void cli_pcre_freeoff(struct cli_pcre_off *data)
{
    if (data) {
        free(data->offset);
        data->offset = NULL;
        free(data->shift);
        data->shift = NULL;
    }
}

int cli_pcre_scanbuf(const unsigned char *buffer, uint32_t length, const struct cli_matcher *root, struct cli_ac_data *mdata, struct cli_ac_result **res, const struct cli_pcre_off *data, cli_ctx *ctx)
{
    struct cli_pcre_meta **metatable = root->pcre_metatable, *pm = NULL;
    struct cli_pcre_data *pd;
    struct cli_ac_result *newres;
    uint32_t adjbuffer, adjshift, adjlength;
    unsigned int i, evalcnt;
    uint64_t evalids, maxfilesize;
    uint32_t global, encompass;
    int rc, offset, ovector[OVECCOUNT];

    if (!(ctx->dconf->pcre & PCRE_CONF_SUPPORT) || (!root->pcre_metatable)) {
        return CL_SUCCESS;
    }

    /* check that scanned buffer does not exceed pcre filesize limit */
    maxfilesize = (uint64_t)cl_engine_get_num(ctx->engine, CL_ENGINE_PCRE_MAX_FILESIZE, &rc);
    if (rc != CL_SUCCESS)
        return rc;
    if (maxfilesize && (length > maxfilesize)) {
        cli_dbgmsg("cli_pcre_scanbuf: pcre max filesize exceeded (limit: %llu, needed: %u)\n", maxfilesize, length);
        return CL_EMAXSIZE;
    }

    for (i = 0; i < root->pcre_metas; ++i) {
        pm = root->pcre_metatable[i];
        pd = &(pm->pdata);

        /* skip checking and running disabled pcres */
        if (pm->flags & CLI_PCRE_DISABLED) {
            cli_dbgmsg("cli_pcre_scanbuf: skipping disabled regex /%s/\n", pd->expression);
            continue;
        }

        /* evaluate trigger */
        if (pm->lsigid[0]) {
            cli_dbgmsg("cli_pcre_scanbuf: checking %s; running regex /%s/\n", pm->trigger, pd->expression);
            if ((strcmp(pm->trigger, PCRE_BYPASS)) && (cli_ac_chklsig(pm->trigger, pm->trigger + strlen(pm->trigger), mdata->lsigcnt[pm->lsigid[1]], &evalcnt, &evalids, 0) != 1))
                continue;
        }
        else {
            cli_dbgmsg("cli_pcre_scanbuf: skipping %s check due to unintialized lsigid\n", pm->trigger);
            /* fall-through to unconditional execution - sigtool-only */
        }

        global = (pm->flags & CLI_PCRE_GLOBAL);       /* search for all matches */
        encompass = (pm->flags & CLI_PCRE_ENCOMPASS); /* encompass search to offset->offset+maxshift */
        offset = pd->search_offset;                   /* this is usually 0 */

        cli_dbgmsg("cli_pcre_scanbuf: triggered %s; running regex /%s/%s\n", pm->trigger, pd->expression, global ? " (global)":"");

        /* adjust the buffer sent to cli_pcre_match for offset and maxshift */
        if (!data) {
            /* default to scanning whole buffer but try to use existing offdata */
            if (pm->offdata[0] == CLI_OFF_ABSOLUTE) {
                adjbuffer = pm->offdata[1];
                adjshift = pm->offdata[2];
            }
            else if (pm->offdata[0] == CLI_OFF_EOF_MINUS) {
                if (length > pm->offdata[1]) {
                    adjbuffer = length - pm->offdata[1];
                    adjshift = pm->offdata[2];
                }
                else {
                    /* EOF is invalid */
                    continue;
                }
            }
            else {
                /* you could call cli_caloff here but you should call cli_pcre_recaloff prior within the caller */
                adjbuffer = 0;
                adjshift = 0;
            }
        }
        else {
            adjbuffer = data->offset[i];
            adjshift = data->shift[i];
        }

        /* check the offset bounds */
        if (adjbuffer < length) {
            /* handle encompass flag */
            if (encompass && adjshift != 0 && adjshift != CLI_OFF_NONE) {
                    if (adjbuffer+adjshift > length)
                        adjlength = length - adjbuffer;
                    else
                        adjlength = adjshift;
            }
            else {
                adjlength = length - adjbuffer;
            }
        }
        else {
            /* starting offset is outside bounds of file, skip pcre execution */
            cli_dbgmsg("cli_pcre_scanbuf: starting offset is outside bounds of file %u >= %u\n", adjbuffer, length);
            continue;
        }

        pm_dbgmsg("cli_pcre_scanbuf: passed buffer adjusted to %u +%u(%u)[%u]%s\n", adjbuffer, adjlength, adjbuffer+adjlength, adjshift, encompass ? " (encompass)":"");

        /* if the global flag is set, loop through the scanning */
        do {
            /* performance metrics */
            cli_event_time_start(p_sigevents, pm->sigtime_id);
            rc = cli_pcre_match(pd, buffer+adjbuffer, adjlength, offset, 0, ovector, OVECCOUNT);
            cli_event_time_stop(p_sigevents, pm->sigtime_id);
            /* if debug, generate a match report */
            if (cli_debug_flag)
                cli_pcre_report(pd, buffer+adjbuffer, adjlength, rc, ovector, OVECCOUNT);

            /* matched, rc shouldn't be >0 unless a full match occurs */
            if (rc > 0) {
                /* check if we've gone over offset+shift */
                if (!encompass && adjshift) {
                    if (ovector[0] > adjshift) {
                        /* ignore matched offset (outside of maxshift) */
                        cli_dbgmsg("cli_pcre_scanbuf: match found outside of maxshift @%u\n", adjbuffer+ovector[0]);
                        break;
                    }
                }

                /* track the detection count */
                cli_event_count(p_sigevents, pm->sigmatch_id);

                /* for logical signature evaluation */
                if (pm->lsigid[0]) {
                    pm_dbgmsg("cli_pcre_scanbuf: assigning lsigcnt[%d][%d], located @ %d\n",
                              pm->lsigid[1], pm->lsigid[2], adjbuffer+ovector[0]);

                    lsig_sub_matched(root, mdata, pm->lsigid[1], pm->lsigid[2], adjbuffer+ovector[0], 0);
                }

                cli_dbgmsg("cli_pcre_scanbuf: located regex match @ %d\n", adjbuffer+ovector[0]);

                /* for raw match data - sigtool only */
                if(res) {
                    newres = (struct cli_ac_result *) malloc(sizeof(struct cli_ac_result));
                    if(!newres) {
                        cli_errmsg("cli_pcre_scanbuff: Can't allocate memory for newres %u\n", sizeof(struct cli_ac_result));
                        return CL_EMEM;
                    }
                    newres->virname = NULL;    /* get value? */
                    newres->customdata = NULL; /* get value? */
                    newres->next = *res;
                    newres->offset = adjbuffer+ovector[0];
                    *res = newres;
                }
            }

            /* move off to the end of the match for next match; offset is relative to adjbuffer
             * NOTE: misses matches starting within the last match; TODO: start from start of last match? */
            offset = ovector[1];

            /* clear the ovector results (they fall through the pcre_match) */
            memset(ovector, 0, sizeof(ovector));
        } while (global && rc > 0 && offset < adjlength);

        /* handle error codes */
        if (rc < 0 && rc != PCRE_ERROR_NOMATCH) {
            switch (rc) {
            case PCRE_ERROR_CALLOUT:
                break;
            case PCRE_ERROR_NOMEMORY:
                cli_errmsg("cli_pcre_scanbuf: cli_pcre_match: pcre_exec: out of memory\n");
                return CL_EMEM;
            case PCRE_ERROR_MATCHLIMIT:
                cli_dbgmsg("cli_pcre_scanbuf: cli_pcre_match: pcre_exec: match limit exceeded\n");
                break;
            case PCRE_ERROR_RECURSIONLIMIT:
                cli_dbgmsg("cli_pcre_scanbuf: cli_pcre_match: pcre_exec: recursive limit exceeded\n");
                break;
            default:
                cli_errmsg("cli_pcre_scanbuf: cli_pcre_match: pcre_exec: returned error %d\n", rc);
                return CL_BREAK;
            }
        }
    }

    return CL_SUCCESS;
}

void cli_pcre_freemeta(struct cli_pcre_meta *pm)
{
    if (!pm)
        return;

    if (pm->trigger)
        free(pm->trigger);

    cli_pcre_free_single(&(pm->pdata));
}

void cli_pcre_freetable(struct cli_matcher *root)
{
    uint32_t i;
    struct cli_pcre_meta *pm = NULL;

    for (i = 0; i < root->pcre_metas; ++i) {
        /* free pcre meta */
        pm = root->pcre_metatable[i];
        cli_pcre_freemeta(pm);
        mpool_free(root->mempool, pm);
    }

    /* free holding structures and set count to zero */
    mpool_free(root->mempool, root->pcre_metatable);
    root->pcre_metatable = NULL;
    root->pcre_metas = 0;
}

#else
/* NO-PCRE FUNCTIONS */
int cli_pcre_recaloff(struct cli_matcher *root, struct cli_pcre_off *data, struct cli_target_info *info, cli_ctx *ctx)
{
    UNUSEDPARAM(root);
    UNUSEDPARAM(info);
    UNUSEDPARAM(ctx);
    if (data) {
        data->offset = NULL;
        data->shift = NULL;
    }
    return CL_SUCCESS;
}

void cli_pcre_freeoff(struct cli_pcre_off *data)
{
    UNUSEDPARAM(data);
    return;
}

#endif /* HAVE_PCRE */
