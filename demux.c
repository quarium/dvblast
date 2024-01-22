/*****************************************************************************
 * demux.c
 *****************************************************************************
 * Copyright (C) 2004, 2008-2011, 2015-2018 VideoLAN
 *
 * Authors: Christophe Massiot <massiot@via.ecp.fr>
 *          Andy Gatward <a.j.gatward@reading.ac.uk>
 *          Marian Ďurkovič <md@bts.sk>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <stdarg.h>
#include <inttypes.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ev.h>
#include <limits.h>
#include <assert.h>

#include "dvblast.h"
#include "en50221.h"
#include "mrtg-cnt.h"

#ifdef HAVE_ICONV
#include <iconv.h>
#endif

#include <bitstream/mpeg/ts.h>
#include <bitstream/mpeg/pes.h>
#include <bitstream/mpeg/psi.h>
#include <bitstream/dvb/si.h>
#include <bitstream/dvb/si_print.h>
#include <bitstream/mpeg/psi_print.h>

/*****************************************************************************
 * Local declarations
 *****************************************************************************/
#define MIN_SECTION_FRAGMENT    PSI_HEADER_SIZE_SYNTAX1


struct ts_pid;
struct ts_sid;
struct sid_out;

/*
 * PID refcount
 */
typedef struct pid_list { list_t list; } pid_list_t;

typedef struct pid_refcount
{
    link_t link;
    link_t internal;
    struct ts_pid *pid;
    struct ts_sid *sid;
    pid_list_t ecms;
    bool changed;
    es_type_t type;
    union {
        struct {
            char lang[4];
        } audio;
        struct {
            char lang[4];
        } subtitle;
    } desc;
} pid_refcount_t;

LINK_OF(pid_refcount, link);
LINK_OF(pid_refcount, internal);

LIST_OF(pid_list, list, pid_refcount, link);
#define pid_list_each(L, I) LIST_EACH(pid_list_next, L, I)
#define pid_list_clean(L)   LIST_CLEAN(pid_list_pop, pid_release, L)

/*
 * SDT Service link
 */
typedef struct sdt_entries { list_t list; } sdt_entries_t;

typedef struct sdt_entry
{
    link_t link;
    link_t sid_link;
    struct ts_sid *sid;
    char *provider;
    char *name;
} sdt_entry_t;

LINK_OF(sdt_entry, link);
LINK_OF(sdt_entry, sid_link);

LIST_OF(sdt_entries, list, sdt_entry, link);
#define sdt_entries_each(L, I) LIST_EACH(sdt_entries_next, L, I)
#define sdt_entries_clean(L)   LIST_CLEAN(sdt_entries_pop, sdt_entry_del, L)

/*
 * PID / Service output link
 */
typedef struct pid_out
{
    link_t pid_link;
    link_t sid_out_link;
    struct ts_pid *pid;
    struct sid_out *sid_out;
    config_pid_t *conf;
    /* incomplete PID (only PCR packets) */
    bool pcr_only;
    uint16_t new_pid;
    bool changed;
} pid_out_t;

LINK_OF(pid_out, pid_link);
LINK_OF(pid_out, sid_out_link);

/*
 * Service / output link
 */

typedef struct sid_out_pid_list { list_t list; } sid_out_pid_list_t;
LIST_OF(sid_out_pid_list, list, pid_out, sid_out_link);
#define sid_out_pid_list_each(L, I) LIST_EACH(sid_out_pid_list_next, L, I)
#define sid_out_pid_list_clean(L) \
    LIST_CLEAN(sid_out_pid_list_pop, pid_out_unlink, L)

typedef struct sid_out
{
    link_t output_link;
    link_t sid_link;
    output_t *output;
    struct ts_sid *sid;
    config_sid_t *conf;
    sid_out_pid_list_t pids;
    uint8_t *pmt_section;
    uint8_t pmt_version;
    uint8_t pmt_cc;
    uint16_t new_sid;
    uint16_t pmt_pid;
    bool changed;
    bool pid_changed;
} sid_out_t;

LINK_OF(sid_out, output_link);
LIST_OF(output_sid_list, list, sid_out, output_link);
#define output_sid_list_each(L, I) LIST_EACH(output_sid_list_next, L, I)
#define output_sid_list_clean(L) \
    LIST_CLEAN(output_sid_list_pop, sid_out_unlink, L)

static sid_out_t *output_sid_find( const output_sid_list_t *list,
                                   struct ts_sid *sid )
{
    output_sid_list_each( list, s )
        if ( s->sid == sid )
            return s;
    return NULL;
}

LINK_OF(sid_out, sid_link);

/*
 * Service
 */
struct eit_sections {
    PSI_TABLE_DECLARE(data);
};

/* EIT is carried in several separate tables, we need to track each table
   separately, otherwise one table overwrites sections of another table */
#define MAX_EIT_TABLES ( EIT_TABLE_ID_SCHED_ACTUAL_LAST - EIT_TABLE_ID_PF_ACTUAL + 1 )

typedef struct sid_sdt_entries { list_t list; } sid_sdt_entries_t;
LIST_OF(sid_sdt_entries, list, sdt_entry, sid_link);
#define sid_sdt_entries_each(L, I) LIST_EACH(sid_sdt_entries_next, L, I)
#define sid_sdt_entries_clean(L)   LIST_CLEAN(sid_sdt_entries_pop, sdt_entry_del, L)

typedef struct sid_out_list { list_t list; } sid_out_list_t;
LIST_OF(sid_out_list, list, sid_out, sid_link);
#define sid_out_list_each(L, I)     LIST_EACH(sid_out_list_next, L, I)
#define sid_out_list_clean(L) \
    LIST_CLEAN(sid_out_list_pop, sid_out_unlink, L)

typedef struct ts_sid
{
    link_t link;
    uint16_t id;
    uint16_t pmt_pid;
    uint16_t pcr_pid;
    uint8_t *current_pmt;
    struct eit_sections eit_table[MAX_EIT_TABLES];
    unsigned long packets_passed;
    bool changed;
    sid_sdt_entries_t sdt_entries;
    pid_list_t pmt;
    pid_list_t pids;
    pid_list_t ecms;
    sid_out_list_t outputs;
} ts_sid_t;

LINK_OF(ts_sid, link);

typedef struct sid_list { list_t list; } sid_list_t;
LIST_OF(sid_list, list, ts_sid, link);
#define sid_list_each(L, I)     LIST_EACH(sid_list_next, L, I)
#define sid_list_flush(L, I)    LIST_FLUSH(sid_list_pop, L, I)
#define sid_list_clean(L)       LIST_CLEAN(sid_list_pop, sid_free, L)

/*
 * PID
 */
typedef struct pid_refcount_list { list_t list; } pid_refcount_list_t;
LIST_OF(pid_refcount_list, list, pid_refcount, internal);
#define pid_refcount_list_each(L, I) LIST_EACH(pid_refcount_list_next, L, I)

typedef struct pid_out_list { list_t list; } pid_out_list_t;
LIST_OF(pid_out_list, list, pid_out, pid_link);
#define pid_out_list_each(L, I) LIST_EACH(pid_out_list_next, L, I)

typedef struct ts_pid
{
    uint16_t id;
    bool b_psi;
    bool b_pes;
    int8_t i_last_cc;
    int i_demux_fd;
    /* b_emm is set to true when PID carries EMM packet
       and should be outputed in all services */
    bool b_emm;

    /* PID info and stats */
    mtime_t i_bytes_ts;
    unsigned long packets_passed;
    ts_pid_info_t info;

    /* biTStream PSI section gathering */
    uint8_t *p_psi_buffer;
    uint16_t i_psi_buffer_used;

    pid_out_list_t outputs;
    pid_refcount_list_t refcounts;

    int i_pes_status; /* pes + unscrambled */
    struct ev_timer timeout_watcher;
} ts_pid_t;

typedef struct sid_t
{
    uint16_t i_sid, i_pmt_pid;
    uint8_t *p_current_pmt;
    struct eit_sections eit_table[MAX_EIT_TABLES];
    unsigned long i_packets_passed;
} sid_t;

/*
 *
 */

mtime_t i_wallclock = 0;

static ts_pid_t p_pids[MAX_PIDS];
static sid_list_t sids = { LIST_INIT(sids.list) };
static sdt_entries_t sdt_entries = { LIST_INIT(sdt_entries.list) };
static pid_list_t psi_list = { LIST_INIT(psi_list.list) };
static pid_list_t emm_list = { LIST_INIT(emm_list.list) };

static PSI_TABLE_DECLARE(pp_current_pat_sections);
static PSI_TABLE_DECLARE(pp_next_pat_sections);
static PSI_TABLE_DECLARE(pp_current_cat_sections);
static PSI_TABLE_DECLARE(pp_next_cat_sections);
static PSI_TABLE_DECLARE(pp_current_nit_sections);
static PSI_TABLE_DECLARE(pp_next_nit_sections);
static PSI_TABLE_DECLARE(pp_current_sdt_sections);
static PSI_TABLE_DECLARE(pp_next_sdt_sections);
static mtime_t i_last_dts = -1;
static int i_demux_fd;
static uint64_t i_nb_packets = 0;
static uint64_t i_nb_invalids = 0;
static uint64_t i_nb_discontinuities = 0;
static uint64_t i_nb_errors = 0;
static int i_tuner_errors = 0;
static mtime_t i_last_error = 0;
static mtime_t i_last_reset = 0;
static struct ev_timer print_watcher;

#ifdef HAVE_ICONV
static iconv_t iconv_handle = (iconv_t)-1;
#endif

/*****************************************************************************
 * Local prototypes
 *****************************************************************************/
static void demux_Handle( block_t *p_ts );
static void SetDTS( block_t *p_list );
static bool SIDIsSelected( ts_sid_t *sid );
static bool PMTNeedsDescrambling( uint8_t *p_pmt );
static void FlushEIT( output_t *p_output, mtime_t i_dts );
static void SendTDT( block_t *p_ts );
static void SendEMM( block_t *p_ts );
static void NewPAT( output_t *p_output );
static void NewPMT( sid_out_t *sid_out );
static void NewNIT( output_t *p_output );
static void NewSDT( output_t *p_output );
static void HandlePSIPacket( uint8_t *p_ts, mtime_t i_dts );
static const char *get_pid_desc(uint16_t i_pid, uint16_t *i_sid);
static ts_sid_t *sid_find( const sid_list_t *sids, uint16_t i_sid );
static void sid_out_unlink( sid_out_t *l );
static void sid_out_Log(int level, void *priv, const char *format, va_list ap);

#define DESC_CHECK(Tag, Desc)               \
    ( Desc != NULL                          \
      && desc_get_tag( Desc ) == 0x##Tag    \
      && desc##Tag##_validate( Desc ) )

/*
 * ES types
 */

static es_type_t es_private_type( const uint8_t *p_es )
{
    uint8_t i_type = pmtn_get_streamtype( p_es );

    if ( i_type == 0x6 )
    {
        pmtn_each_desc( (uint8_t *)p_es, p_desc )
        {
            uint8_t i_tag = desc_get_tag( p_desc );

            switch ( i_tag )
            {
                case 0x6a: /* A/52 */
                case 0x7a: /* Enhanced A/52 */
                case 0x7b: /* DCA */
                case 0x7c: /* AAC */
                    return ES_TYPE_AUDIO;

                case 0x46: /* VBI + teletext */
                case 0x56: /* teletext */
                case 0x59: /* dvbsub */
                    return ES_TYPE_SUBTITLE;
            }
        }
        return ES_TYPE_PRIVATE;
    }

    return ES_TYPE_UNKNOWN;
}

static es_type_t es_type( const uint8_t *p_es )
{
    uint8_t i_type = pmtn_get_streamtype( p_es );

    switch ( i_type )
    {
        case 0x3: /* audio MPEG-1 */
        case 0x4: /* audio MPEG-2 */
        case 0xf: /* audio AAC ADTS */
        case 0x11: /* audio AAC LATM */
        case 0x81: /* ATSC A/52 */
        case 0x87: /* ATSC Enhanced A/52 */
            return ES_TYPE_AUDIO;

        case 0x1: /* video MPEG-1 */
        case 0x2: /* video MPEG-2 */
        case 0x10: /* video MPEG-4 */
        case 0x1b: /* video H264 */
        case 0x24: /* video H265 */
        case 0x42: /* video AVS */
            return ES_TYPE_VIDEO;

        case 0x6:
            return ES_TYPE_PRIVATE;

        default:
            break;
    }

    /* FIXME: also parse IOD */
    return ES_TYPE_UNKNOWN;
}

static es_type_t es_type_or_private( const uint8_t *p_es )
{
    es_type_t type = es_type( p_es );
    return type == ES_TYPE_PRIVATE ? es_private_type( p_es ) : type;
}

static const char *es_type_str( es_type_t type )
{
    switch ( type )
    {
        case ES_TYPE_VIDEO: return "Video";
        case ES_TYPE_AUDIO: return "Audio";
        case ES_TYPE_SUBTITLE: return "Subtitle";
        case ES_TYPE_PRIVATE: return "Private";

        case ES_TYPE_PAT: return "PAT";
        case ES_TYPE_CAT: return "CAT";
        case ES_TYPE_NIT: return "NIT";
        case ES_TYPE_SDT: return "SDT";
        case ES_TYPE_EIT: return "EIT";
        case ES_TYPE_RST: return "RST";
        case ES_TYPE_TDT: return "TDT";
        case ES_TYPE_PMT: return "PMT";
        case ES_TYPE_ECM: return "ECM";
        case ES_TYPE_EMM: return "EMM";

        case ES_TYPE_ANY:
        case ES_TYPE_UNKNOWN:
            break;
    }
    return "Unknown";
}

static pidmap_offset es_type_offset( es_type_t type )
{
    switch ( type )
    {
        case ES_TYPE_AUDIO:
            return I_APID;
        case ES_TYPE_VIDEO:
            return I_VPID;
        case ES_TYPE_SUBTITLE:
            return I_SPUPID;
        default:
            return -1;
    }
}

/*****************************************************************************
 * PIDCarriesPES
 *****************************************************************************/
static bool PIDCarriesPES( es_type_t type )
{
    switch (type) {
        case ES_TYPE_VIDEO:
        case ES_TYPE_AUDIO:
        case ES_TYPE_SUBTITLE:
        case ES_TYPE_PRIVATE:
            return true;

        case ES_TYPE_PAT:
        case ES_TYPE_CAT:
        case ES_TYPE_NIT:
        case ES_TYPE_SDT:
        case ES_TYPE_EIT:
        case ES_TYPE_RST:
        case ES_TYPE_TDT:
        case ES_TYPE_PMT:
        case ES_TYPE_ECM:
        case ES_TYPE_EMM:
        case ES_TYPE_ANY:
        case ES_TYPE_UNKNOWN:
            break;
    }
    return false;
}

/*****************************************************************************
 * PIDCarriesPES
 *****************************************************************************/
static bool PIDIsPSI( es_type_t type )
{
    switch (type) {
        case ES_TYPE_PAT:
        case ES_TYPE_CAT:
        case ES_TYPE_NIT:
        case ES_TYPE_SDT:
        case ES_TYPE_EIT:
        case ES_TYPE_RST:
        case ES_TYPE_TDT:
        case ES_TYPE_PMT:
            return true;

        case ES_TYPE_VIDEO:
        case ES_TYPE_AUDIO:
        case ES_TYPE_SUBTITLE:
        case ES_TYPE_PRIVATE:
        case ES_TYPE_ECM:
        case ES_TYPE_EMM:
        case ES_TYPE_ANY:
        case ES_TYPE_UNKNOWN:
            break;
    }
    return false;
}

/*****************************************************************************
 * PIDWouldBeSelected
 *****************************************************************************/
static bool PIDWouldBeSelected( es_type_t type )
{
    /* FIXME: also parse IOD */
    return b_any_type || PIDCarriesPES( type );
}

/*
 * pid refcount
 */

void pid_refcount_Log(int level, void *priv, const char *format, va_list ap)
{
    pid_refcount_t *refcount = priv;
    int len = snprintf( NULL, 0, "pid[%u] %s",
                        refcount->pid->id, format );
    if (len > 0)
    {
        char fmt[len + 1];
        snprintf( fmt, len + 1, "pid[%u] %s",
                  refcount->pid->id, format );
        msg_Log( level, refcount, fmt, ap );
    }
}
static inline MSG_LOG( pid_refcount, Dbg, VERB_DBG )
static inline MSG_LOG( pid_refcount, Info, VERB_INFO )
static inline MSG_LOG( pid_refcount, Err, VERB_ERR )
static inline MSG_LOG( pid_refcount, Warn, VERB_WARN )

static pid_refcount_t *pid_find( const pid_list_t *pids, uint16_t pid )
{
    pid_list_each( pids, p )
        if ( p->pid->id == pid )
            return p;
    return NULL;
}

static void pid_refcount_add( pid_refcount_t *ref )
{
    if ( ref ) {
        uint16_t i_pid = ref->pid->id;

        p_pids[i_pid].b_pes = PIDCarriesPES( ref->type );
        p_pids[i_pid].b_emm =
            ref->type == ES_TYPE_CAT || ref->type == ES_TYPE_EMM;
        pid_refcount_list_add( &p_pids[i_pid].refcounts, ref );

        if ( ref->type == ES_TYPE_PMT && !b_select_pmts )
            return;

        p_pids[i_pid].b_psi = PIDIsPSI( ref->type );
        if ( !b_budget_mode && p_pids[i_pid].i_demux_fd == -1 )
            p_pids[i_pid].i_demux_fd = pf_SetFilter( i_pid );
    }
}

static void pid_refcount_del( pid_refcount_t *ref )
{
    if ( ref ) {
        uint16_t i_pid = ref->pid->id;

        pid_refcount_del_internal( ref );

        bool last_ref = true;
        pid_refcount_list_each( &p_pids[i_pid].refcounts, r )
            if ( ref->type != ES_TYPE_PMT || b_select_pmts )
                last_ref = false;

        if ( last_ref ) {
            if ( !b_budget_mode && last_ref && p_pids[i_pid].i_demux_fd != -1 )
            {
                pf_UnsetFilter( p_pids[i_pid].i_demux_fd, i_pid );
                p_pids[i_pid].i_demux_fd = -1;
            }
            if ( p_pids[i_pid].b_psi )
                psi_assemble_reset( &p_pids[i_pid].p_psi_buffer,
                                    &p_pids[i_pid].i_psi_buffer_used );
            p_pids[i_pid].b_emm = false;
            p_pids[i_pid].b_psi = false;
        }
    }
}

static pid_refcount_t *pid_use( pid_list_t *pids, uint16_t pid,
                                ts_sid_t *sid, es_type_t type )
{
    pid_refcount_t *refcount = pid_find( pids, pid );
    if ( !refcount ) {
        refcount = malloc( sizeof(*refcount) );
        if ( refcount ) {
            pid_refcount_init_link( refcount );
            pid_refcount_init_internal( refcount );
            refcount->pid = &p_pids[pid];
            refcount->sid = sid;
            pid_list_init( &refcount->ecms );
            refcount->changed = true;
            refcount->type = type;
            memset( &refcount->desc, 0, sizeof(refcount->desc) );
            pid_list_add( pids, refcount );
            pid_refcount_add( refcount );
            if ( sid )
                pid_refcount_Dbg( refcount, "used by sid[%u] as %s",
                                  sid->id, es_type_str(type) );
            else
                pid_refcount_Dbg( refcount, "used as %s",
                                  es_type_str(type) );
        }
    }
    else {
        refcount->changed = refcount->type != type || refcount->sid != sid;
        if ( refcount->changed ) {
            if ( sid )
                pid_refcount_Dbg( refcount, "update by sid[%u] as %s",
                                  sid->id, es_type_str(type) );
            else
                pid_refcount_Dbg( refcount, "updated as %s",
                                  es_type_str(type) );
        }
        refcount->type = type;
        refcount->sid = sid;
    }
    return refcount;
}

static void pid_release( pid_refcount_t *refcount )
{
    if ( refcount ) {
        if ( refcount->sid )
            pid_refcount_Dbg( refcount, "released by sid[%u] (%s)",
                              refcount->sid->id,
                              es_type_str( refcount->type ) );
        else
            pid_refcount_Dbg( refcount, "released (%s)",
                              es_type_str( refcount->type ) );
        pid_refcount_del_link( refcount );
        pid_list_clean( &refcount->ecms );
        pid_refcount_del( refcount );
        free( refcount );
    }
}

/*
 * pid
 */

static void ts_pid_init( ts_pid_t *ts_pid, uint16_t pid )
{
    if ( ts_pid ) {
        memset( ts_pid, 0, sizeof (*ts_pid) );
        pid_out_list_init( &ts_pid->outputs );
        pid_refcount_list_init( &ts_pid->refcounts );
        ts_pid->id = pid;
        ts_pid->i_last_cc = -1;
        ts_pid->i_demux_fd = -1;
        ts_pid->i_pes_status = -1;
        psi_assemble_init( &ts_pid->p_psi_buffer, &ts_pid->i_psi_buffer_used );
    }
}

static void ts_pid_clean( ts_pid_t *ts_pid )
{
    if ( ts_pid )
        ev_timer_stop( event_loop, &ts_pid->timeout_watcher );
}

static ts_pid_t *pid_match_conf( const ts_pid_t *pid, const config_pid_t *conf )
{
    if ( !pid || !conf )
        return (ts_pid_t *)pid;

    pid_refcount_list_each( &pid->refcounts, p ) {
        if ( conf->es_type != ES_TYPE_ANY && p->type != conf->es_type )
            continue;

        if ( strlen( conf->lang ) ) {
            switch ( p->type ) {
                case ES_TYPE_AUDIO:
                    if ( strcmp( conf->lang, p->desc.audio.lang ) )
                        continue;
                    break;
                case ES_TYPE_SUBTITLE:
                    if ( strcmp( conf->lang, p->desc.subtitle.lang ) )
                        continue;
                    break;
                default:
                    break;
            }
        }

        return (ts_pid_t *)pid;
    }

    return NULL;
}

/*
 * service refcount
 */
static sdt_entry_t *sdt_entry_find(const sdt_entries_t *list, uint16_t sid)
{
    sdt_entries_each( list, s )
        if ( s->sid->id == sid )
            return s;
    return NULL;
}

static sdt_entry_t *sdt_entry_new(sdt_entries_t *list, uint16_t i_sid)
{
    sdt_entry_t *std_entry = sdt_entry_find( list, i_sid );
    if ( !std_entry ) {
        ts_sid_t *sid = sid_find( &sids, i_sid );
        std_entry = sid ? malloc( sizeof(*std_entry) ) : NULL;
        if ( std_entry ) {
            sdt_entry_init_link( std_entry );
            sdt_entry_init_sid_link( std_entry );
            std_entry->sid = sid;
            std_entry->provider = NULL;
            std_entry->name = NULL;
            sid_sdt_entries_add( &sid->sdt_entries, std_entry );
            sdt_entries_add( list, std_entry );
        }
    }
    return std_entry;
}

static void sdt_entry_del( sdt_entry_t *std_entry )
{
    if ( std_entry ) {
        sdt_entry_del_sid_link( std_entry );
        sdt_entry_del_link( std_entry );
        free( std_entry->provider );
        free( std_entry->name );
        free( std_entry );
    }
}

/*
 * sid
 */

static ts_sid_t *sid_find( const sid_list_t *sids, uint16_t i_sid )
{
    sid_list_each( sids, sid )
        if ( sid->id == i_sid )
            return sid;
    return NULL;
}

static ts_sid_t *sid_at( const sid_list_t *sids, unsigned at )
{
    return at ? sid_list_at( sids, at - 1) : NULL;
}

static ts_sid_t *sid_match_conf( const ts_sid_t *sid, const config_sid_t *conf )
{
    if ( !sid || !conf )
        return (ts_sid_t *)sid;

    if ( conf->name || conf->provider) {
        sid_sdt_entries_each( &sid->sdt_entries, e ) {
            if ( conf->name ) {
                if ( !e->name || strcmp( e->name, conf->name ) )
                    continue;
            }

            if ( conf->provider ) {
                if ( !e->provider || strcmp( e->provider, conf->provider ) )
                    continue;
            }

            return (ts_sid_t *)sid;
        }
        return NULL;
    }

    return (ts_sid_t *)sid;
}

static ts_sid_t *sid_find_from_config( const sid_list_t *sids,
                                       const output_sid_list_t *selected,
                                       const config_sid_t *conf )
{
    if (!conf)
        return NULL;

    switch (conf->type) {
        case OUTPUT_CONFIG_TYPE_STATIC:
            return sid_match_conf( sid_find( sids, conf->value ), conf );
        case OUTPUT_CONFIG_TYPE_COUNT:
            return sid_match_conf( sid_at( sids, conf->value ), conf );
        case OUTPUT_CONFIG_TYPE_ANY:
        case OUTPUT_CONFIG_TYPE_ALL:
            sid_list_each( sids, sid ) {
                if ( !output_sid_find( selected, sid ) &&
                     sid_match_conf( sid, conf ) )
                    return sid;
            }
            break;
    }
    return NULL;
}

static ts_sid_t *sid_link( sid_list_t *sids,
                           uint16_t sid, uint16_t pmt_pid )
{
    ts_sid_t *ts_sid = sid_find( sids, sid );
    if ( !ts_sid ) {
        ts_sid = calloc( 1, sizeof(*ts_sid) );
        if ( ts_sid ) {
            ts_sid_init_link( ts_sid );
            sid_out_list_init( &ts_sid->outputs );
            sid_sdt_entries_init( &ts_sid->sdt_entries );
            pid_list_init( &ts_sid->pmt );
            pid_list_init( &ts_sid->pids );
            pid_list_init( &ts_sid->ecms );
            ts_sid->id = sid;
            ts_sid->pmt_pid = pmt_pid;
            ts_sid->current_pmt = NULL;
            ts_sid->changed = true;
            for ( uint8_t i = 0; i < MAX_EIT_TABLES; i++ )
                psi_table_init( ts_sid->eit_table[i].data );
            pid_use( &ts_sid->pmt, pmt_pid, ts_sid, ES_TYPE_PMT );
            sid_list_add( sids, ts_sid );
            Dbg( "new program: %u on pid %u", sid, pmt_pid );
        }
    }
    else
        ts_sid->changed = false;
    return ts_sid;
}

static void sid_free( ts_sid_t *ts_sid )
{
    if ( ts_sid ) {
        Dbg( "delete program: %u on pid %u", ts_sid->id, ts_sid->pmt_pid );
        sid_out_list_clean( &ts_sid->outputs );
        sid_sdt_entries_clean( &ts_sid->sdt_entries );
        pid_list_clean( &ts_sid->ecms );
        pid_list_clean( &ts_sid->pids );
        pid_list_clean( &ts_sid->pmt );
        ts_sid_del_link(ts_sid);
        for ( uint8_t i = 0; i < MAX_EIT_TABLES; i++ )
            psi_table_free( ts_sid->eit_table[i].data );
        free( ts_sid->current_pmt );
        free( ts_sid );
    }
}

/*
 * pid output
 */

static uint16_t pid_out_id( const pid_out_t *pid )
{
    return pid && pid->pid ? pid->pid->id : 0;
}

void pid_out_Log(int level, void *priv, const char *format, va_list ap)
{
    pid_out_t *pid = priv;
    int len = snprintf( NULL, 0, "pid[%u] %s", pid_out_id( pid ), format);
    if (len > 0)
    {
        char fmt[len + 1];
        snprintf( fmt, len + 1, "pid[%u] %s", pid_out_id( pid ), format );
        sid_out_Log( level, pid->sid_out, fmt, ap );
    }
}
static inline MSG_LOG( pid_out, Dbg, VERB_DBG )
static inline MSG_LOG( pid_out, Info, VERB_INFO )
static inline MSG_LOG( pid_out, Err, VERB_ERR )
static inline MSG_LOG( pid_out, Warn, VERB_WARN )

static pid_out_t *pid_out_find( const sid_out_pid_list_t *pids,
                                sid_out_t *sid_out,
                                uint16_t pid )
{
    sid_out_pid_list_each( pids, l )
        if ( pid_out_id( l ) == pid && l->sid_out == sid_out )
            return l;
    return NULL;
}

static pid_out_t *pid_out_create( sid_out_pid_list_t *pids,
                                  sid_out_t *sid_out,
                                  uint16_t pid )
{
    pid_out_t *l = pids && sid_out ? malloc(sizeof ( *l )) : NULL;
    if ( l )
    {
        pid_out_init_pid_link( l );
        pid_out_init_sid_out_link( l );
        l->sid_out = sid_out;
        l->pid = &p_pids[pid];
        l->new_pid = 0;
        l->conf = NULL;
        l->changed = true;
        l->pcr_only = false;
        pid_out_list_add( &p_pids[pid].outputs, l );
        sid_out_pid_list_add( pids, l );
    }
    return l;
}

static pid_out_t *pid_out_link( sid_out_pid_list_t *pids,
                                sid_out_t *sid,
                                uint16_t pid )
{
    pid_out_t *l = pid_out_find( pids, sid, pid );
    if ( !l )
        l = pid_out_create( pids, sid, pid );
    else
        l->changed = false;

    if ( l )
    {
        l->changed = l->changed || l->pcr_only;
        l->pcr_only = false;
        pid_out_Dbg( l, "%s", l->changed ? "added" : "updated" );
    }

    return l;
}

static pid_out_t *pid_out_link_pcr( sid_out_pid_list_t *pids,
                                    sid_out_t *sid,
                                    uint16_t pid )
{
    pid_out_t *l = pid_out_find( pids, sid, pid );
    if ( !l )
        l = pid_out_create( pids, sid, pid );
    else
        l->changed = false;

    if ( l )
    {
        l->changed = l->changed || !l->pcr_only;
        l->pcr_only = true;
        pid_out_Dbg( l, "%s (pcr only)", l->changed ? "added" : "updated" );
    }

    return l;
}

static void pid_out_remap( pid_out_t *l, uint16_t new_pid )
{
    if ( l )
    {
        l->changed = l->changed || l->new_pid != new_pid;
        if ( l->new_pid != new_pid )
        {
            if ( !new_pid )
                pid_out_Dbg( l, "unmap" );
            else if ( !l->new_pid )
                pid_out_Dbg( l, "remap to %u", new_pid );
            else
                pid_out_Dbg( l, "remap to %u (was %u)", new_pid, l->new_pid );
        }
        l->new_pid = new_pid;
    }
}

static void pid_out_unlink( pid_out_t *l )
{
    if ( !l )
        return;

    pid_out_del_sid_out_link( l );
    pid_out_del_pid_link( l );
    pid_out_Dbg( l, "removed" );
    free( l );
}

/*
 * sid output
 */

static uint16_t sid_out_id( const sid_out_t *sid )
{
    return sid && sid->sid ? sid->sid->id : 0;
}

static output_t *sid_out_get_output( const sid_out_t *sid )
{
    return sid ? sid->output : NULL;
}

static void sid_out_Log(int level, void *priv, const char *format, va_list ap)
{
    sid_out_t *sid = priv;
    int len = snprintf( NULL, 0, "sid[%u] %s", sid_out_id( sid ), format);
    if (len > 0)
    {
        char fmt[len + 1];
        snprintf( fmt, len + 1, "sid[%u] %s", sid_out_id( sid ), format );
        output_Log( level, sid_out_get_output( sid ), fmt, ap );
    }
}
static inline MSG_LOG( sid_out, Dbg, VERB_DBG )
static inline MSG_LOG( sid_out, Info, VERB_INFO )
static inline MSG_LOG( sid_out, Err, VERB_ERR )
static inline MSG_LOG( sid_out, Warn, VERB_WARN )

static sid_out_t *sid_out_find(const output_sid_list_t *sids,
                               output_t *output, ts_sid_t *sid)
{
    output_sid_list_each( sids, l )
        if (l->output == output && l->sid == sid)
            return l;
    return NULL;
}

static sid_out_t *sid_out_create(output_sid_list_t *sids,
                                 output_t *output, ts_sid_t *sid)
{
    sid_out_t *l = sids && output && sid ? malloc(sizeof ( *l )) : NULL;
    if ( l )
    {
        if ( i_ca_handle && PMTNeedsDescrambling( sid->current_pmt ) )
        {
            if ( SIDIsSelected( sid ) )
                en50221_UpdatePMT( sid->current_pmt );
            else
                en50221_AddPMT( sid->current_pmt );
        }

        sid_out_init_sid_link( l );
        sid_out_init_output_link( l );
        l->output = output;
        l->sid = sid;
        l->conf = NULL;
        l->pmt_section = NULL;
        l->pmt_version = rand() & 0xff;
        l->pmt_cc = rand() & 0xff;
        l->new_sid = 0;
        l->pmt_pid = 0;
        l->changed = true;
        l->pid_changed = false;
        sid_out_pid_list_init( &l->pids );
        output_sid_list_add( sids, l );
        sid_out_list_add( &sid->outputs, l );
    }

    return l;
}

static sid_out_t *sid_out_link(output_sid_list_t *sids,
                               output_t *output, ts_sid_t *sid)
{
    sid_out_t *l = sid_out_find( sids, output, sid );
    if ( !l )
        l = sid_out_create( sids, output, sid );
    else
        l->changed = false;

    if ( l )
        sid_out_Dbg( l, "%s", l->changed ? "added" : "updated" );
    return l;
}

static void sid_out_remap( sid_out_t *l, uint16_t new_sid, uint16_t pmt_pid )
{
    if ( l && ( l->new_sid != new_sid || l->pmt_pid != pmt_pid ) )
    {
        l->changed = true;
        if ( !new_sid && !pmt_pid )
            sid_out_Dbg( l, "undo remap" );
        else if ( !pmt_pid )
            sid_out_Dbg( l, "remap to %u", new_sid);
        else if ( !new_sid )
            sid_out_Dbg( l, "remap PMT to %u", pmt_pid );
        else
            sid_out_Dbg( l, "remap to %u and PMT to %u", new_sid, pmt_pid );
        l->new_sid = new_sid;
        l->pmt_pid = pmt_pid;
    }
}

static void sid_out_unlink( sid_out_t *l )
{
    if ( !l)
        return;

    ts_sid_t *sid = l->sid;
    sid_out_pid_list_clean( &l->pids );
    sid_out_del_output_link( l );
    sid_out_del_sid_link( l );
    free( l->pmt_section );
    sid_out_Dbg( l, "removed" );
    free( l );

    if ( i_ca_handle && PMTNeedsDescrambling( sid->current_pmt ) )
    {
        if ( SIDIsSelected( sid ) )
            en50221_UpdatePMT( sid->current_pmt );
        else
            en50221_DeletePMT( sid->current_pmt );
    }
}

uint16_t demux_remap_pid( output_t *output, uint16_t pid )
{
    if ( !output )
        return pid;

    pid_out_list_each( &p_pids[pid].outputs, l )
        if ( l->new_pid )
            return l->new_pid;

    if ( b_do_remap || output->config.b_do_remap)
        if ( output->pi_newpids[pid] != UNUSED_PID )
            return output->pi_newpids[pid];

    return pid;
}

void demux_unlink_output( output_t *output )
{
    output_sid_list_clean( &output->sids );
}

/*
 * Remap an ES pid to a fixed value.
 * Multiple streams of the same type use sequential pids
 * Returns the new pid and updates the map tables
*/
static uint16_t map_es_pid(output_t * p_output, uint8_t *p_es, uint16_t i_pid)
{
    uint16_t i_newpid = i_pid;
    uint16_t i_stream_type = pmtn_get_streamtype(p_es);

    if ( !b_do_remap && !p_output->config.b_do_remap )
        return i_pid;

    output_Dbg( p_output, "REMAP: Found elementary stream type 0x%02x "
               "with original PID 0x%x (%u):", i_stream_type, i_pid, i_pid);

    es_type_t type = es_type( p_es );
    if ( type == ES_TYPE_PRIVATE )
    {
        type = es_private_type( p_es );
        output_Dbg( p_output, "REMAP: PES Private Data stream identified "
                   "as [%s]", es_type_str( type ));
    }

    pidmap_offset offset = es_type_offset( type );
    if ( offset != -1 )
        i_newpid = b_do_remap ?
            pi_newpids[offset] :
            p_output->config.pi_confpids[offset];

    if (!i_newpid)
        return i_pid;

    /* Got the new base for the mapped pid. Find the next free one
       we do this to ensure that multiple audios get unique pids */
    while (p_output->pi_freepids[i_newpid] != UNUSED_PID)
        i_newpid++;
    p_output->pi_freepids[i_newpid] = i_pid;  /* Mark as in use */
    p_output->pi_newpids[i_pid] = i_newpid;   /* Save the new pid */

    output_Dbg( p_output, "REMAP: => Elementary stream is "
               "remapped to PID 0x%x (%u)", i_newpid, i_newpid);

    return i_newpid;
}

/*****************************************************************************
 * Print info
 *****************************************************************************/
static void PrintCb_pid_list( const pid_list_t *pids, const char *indent )
{
    pid_list_each( pids, ref )
    {
        const ts_pid_t *p = ref->pid;
        const ts_sid_t *s = ref->sid;
        if ( !p )
            continue;

        uint64_t i_bitrate = p->packets_passed * TS_SIZE * 8 * 1000000 / i_print_period;
        switch (i_print_type)
        {
            case PRINT_XML:
                fprintf(print_fh,
                        "<PID number=\"%u\" bitrate=\"%"PRIu64"\"/>",
                        p->id, i_bitrate);
                break;
            case PRINT_TEXT: {
                const char *desc = "";
                if ( ref->type == ES_TYPE_AUDIO )
                    desc = ref->desc.audio.lang;
                else if ( ref->type == ES_TYPE_SUBTITLE )
                    desc = ref->desc.subtitle.lang;

                char pid_str[32];
                snprintf( pid_str, 32, "pid[%u]", p->id );
                fprintf( print_fh, "%s %s %-9s %8s %3s bitrate: %"PRIu64"\n",
                         indent,
                         s && p->id == s->pcr_pid ? ">" : "-", pid_str,
                         es_type_str(ref->type), desc, i_bitrate );
                break;
            }
            default:
                break;
        }
    }
}

static void PrintCb( struct ev_loop *loop, struct ev_timer *w, int revents )
{
    uint64_t i_bitrate = i_nb_packets * TS_SIZE * 8 * 1000000 / i_print_period;
    switch (i_print_type)
    {
        case PRINT_XML:
            fprintf(print_fh,
                    "<STATUS type=\"bitrate\" status=\"%d\" value=\"%"PRIu64"\">",
                    i_bitrate ? 1 : 0, i_bitrate);
            break;
        case PRINT_TEXT:
            fprintf(print_fh, "bitrate: %"PRIu64"\n", i_bitrate);
            break;
        default:
            break;
    }
    i_nb_packets = 0;

    PrintCb_pid_list( &psi_list, "" );
    PrintCb_pid_list( &emm_list, "" );

    sid_list_each( &sids, p_sid )
    {
        uint64_t i_bitrate = p_sid->packets_passed * TS_SIZE * 8 * 1000000 / i_print_period;
        const char *name = NULL;
        const char *provider = NULL;
        sid_sdt_entries_each( &p_sid->sdt_entries, s ) {
            name = s->name;
            provider = s->provider;
        }

        switch (i_print_type)
        {
            case PRINT_XML:
                fprintf(print_fh,
                        "<PROGRAM number=\"%u\"%s%s%s%s%s%s bitrate=\"%"PRIu64"\">",
                        p_sid->id,
                        name ? " name=\"" : "",
                        name ?: "",
                        name ? "\"" : "",
                        provider ? " provider=\"" : "",
                        provider ?: "",
                        provider ? "\"" : "",
                        i_bitrate);
                break;
            case PRINT_TEXT:
                if ( name )
                    fprintf(print_fh, " - program number %u (%s%s%s) bitrate: %"PRIu64"\n",
                            p_sid->id,
                            name, provider ? " from " : "", provider ?: "",
                            i_bitrate);
                else
                    fprintf(print_fh, " - program number %u bitrate: %"PRIu64"\n",
                            p_sid->id, i_bitrate);
                break;
            default:
                break;
        }
        p_sid->packets_passed = 0;

        PrintCb_pid_list( &p_sid->pmt, "  ");
        PrintCb_pid_list( &p_sid->ecms, "  ");
        PrintCb_pid_list( &p_sid->pids, "  ");

        switch (i_print_type)
        {
            case PRINT_XML:
                fprintf(print_fh, "</PROGRAM>");
                break;
            default:
                break;
        }

    }

    switch (i_print_type)
    {
        case PRINT_XML:
            fprintf(print_fh, "</STATUS>\n");
            break;
        default:
            break;
    }

    if ( i_nb_invalids )
    {
        switch (i_print_type)
        {
            case PRINT_XML:
                fprintf(print_fh,
                        "<ERROR type=\"invalid_ts\" number=\"%"PRIu64"\" />\n",
                        i_nb_invalids);
                break;
            case PRINT_TEXT:
                fprintf(print_fh, "invalids: %"PRIu64"\n", i_nb_invalids);
                break;
            default:
                break;
        }
        i_nb_invalids = 0;
    }

    if ( i_nb_discontinuities )
    {
        switch (i_print_type)
        {
            case PRINT_XML:
                fprintf(print_fh,
                        "<ERROR type=\"invalid_discontinuity\" number=\"%"PRIu64"\" />\n",
                        i_nb_discontinuities);
                break;
            case PRINT_TEXT:
                fprintf(print_fh, "discontinuities: %"PRIu64"\n",
                        i_nb_discontinuities);
                break;
            default:
                break;
        }
        i_nb_discontinuities = 0;
    }

    if ( i_nb_errors )
    {
        switch (i_print_type)
        {
            case PRINT_XML:
                fprintf(print_fh,
                        "<ERROR type=\"transport_error\" number=\"%"PRIu64"\" />\n",
                        i_nb_errors);
                break;
            case PRINT_TEXT:
                fprintf(print_fh, "errors: %"PRIu64"\n", i_nb_errors);
                break;
            default:
                break;
        }
        i_nb_errors = 0;
    }
}

static void PrintESCb( struct ev_loop *loop, struct ev_timer *w, int revents )
{
    ts_pid_t *p_pid = container_of( w, ts_pid_t, timeout_watcher );
    uint16_t i_pid = p_pid->id;

    switch (i_print_type)
    {
        case PRINT_XML:
            fprintf(print_fh,
                    "<STATUS type=\"pid\" pid=\"%"PRIu16"\" status=\"0\" />\n",
                    i_pid);
            break;
        case PRINT_TEXT:
            fprintf(print_fh, "pid: %"PRIu16" down\n", i_pid);
            break;
        default:
            break;
    }

    ev_timer_stop( loop, w );
    p_pid->i_pes_status = -1;
}

static void PrintES( uint16_t i_pid )
{
    const ts_pid_t *p_pid = &p_pids[i_pid];

    switch (i_print_type)
    {
        case PRINT_XML:
            fprintf(print_fh,
                    "<STATUS type=\"pid\" pid=\"%"PRIu16"\" status=\"1\" pes=\"%d\" />\n",
                    i_pid, p_pid->i_pes_status == 1 ? 1 : 0);
            break;
        case PRINT_TEXT:
            fprintf(print_fh, "pid: %"PRIu16" up%s\n",
                    i_pid, p_pid->i_pes_status == 1 ? " pes" : "");
            break;
        default:
            break;
    }
}

/*****************************************************************************
 * demux_Open
 *****************************************************************************/
void demux_Open( void )
{
    int i;

    pf_Open();

    for ( i = 0; i < MAX_PIDS; i++ )
        ts_pid_init( &p_pids[i], i );

    if ( b_budget_mode )
        i_demux_fd = pf_SetFilter(8192);

    psi_table_init( pp_current_pat_sections );
    psi_table_init( pp_next_pat_sections );
    pid_use( &psi_list, PAT_PID, NULL, ES_TYPE_PAT );
    if ( b_enable_emm )
    {
        psi_table_init( pp_current_cat_sections );
        psi_table_init( pp_next_cat_sections );
        pid_use( &psi_list, CAT_PID, NULL, ES_TYPE_CAT );
    }
    pid_use( &psi_list, NIT_PID, NULL, ES_TYPE_NIT );
    psi_table_init( pp_current_sdt_sections );
    psi_table_init( pp_next_sdt_sections );
    pid_use( &psi_list, SDT_PID, NULL, ES_TYPE_SDT );
    pid_use( &psi_list, EIT_PID, NULL, ES_TYPE_EIT );
    pid_use( &psi_list, RST_PID, NULL, ES_TYPE_RST );
    pid_use( &psi_list, TDT_PID, NULL, ES_TYPE_TDT );

    if ( i_print_period )
    {
        ev_timer_init( &print_watcher, PrintCb,
                       i_print_period / 1000000., i_print_period / 1000000. );
        ev_timer_start( event_loop, &print_watcher );
    }
}

/*****************************************************************************
 * demux_Close
 *****************************************************************************/
void demux_Close( void )
{
    psi_table_free( pp_current_pat_sections );
    psi_table_free( pp_next_pat_sections );
    psi_table_free( pp_current_cat_sections );
    psi_table_free( pp_next_cat_sections );
    psi_table_free( pp_current_nit_sections );
    psi_table_free( pp_next_nit_sections );
    psi_table_free( pp_current_sdt_sections );
    psi_table_free( pp_next_sdt_sections );

    for ( int i = 0; i < MAX_PIDS; i++ )
        ts_pid_clean( &p_pids[i] );

    sid_list_clean( &sids );

#ifdef HAVE_ICONV
    if (iconv_handle != (iconv_t)-1) {
        iconv_close(iconv_handle);
        iconv_handle = (iconv_t)-1;
    }
#endif

    pid_list_clean( &emm_list );
    pid_list_clean( &psi_list );

    if ( i_print_period )
        ev_timer_stop( event_loop, &print_watcher );
}

/*****************************************************************************
 * demux_Run
 *****************************************************************************/
void demux_Run( block_t *p_ts )
{
    i_wallclock = mdate();
    mrtgAnalyse( p_ts );
    SetDTS( p_ts );

    while ( p_ts != NULL )
    {
        block_t *p_next = p_ts->p_next;
        p_ts->p_next = NULL;
        demux_Handle( p_ts );
        p_ts = p_next;
    }
}

/*****************************************************************************
 * demux_Handle
 *****************************************************************************/
static void demux_Handle( block_t *p_ts )
{
    uint16_t i_pid = ts_get_pid( p_ts->p_ts );
    ts_pid_t *p_pid = &p_pids[i_pid];
    uint8_t i_cc = ts_get_cc( p_ts->p_ts );

    i_nb_packets++;

    if ( !ts_validate( p_ts->p_ts ) )
    {
        Warn( "lost TS sync" );
        block_Delete( p_ts );
        i_nb_invalids++;
        return;
    }

    if ( i_pid != PADDING_PID )
        p_pid->info.i_scrambling = ts_get_scrambling( p_ts->p_ts );

    p_pid->info.i_last_packet_ts = i_wallclock;
    p_pid->info.i_packets++;

    p_pid->packets_passed++;

    /* Calculate bytes_per_sec */
    if ( i_wallclock > p_pid->i_bytes_ts + 1000000 ) {
        p_pid->info.i_bytes_per_sec = p_pid->packets_passed * TS_SIZE;
        p_pid->packets_passed = 0;
        p_pid->i_bytes_ts = i_wallclock;
    }

    if ( p_pid->info.i_first_packet_ts == 0 )
        p_pid->info.i_first_packet_ts = i_wallclock;

    if ( i_print_period )
    {
        pid_refcount_list_each(&p_pid->refcounts, ref )
            if ( ref->sid )
                ref->sid->packets_passed++;
    }

    if ( i_pid != PADDING_PID && p_pid->i_last_cc != -1
          && !ts_check_duplicate( i_cc, p_pid->i_last_cc )
          && ts_check_discontinuity( i_cc, p_pid->i_last_cc ) )
    {
        unsigned int expected_cc = (p_pid->i_last_cc + 1) & 0x0f;
        uint16_t i_sid = 0;
        const char *pid_desc = get_pid_desc(i_pid, &i_sid);

        p_pid->info.i_cc_errors++;
        i_nb_discontinuities++;

        Warn( "TS discontinuity on pid %4hu expected_cc %2u got %2u (%s, sid %d)",
              i_pid, expected_cc, i_cc, pid_desc, i_sid );
    }

    if ( ts_get_transporterror( p_ts->p_ts ) )
    {
        uint16_t i_sid = 0;
        const char *pid_desc = get_pid_desc(i_pid, &i_sid);

        p_pid->info.i_transport_errors++;

        Warn( "transport_error_indicator on pid %hu (%s, sid %u)",
              i_pid, pid_desc, i_sid );

        i_nb_errors++;
        i_tuner_errors++;
        i_last_error = i_wallclock;
    }
    else if ( i_wallclock > i_last_error + WATCHDOG_WAIT )
        i_tuner_errors = 0;

    if ( i_tuner_errors > MAX_ERRORS )
    {
        i_tuner_errors = 0;
        Warn( "too many transport errors, tuning again" );
        switch (i_print_type) {
        case PRINT_XML:
            fprintf(print_fh, "<EVENT type=\"reset\" cause=\"transport\" />\n");
            break;
        case PRINT_TEXT:
            fprintf(print_fh, "reset cause: transport\n");
            break;
        default:
            break;
        }
        pf_Reset();
    }

    if ( i_es_timeout )
    {
        int i_pes_status = -1;
        if ( ts_get_scrambling( p_ts->p_ts ) )
            i_pes_status = 0;
        else if ( ts_get_unitstart( p_ts->p_ts ) )
        {
            uint8_t *p_payload = ts_payload( p_ts->p_ts );
            if ( p_payload + 3 < p_ts->p_ts + TS_SIZE )
                i_pes_status = pes_validate( p_payload ) ? 1 : 0;
        }

        if ( i_pes_status != -1 )
        {
            if ( p_pid->i_pes_status == -1 )
            {
                p_pid->i_pes_status = i_pes_status;
                PrintES( i_pid );

                if ( i_pid != TDT_PID )
                {
                    ev_timer_init( &p_pid->timeout_watcher, PrintESCb,
                                   i_es_timeout / 1000000.,
                                   i_es_timeout / 1000000. );
                    ev_timer_start( event_loop, &p_pid->timeout_watcher );
                }
                else
                {
                    ev_timer_init( &p_pid->timeout_watcher, PrintESCb, 30, 30 );
                    ev_timer_start( event_loop, &p_pid->timeout_watcher );
                }
            }
            else
            {
                if ( p_pid->i_pes_status != i_pes_status )
                {
                    p_pid->i_pes_status = i_pes_status;
                    PrintES( i_pid );
                }

                ev_timer_again( event_loop, &p_pid->timeout_watcher );
            }
        }
    }

    if ( !ts_get_transporterror( p_ts->p_ts ) )
    {
        /* PSI parsing */
        if ( i_pid == TDT_PID || i_pid == RST_PID )
            SendTDT( p_ts );
        else if ( p_pid->b_psi )
            HandlePSIPacket( p_ts->p_ts, p_ts->i_dts );

        if ( b_enable_emm && p_pid->b_emm )
            SendEMM( p_ts );
    }

    p_pid->i_last_cc = i_cc;

    /* Output */
    pid_out_list_each( &p_pid->outputs, pid_out )
    {
        sid_out_t *sid_out = pid_out->sid_out;
        output_t *p_output = sid_out->output;

        if ( p_output != NULL )
        {
            if ( i_ca_handle && (p_output->config.i_config & OUTPUT_WATCH) &&
                 ts_get_unitstart( p_ts->p_ts ) )
            {
                uint8_t *p_payload;

                if ( ts_get_scrambling( p_ts->p_ts ) ||
                     ( p_pid->b_pes
                        && (p_payload = ts_payload( p_ts->p_ts )) + 3
                             < p_ts->p_ts + TS_SIZE
                          && !pes_validate(p_payload) ) )
                {
                    if ( i_wallclock >
                            i_last_reset + WATCHDOG_REFRACTORY_PERIOD )
                    {
                        p_output->i_nb_errors++;
                        p_output->i_last_error = i_wallclock;
                    }
                }
                else if ( i_wallclock > p_output->i_last_error + WATCHDOG_WAIT )
                    p_output->i_nb_errors = 0;

                if ( p_output->i_nb_errors > MAX_ERRORS )
                {
                    output_list_each( &outputs, output )
                        output->i_nb_errors = 0;

                    output_Warn( p_output, "too many errors, resetting" );

                    switch (i_print_type) {
                    case PRINT_XML:
                        fprintf(print_fh, "<EVENT type=\"reset\" cause=\"scrambling\" />\n");
                        break;
                    case PRINT_TEXT:
                        fprintf(print_fh, "reset cause: scrambling");
                        break;
                    default:
                        break;
                    }
                    i_last_reset = i_wallclock;
                    en50221_Reset();
                }
            }

            if ( !pid_out->pcr_only
                 || (ts_has_adaptation(p_ts->p_ts)
                     && ts_get_adaptation(p_ts->p_ts)
                     && tsaf_has_pcr(p_ts->p_ts)) )
                output_Put( p_output, p_ts );

            if ( p_output->p_eit_ts_buffer != NULL
                  && p_ts->i_dts > p_output->p_eit_ts_buffer->i_dts
                                    + MAX_EIT_RETENTION )
                FlushEIT( p_output, p_ts->i_dts );
        }
    }

    output_list_each( &outputs, output )
        if ( config_sid_list_is_empty( &output->config.sids ) )
            output_Put( output, p_ts );

    if ( output_dup )
        output_Put( output_dup, p_ts );

    if ( config_get_passthrough() )
        fwrite(p_ts->p_ts, TS_SIZE, 1, stdout);

    p_ts->i_refcount--;
    if ( !p_ts->i_refcount )
        block_Delete( p_ts );
}

/*****************************************************************************
 * GetPIDS
 *****************************************************************************/
static bool GetPIDS( sid_out_t *sid_out )
{
    if ( !sid_out )
        return false;

    output_t *output = sid_out->output;
    ts_sid_t *sid = sid_out->sid;
    bool changed = false;

    sid_out_pid_list_t old_pids;
    sid_out_pid_list_move( &sid_out->pids, &old_pids );
    if ( !output || !sid ) {
        changed = changed || !sid_out_pid_list_is_empty( &old_pids );
        sid_out_pid_list_clean( &old_pids );
        return changed;
    }

    uint16_t pmt_pid = sid->pmt_pid;
    uint16_t pcr_pid = sid->pcr_pid;

    config_pid_list_t *confs = NULL;
    if ( sid_out->conf && !config_pid_list_is_empty( &sid_out->conf->pids ) )
        confs = &sid_out->conf->pids;
    else if ( !config_pid_list_is_empty( &output->config.pids ) )
        confs = &output->config.pids;

    unsigned es_type_count[CHAR_MAX];
    memset(es_type_count, 0, sizeof (es_type_count));
    pid_out_t *l;

    pid_list_each( &sid->pids, pid_sid )
    {
        uint16_t i_pid = pid_sid->pid->id;
        es_type_t es_type = pid_sid->type;

        if ( es_type < CHAR_MAX )
            es_type_count[es_type]++;
        es_type_count[ES_TYPE_ANY]++;

        bool b_select = confs ? false : PIDWouldBeSelected( es_type );
        config_pid_t *conf = NULL;
        if ( confs )
        {
            config_pid_list_each( confs, p )
            {
                switch (p->type) {
                    case OUTPUT_CONFIG_TYPE_STATIC:
                        if ( p->value != i_pid )
                            continue;
                        break;
                    case OUTPUT_CONFIG_TYPE_COUNT:
                        if ( p->value != es_type_count[p->es_type] )
                            continue;
                        break;
                    case OUTPUT_CONFIG_TYPE_ALL:
                        break;
                    case OUTPUT_CONFIG_TYPE_ANY: {
                        bool found = false;
                        sid_out_pid_list_each( &sid_out->pids, ll )
                            if ( ll->conf == p ) {
                                found = true;
                                break;
                            }
                        if ( found )
                            continue;
                    }
                }

                if ( pid_match_conf( pid_sid->pid, p ) )
                {
                    b_select = true;
                    conf = p;
                    break;
                }
            }
        }

        if ( !b_select ) continue;

        l = pid_out_link( &old_pids, sid_out, i_pid );
        if ( !l ) continue;
        l->conf = conf;
        pid_out_remap( l, conf ? conf->new_pid : 0 );
        changed = l->changed || changed;
        sid_out_pid_list_add( &sid_out->pids, l );

        pid_list_each( &pid_sid->ecms, ecm ) {
            l = pid_out_link( &old_pids, sid_out, ecm->pid->id );
            if ( !l ) continue;
            changed = l->changed || changed;
            sid_out_pid_list_add( &sid_out->pids, l );
        }
    }

    pid_list_each( &sid->ecms, ecm ) {
        l = pid_out_link( &old_pids, sid_out, ecm->pid->id );
        if ( !l ) continue;
        changed = l->changed || changed;
        sid_out_pid_list_add( &sid_out->pids, l );
    }

    bool pcr_selected = pid_out_find( &sid_out->pids, sid_out, pcr_pid );
    if ( !pcr_selected && pcr_pid != PADDING_PID && pcr_pid != pmt_pid )
    {
        l = pid_out_link_pcr( &old_pids, sid_out, pcr_pid );
        if ( l )
        {
            /* We only need the PCR packets of this stream (incomplete) */
            changed = l->changed || changed;
            sid_out_pid_list_add( &sid_out->pids, l );
        }
    }

    /* remove old pids */
    changed = changed || !sid_out_pid_list_is_empty( &old_pids );
    sid_out_pid_list_clean( &old_pids );

    return changed;
}

/*****************************************************************************
 * demux_Change : called from main thread
 *****************************************************************************/
void demux_Change( output_t *p_output, output_config_t *p_config )
{
    bool b_sid_change = false, b_pid_change = false, b_tsid_change = false;
    bool b_dvb_change = !!((p_output->config.i_config ^ p_config->i_config)
                             & OUTPUT_DVB);
    bool b_epg_change = !!((p_output->config.i_config ^ p_config->i_config)
                             & OUTPUT_EPG);
    bool b_network_change =
        (dvb_string_cmp(&p_output->config.network_name, &p_config->network_name) ||
         p_output->config.i_network_id != p_config->i_network_id);
    bool b_service_name_change = false;
    bool b_remap_change =
        p_output->config.i_onid != p_config->i_onid ||
        p_output->config.b_do_remap != p_config->b_do_remap ||
        p_output->config.pi_confpids[I_PMTPID] != p_config->pi_confpids[I_PMTPID] ||
        p_output->config.pi_confpids[I_APID] != p_config->pi_confpids[I_APID] ||
        p_output->config.pi_confpids[I_VPID] != p_config->pi_confpids[I_VPID] ||
        p_output->config.pi_confpids[I_SPUPID] != p_config->pi_confpids[I_SPUPID];

    if ( (p_output->config.psz_displayname && !p_config->psz_displayname) ||
         (!p_output->config.psz_displayname && p_config->psz_displayname) ||
         (p_output->config.psz_displayname && p_config->psz_displayname &&
          strcmp( p_output->config.psz_displayname,
                  p_config->psz_displayname ) ) )
    {
        free( p_output->config.psz_displayname );
        p_output->config.psz_displayname = p_config->psz_displayname ?
            strdup( p_config->psz_displayname ) : NULL;
    }

    p_output->config.i_config = p_config->i_config;
    p_output->config.i_network_id = p_config->i_network_id;
    p_output->config.i_onid = p_config->i_onid;
    p_output->config.b_do_remap = p_config->b_do_remap;
    memcpy(p_output->config.pi_confpids, p_config->pi_confpids,
           sizeof(uint16_t) * N_MAP_PIDS);

    /* Change output settings related to names. */
    dvb_string_clean( &p_output->config.network_name );
    dvb_string_copy( &p_output->config.network_name,
                     &p_config->network_name );

    if ( p_config->i_tsid != -1 && p_output->config.i_tsid != p_config->i_tsid )
    {
        p_output->i_tsid = p_output->config.i_tsid = p_config->i_tsid;
        b_tsid_change = true;
    }
    if ( p_config->i_tsid == -1 && p_output->config.i_tsid != -1 )
    {
        p_output->config.i_tsid = p_config->i_tsid;
        if ( psi_table_validate(pp_current_pat_sections) && !b_random_tsid )
            p_output->i_tsid =
                psi_table_get_tableidext(pp_current_pat_sections);
        else
            p_output->i_tsid = rand() & 0xffff;
        b_tsid_change = true;
    }

    /* import new config list */

    config_sid_list_t old_sids_config;
    config_sid_list_move( &p_output->config.sids, &old_sids_config );
    config_sid_list_move( &p_config->sids, &p_output->config.sids );

    config_pid_list_t old_pids_config;
    config_pid_list_move( &p_output->config.pids, &old_pids_config );
    config_pid_list_move( &p_config->pids, &p_output->config.pids );

    /* services */

    output_sid_list_t old_sids;
    output_sid_list_move( &p_output->sids, &old_sids );
    config_sid_list_each( &p_output->config.sids, conf )
    {
        ts_sid_t *sid = NULL;
        do {
            sid = sid_find_from_config( &sids, &p_output->sids, conf );
            sid_out_t *l = sid_out_link( &old_sids, p_output, sid );
            if ( !l ) continue;
            if ( l->conf ) {
                if ( dvb_string_cmp( &l->conf->new_name, &conf->new_name ) ||
                     dvb_string_cmp( &l->conf->new_provider, &conf->new_provider ) )
                    b_service_name_change = true;
            }
            l->conf = conf;
            sid_out_remap( l, conf->new_sid, conf->pmt_pid );
            b_sid_change = l->changed || b_sid_change;
            output_sid_list_add( &p_output->sids, l );
        } while ( conf->type == OUTPUT_CONFIG_TYPE_ALL && sid );
    }

    /* pids */

    output_sid_list_each( &p_output->sids, l )
    {
        l->pid_changed = GetPIDS( l );
        b_pid_change = l->pid_changed || b_pid_change;
    }

    /* remove old services */
    b_sid_change = b_sid_change || !output_sid_list_is_empty( &old_sids );
    output_sid_list_clean( &old_sids );

    /* delete old confif list */
    config_pid_list_clean( &old_pids_config );
    config_sid_list_clean( &old_sids_config );

    if ( b_sid_change || b_pid_change || b_tsid_change || b_dvb_change ||
         b_network_change || b_service_name_change || b_remap_change )
    {
        output_Dbg( p_output, "change %s%s%s%s%s%s%s",
                   b_sid_change ? "sid " : "",
                   b_pid_change ? "pid " : "",
                   b_tsid_change ? "tsid " : "",
                   b_dvb_change ? "dvb " : "",
                   b_network_change ? "network " : "",
                   b_service_name_change ? "service_name " : "",
                   b_remap_change ? "remap " : "" );
    }

    if ( b_sid_change || b_remap_change )
    {
        NewSDT( p_output );
        NewNIT( p_output );
        NewPAT( p_output );
        output_sid_list_each( &p_output->sids, sid_out )
            NewPMT( sid_out );
    }
    else
    {
        if ( b_tsid_change )
        {
            NewSDT( p_output );
            NewNIT( p_output );
            NewPAT( p_output );
        }
        else if ( b_dvb_change )
        {
            NewNIT( p_output );
            NewPAT( p_output );
        }
        else if ( b_network_change )
            NewNIT( p_output );

        if ( !b_tsid_change && (b_service_name_change || b_epg_change) )
            NewSDT( p_output );

        if ( b_pid_change )
            output_sid_list_each( &p_output->sids, sid_out )
                if ( sid_out->pid_changed )
                    NewPMT( sid_out );
    }
}

/*****************************************************************************
 * SetDTS
 *****************************************************************************/
static void SetDTS( block_t *p_list )
{
    int i_nb_ts = 0, i;
    mtime_t i_duration;
    block_t *p_ts = p_list;

    while ( p_ts != NULL )
    {
        i_nb_ts++;
        p_ts = p_ts->p_next;
    }

    /* We suppose the stream is CBR, at least between two consecutive read().
     * This is especially true in budget mode */
    if ( i_last_dts == -1 )
        i_duration = 0;
    else
        i_duration = i_wallclock - i_last_dts;

    p_ts = p_list;
    i = i_nb_ts - 1;
    while ( p_ts != NULL )
    {
        p_ts->i_dts = i_wallclock - i_duration * i / i_nb_ts;
        i--;
        p_ts = p_ts->p_next;
    }

    i_last_dts = i_wallclock;
}

/*****************************************************************************
 * OutputPSISection
 *****************************************************************************/
static void OutputPSISection( output_t *p_output, uint8_t *p_section,
                              uint16_t i_pid, uint8_t *pi_cc, mtime_t i_dts,
                              block_t **pp_ts_buffer,
                              uint8_t *pi_ts_buffer_offset )
{
    uint16_t i_section_length = psi_get_length(p_section) + PSI_HEADER_SIZE;
    uint16_t i_section_offset = 0;

    do
    {
        block_t *p_block;
        uint8_t *p;
        uint8_t i_ts_offset;
        bool b_append = (pp_ts_buffer != NULL && *pp_ts_buffer != NULL);

        if ( b_append )
        {
            p_block = *pp_ts_buffer;
            i_ts_offset = *pi_ts_buffer_offset;
        }
        else
        {
            p_block = block_New();
            p_block->i_dts = i_dts;
            i_ts_offset = 0;
        }
        p = p_block->p_ts;

        psi_split_section( p, &i_ts_offset, p_section, &i_section_offset );

        if ( !b_append )
        {
            ts_set_pid( p, i_pid );
            ts_set_cc( p, *pi_cc );
            (*pi_cc)++;
            *pi_cc &= 0xf;
        }

        if ( i_section_offset == i_section_length )
        {
            if ( i_ts_offset < TS_SIZE - MIN_SECTION_FRAGMENT
                  && pp_ts_buffer != NULL )
            {
                *pp_ts_buffer = p_block;
                *pi_ts_buffer_offset = i_ts_offset;
                break;
            }
            else
                psi_split_end( p, &i_ts_offset );
        }

        p_block->i_dts = i_dts;
        p_block->i_refcount--;
        output_Put( p_output, p_block );
        if ( pp_ts_buffer != NULL )
        {
            *pp_ts_buffer = NULL;
            *pi_ts_buffer_offset = 0;
        }
    }
    while ( i_section_offset < i_section_length );
}

/*****************************************************************************
 * SendPAT
 *****************************************************************************/
static void SendPAT( mtime_t i_dts )
{
    output_list_each( &outputs, output )
    {
        if ( config_sid_list_is_empty( &output->config.sids ) )
            continue;

        if ( output->p_pat_section == NULL &&
             psi_table_validate(pp_current_pat_sections) )
        {
            /* SID doesn't exist - build an empty PAT. */
            uint8_t *p;
            output->i_pat_version++;

            p = output->p_pat_section = psi_allocate();
            pat_init( p );
            pat_set_length( p, 0 );
            pat_set_tsid( p, output->i_tsid );
            psi_set_version( p, output->i_pat_version );
            psi_set_current( p );
            psi_set_section( p, 0 );
            psi_set_lastsection( p, 0 );
            psi_set_crc( output->p_pat_section );
        }


        if ( output->p_pat_section != NULL )
            OutputPSISection( output, output->p_pat_section, PAT_PID,
                              &output->i_pat_cc, i_dts, NULL, NULL );
    }
}

/*****************************************************************************
 * SendPMT
 *****************************************************************************/
static void SendPMT( ts_sid_t *p_sid, mtime_t i_dts )
{
    int i_pmt_pid = p_sid->pmt_pid;

    if ( b_do_remap )
        i_pmt_pid = pi_newpids[ I_PMTPID ];

    sid_out_list_each( &p_sid->outputs, l )
    {
        output_t *output = l->output;

        if ( l->pmt_section != NULL )
        {
            if ( output->config.b_do_remap && output->config.pi_confpids[I_PMTPID] )
                i_pmt_pid = output->config.pi_confpids[I_PMTPID];
            else if ( l->pmt_pid )
                i_pmt_pid = l->pmt_pid;

            OutputPSISection( output, l->pmt_section,
                              i_pmt_pid, &l->pmt_cc, i_dts,
                              NULL, NULL );
        }
    }
}

/*****************************************************************************
 * SendNIT
 *****************************************************************************/
static void SendNIT( mtime_t i_dts )
{
    output_list_each( &outputs, output )
    {
        if ( !config_sid_list_is_empty( &output->config.sids )
             && (output->config.i_config & OUTPUT_DVB)
             && output->p_nit_section != NULL )
            OutputPSISection( output, output->p_nit_section, NIT_PID,
                              &output->i_nit_cc, i_dts, NULL, NULL );
    }
}

/*****************************************************************************
 * SendSDT
 *****************************************************************************/
static void SendSDT( mtime_t i_dts )
{
    output_list_each( &outputs, output )
    {
        if ( !config_sid_list_is_empty( &output->config.sids )
             && (output->config.i_config & OUTPUT_DVB)
             && output->p_sdt_section != NULL )
            OutputPSISection( output, output->p_sdt_section, SDT_PID,
                              &output->i_sdt_cc, i_dts, NULL, NULL );
    }
}

/*****************************************************************************
 * SendEIT
 *****************************************************************************/
static bool IsEITpf( int i_table_id )
{
    return i_table_id == EIT_TABLE_ID_PF_ACTUAL;
}

static bool IsEPG( int i_table_id )
{
    /* We only handle EPG for the current (actual) TS, not others. */
    return i_table_id >= EIT_TABLE_ID_SCHED_ACTUAL_FIRST &&
           i_table_id <= EIT_TABLE_ID_SCHED_ACTUAL_LAST;
}

static void SendEIT( ts_sid_t *p_sid, mtime_t i_dts, uint8_t *p_eit )
{
    uint8_t i_table_id = psi_get_tableid( p_eit );
    bool b_epg = IsEPG( i_table_id );
    uint16_t i_onid = eit_get_onid(p_eit);

    sid_out_list_each( &p_sid->outputs, l )
    {
        output_t *output = l->output;

        if ( !config_sid_list_is_empty( &output->config.sids )
             && (output->config.i_config & OUTPUT_DVB)
             && (!b_epg || (output->config.i_config & OUTPUT_EPG)) )
        {
            eit_set_tsid( p_eit, output->i_tsid );

            if ( l->new_sid )
                eit_set_sid( p_eit, l->new_sid );
            else
                eit_set_sid( p_eit, p_sid->id );

            if ( output->config.i_onid )
                eit_set_onid( p_eit, output->config.i_onid );

            psi_set_crc( p_eit );

            OutputPSISection( output, p_eit, EIT_PID, &output->i_eit_cc,
                              i_dts, &output->p_eit_ts_buffer,
                              &output->i_eit_ts_buffer_offset );

            if ( output->config.i_onid )
                eit_set_onid( p_eit, i_onid );
        }
    }
}

/*****************************************************************************
 * FlushEIT
 *****************************************************************************/
static void FlushEIT( output_t *p_output, mtime_t i_dts )
{
    block_t *p_block = p_output->p_eit_ts_buffer;

    psi_split_end( p_block->p_ts, &p_output->i_eit_ts_buffer_offset );
    p_block->i_dts = i_dts;
    p_block->i_refcount--;
    output_Put( p_output, p_block );
    p_output->p_eit_ts_buffer = NULL;
    p_output->i_eit_ts_buffer_offset = 0;
}

/*****************************************************************************
 * SendTDT
 *****************************************************************************/
static void SendTDT( block_t *p_ts )
{
    output_list_each( &outputs, output )
    {
        if ( !config_sid_list_is_empty( &output->config.sids )
             && (output->config.i_config & OUTPUT_DVB)
             && output->p_sdt_section != NULL )
            output_Put( output, p_ts );
    }
}
/*****************************************************************************
 * SendEMM
 *****************************************************************************/
static void SendEMM( block_t *p_ts )
{
    output_list_each( &outputs, output )
    {
        if ( !config_sid_list_is_empty( &output->config.sids ) )
            output_Put( output, p_ts );
    }
}

/*****************************************************************************
 * NewPAT
 *****************************************************************************/
static void NewPAT( output_t *p_output )
{
    const uint8_t *p_program;

    free( p_output->p_pat_section );
    p_output->p_pat_section = NULL;
    p_output->i_pat_version++;

    if ( !psi_table_validate(pp_current_pat_sections) ) return;

    uint8_t *p = p_output->p_pat_section = psi_allocate();
    pat_init( p );
    psi_set_length( p, PSI_MAX_SIZE );
    pat_set_tsid( p, p_output->i_tsid );
    psi_set_version( p, p_output->i_pat_version );
    psi_set_current( p );
    psi_set_section( p, 0 );
    psi_set_lastsection( p, 0 );

    uint8_t k = 0;
    if ( p_output->config.i_config & OUTPUT_DVB )
    {
        /* NIT */
        p = pat_get_program( p_output->p_pat_section, k++ );
        patn_init( p );
        patn_set_program( p, 0 );
        patn_set_pid( p, NIT_PID );
    }

    output_sid_list_each( &p_output->sids, l )
    {
        ts_sid_t *sid = l->sid;

        p = pat_get_program( p_output->p_pat_section, k++ );
        patn_init( p );
        if ( l->new_sid )
        {
            sid_out_Dbg( l, "Mapping PAT SID to %d", l->new_sid );
            patn_set_program( p, l->new_sid );
        }
        else
            patn_set_program( p, sid->id );

        p_program = pat_table_find_program( pp_current_pat_sections, sid->id );
        if ( p_program == NULL ) return;

        if ( b_do_remap )
        {
            sid_out_Dbg( l, "Mapping PMT PID %d to %d",
                            patn_get_pid( p_program ), pi_newpids[I_PMTPID] );
            patn_set_pid( p, pi_newpids[I_PMTPID]);
        } else if ( p_output->config.b_do_remap && p_output->config.pi_confpids[I_PMTPID] ) {
            sid_out_Dbg( l, "Mapping PMT PID %d to %d",
                            patn_get_pid( p_program ),
                            p_output->config.pi_confpids[I_PMTPID] );
            patn_set_pid( p, p_output->config.pi_confpids[I_PMTPID]);
        } else if ( l->pmt_pid ) {
            sid_out_Dbg( l, "Mapping PMT PID %d to %d",
                            patn_get_pid( p_program ), l->pmt_pid );
            patn_set_pid( p, l->pmt_pid );
        } else {
            patn_set_pid( p, patn_get_pid( p_program ) );
        }
    }

    p = pat_get_program( p_output->p_pat_section, k );
    pat_set_length( p_output->p_pat_section,
                    p - p_output->p_pat_section - PAT_HEADER_SIZE );
    psi_set_crc( p_output->p_pat_section );
    pat_table_print( &p_output->p_pat_section,
                     output_Dbg, p_output, PRINT_TEXT );
}

/*****************************************************************************
 * NewPMT
 *****************************************************************************/
static void CopyDescriptors( uint8_t *p_descs, uint8_t *p_current_descs )
{
    uint8_t *p_desc = NULL;

    descs_set_length( p_descs, DESCS_MAX_SIZE );

    descs_each_desc( p_current_descs, p_current_desc )
    {
        uint8_t i_tag = desc_get_tag( p_current_desc );

        if ( !b_enable_ecm && i_tag == 0x9 ) continue;

        p_desc = descs_next_desc( p_descs, p_desc );
        if ( p_desc == NULL ) continue; /* This shouldn't happen */
        memcpy( p_desc, p_current_desc,
                DESC_HEADER_SIZE + desc_get_length( p_current_desc ) );
    }

    p_desc = descs_next_desc( p_descs, p_desc );
    if ( p_desc == NULL )
        /* This shouldn't happen if the incoming PMT is valid */
        descs_set_length( p_descs, 0 );
    else
        descs_set_length( p_descs, p_desc - p_descs - DESCS_HEADER_SIZE );
}

static void NewPMT( sid_out_t *sid_out )
{
    output_t *output = sid_out->output;
    ts_sid_t *sid = sid_out->sid;

    uint8_t *p_current_pmt;
    uint8_t *p_es = NULL;
    uint8_t *p;
    uint16_t i_pcrpid;

    if ( sid == NULL ) return;

    free( sid_out->pmt_section );
    sid_out->pmt_section = NULL;
    sid_out->pmt_version++;

    if ( sid->current_pmt == NULL ) return;
    p_current_pmt = sid->current_pmt;

    p = sid_out->pmt_section = psi_allocate();
    pmt_init( p );
    psi_set_length( p, PSI_MAX_SIZE );
    if ( sid_out->new_sid )
    {
        output_Dbg( output, "Mapping PMT SID %d to %d", sid->id,
                   sid_out->new_sid );
        pmt_set_program( p, sid_out->new_sid );
    }
    else
        pmt_set_program( p, sid->id );
    psi_set_version( p, sid_out->pmt_version );
    psi_set_current( p );
    pmt_set_desclength( p, 0 );
    init_pid_mapping( output );

    CopyDescriptors( pmt_get_descs( p ), pmt_get_descs( p_current_pmt ) );

    i_pcrpid = pmt_get_pcrpid( p_current_pmt );

    pmt_each_es( p_current_pmt, p_current_es )
    {
        uint16_t i_pid = pmtn_get_pid( p_current_es );
        pid_out_t *l;

        l = pid_out_find( &sid_out->pids, sid_out, i_pid );
        if ( !l ) continue;
        if ( l->pcr_only ) continue;
        p_es = pmt_next_es( p, p_es );
        if ( p_es == NULL ) continue; /* This shouldn't happen */
        pmtn_init( p_es );
        pmtn_set_streamtype( p_es, pmtn_get_streamtype( p_current_es ) );
        if ( l->new_pid )
        {
            pmtn_set_pid( p_es, l->new_pid );
            if ( i_pid == i_pcrpid )
                i_pcrpid = l->new_pid;
        }
        else
            pmtn_set_pid( p_es, map_es_pid( output, p_current_es, i_pid ) );
        pmtn_set_desclength( p_es, 0 );

        CopyDescriptors( pmtn_get_descs( p_es ),
                         pmtn_get_descs( p_current_es ) );
    }

    /* Do the pcr pid after everything else as it may have been remapped */
    if ( output->pi_newpids[i_pcrpid] != UNUSED_PID ) {
        output_Dbg( output, "REMAP: The PCR PID was changed "
                   "from 0x%x (%u) to 0x%x (%u)",
                   i_pcrpid, i_pcrpid,
                   output->pi_newpids[i_pcrpid], output->pi_newpids[i_pcrpid] );
        i_pcrpid = output->pi_newpids[i_pcrpid];
    } else {
        output_Dbg( output, "The PCR PID has kept its original value "
                   "of 0x%x (%u)", i_pcrpid, i_pcrpid);
    }
    pmt_set_pcrpid( p, i_pcrpid );
    p_es = pmt_next_es( p, p_es );
    if ( p_es == NULL )
        /* This shouldn't happen if the incoming PMT is valid */
        pmt_set_length( p, 0 );
    else
        pmt_set_length( p, p_es - p - PMT_HEADER_SIZE );
    psi_set_crc( p );
    pmt_print( p, output_Dbg, output, demux_Iconv, NULL, PRINT_TEXT );
}

/*****************************************************************************
 * NewNIT
 *****************************************************************************/
static void NewNIT( output_t *p_output )
{
    uint8_t *p_ts;
    uint8_t *p_header2;
    uint8_t *p;

    free( p_output->p_nit_section );
    p_output->p_nit_section = NULL;
    p_output->i_nit_version++;

    p = p_output->p_nit_section = psi_allocate();
    nit_init( p, true );
    nit_set_length( p, PSI_MAX_SIZE );
    nit_set_nid( p, p_output->config.i_network_id );
    psi_set_version( p, p_output->i_nit_version );
    psi_set_current( p );
    psi_set_section( p, 0 );
    psi_set_lastsection( p, 0 );

    if ( p_output->config.network_name.i )
    {
        uint8_t *p_descs;
        uint8_t *p_desc;
        nit_set_desclength( p, DESCS_MAX_SIZE );
        p_descs = nit_get_descs( p );
        p_desc = descs_get_desc( p_descs, 0 );
        desc40_init( p_desc );
        desc40_set_networkname( p_desc, p_output->config.network_name.p,
                                p_output->config.network_name.i );
        p_desc = descs_get_desc( p_descs, 1 );
        descs_set_length( p_descs, p_desc - p_descs - DESCS_HEADER_SIZE );
    }
    else
        nit_set_desclength( p, 0 );

    p_header2 = nit_get_header2( p );
    nith_init( p_header2 );
    nith_set_tslength( p_header2, NIT_TS_SIZE );

    p_ts = nit_get_ts( p, 0 );
    nitn_init( p_ts );
    nitn_set_tsid( p_ts, p_output->i_tsid );
    if ( p_output->config.i_onid )
        nitn_set_onid( p_ts, p_output->config.i_onid );
    else
        nitn_set_onid( p_ts, p_output->config.i_network_id );
    nitn_set_desclength( p_ts, 0 );

    p_ts = nit_get_ts( p, 1 );
    if ( p_ts == NULL )
        /* This shouldn't happen */
        nit_set_length( p, 0 );
    else
        nit_set_length( p, p_ts - p - NIT_HEADER_SIZE );
    psi_set_crc( p_output->p_nit_section );
    nit_table_print( &p_output->p_nit_section, output_Dbg, p_output,
                     demux_Iconv, NULL, PRINT_TEXT );
}

/*****************************************************************************
 * NewSDT
 *****************************************************************************/
static void NewSDT( output_t *p_output )
{
    uint8_t *p_service, *p_current_service;
    uint8_t *p;

    free( p_output->p_sdt_section );
    p_output->p_sdt_section = NULL;
    p_output->i_sdt_version++;

    if ( !psi_table_validate(pp_current_sdt_sections) ) return;

    p = p_output->p_sdt_section = psi_allocate();
    sdt_init( p, true );
    sdt_set_length( p, PSI_MAX_SIZE );
    sdt_set_tsid( p, p_output->i_tsid );
    psi_set_version( p, p_output->i_sdt_version );
    psi_set_current( p );
    psi_set_section( p, 0 );
    psi_set_lastsection( p, 0 );
    if ( p_output->config.i_onid )
        sdt_set_onid( p, p_output->config.i_onid );
    else
        sdt_set_onid( p,
            sdt_get_onid( psi_table_get_section( pp_current_sdt_sections, 0 ) ) );

    unsigned k = 0;
    output_sid_list_each( &p_output->sids, sid_out )
    {
        ts_sid_t *sid = sid_out->sid;
        config_sid_t *conf = sid_out->conf;

        p_current_service = sdt_table_find_service( pp_current_sdt_sections,
                                                    sid->id );

        if ( p_current_service == NULL )
        {
            if ( p_output->p_pat_section != NULL &&
                 pat_get_program( p_output->p_pat_section, 0 ) == NULL )
            {
                /* Empty PAT and no SDT anymore */
                free( p_output->p_pat_section );
                p_output->p_pat_section = NULL;
                p_output->i_pat_version++;
            }
            return;
        }

        p_service = sdt_get_service( p, k++ );
        sdtn_init( p_service );
        if ( sid_out->new_sid )
        {
            output_Dbg( p_output, "Mapping SDT SID %d to %d", sid->id,
                       sid_out->new_sid );
            sdtn_set_sid( p_service, sid_out->new_sid );
        }
        else
            sdtn_set_sid( p_service, sid->id );

        /* We always forward EITp/f */
        if ( sdtn_get_eitpresent(p_current_service) )
            sdtn_set_eitpresent(p_service);

        if ( (p_output->config.i_config & OUTPUT_EPG) == OUTPUT_EPG &&
             sdtn_get_eitschedule(p_current_service) )
            sdtn_set_eitschedule(p_service);

        sdtn_set_running( p_service, sdtn_get_running(p_current_service) );
        /* Do not set free_ca */
        sdtn_set_desclength( p_service, sdtn_get_desclength(p_current_service) );

        if ( !conf->new_provider.i && !conf->new_name.i ) {
            /* Copy all descriptors unchanged */
            memcpy( descs_get_desc( sdtn_get_descs(p_service), 0 ),
                    descs_get_desc( sdtn_get_descs(p_current_service), 0 ),
                    sdtn_get_desclength(p_current_service) );
        } else {
            int i_total_desc_len = 0;
            uint8_t *p_new_desc = descs_get_desc( sdtn_get_descs(p_service), 0 );

            descs_each_desc( sdtn_get_descs( p_current_service ), p_desc )
            {
                /* Regenerate descriptor 48 (service name) */
                if ( DESC_CHECK( 48, p_desc ) )
                {
                    uint8_t i_old_provider_len, i_old_service_len;
                    uint8_t i_new_desc_len = 3; /* 1 byte - type, 1 byte provider_len, 1 byte service_len */
                    const uint8_t *p_old_provider = desc48_get_provider( p_desc, &i_old_provider_len );
                    const uint8_t *p_old_service = desc48_get_service( p_desc, &i_old_service_len );

                    desc48_init( p_new_desc );
                    desc48_set_type( p_new_desc, desc48_get_type( p_desc ) );

                    if ( conf->new_provider.i ) {
                        desc48_set_provider( p_new_desc,
                                             conf->new_provider.p,
                                             conf->new_provider.i );
                        i_new_desc_len += conf->new_provider.i;
                    } else {
                        desc48_set_provider( p_new_desc, p_old_provider,
                                             i_old_provider_len );
                        i_new_desc_len += i_old_provider_len;
                    }

                    if ( conf->new_name.i ) {
                        desc48_set_service( p_new_desc,
                                            conf->new_name.p,
                                            conf->new_name.i );
                        i_new_desc_len += conf->new_name.i;
                    } else {
                        desc48_set_service( p_new_desc, p_old_service,
                                            i_old_service_len );
                        i_new_desc_len += i_old_service_len;
                    }

                    desc_set_length( p_new_desc, i_new_desc_len );
                    i_total_desc_len += DESC_HEADER_SIZE + i_new_desc_len;
                    p_new_desc += DESC_HEADER_SIZE + i_new_desc_len;
                } else {
                    /* Copy single descriptor */
                    int i_desc_len = DESC_HEADER_SIZE + desc_get_length( p_desc );
                    memcpy( p_new_desc, p_desc, i_desc_len );
                    p_new_desc += i_desc_len;
                    i_total_desc_len += i_desc_len;
                }
            }
            sdtn_set_desclength( p_service, i_total_desc_len );
        }
    }

    p_service = sdt_get_service( p, k++ );
    if ( p_service == NULL )
        /* This shouldn't happen if the incoming SDT is valid */
        sdt_set_length( p, 0 );
    else
        sdt_set_length( p, p_service - p - SDT_HEADER_SIZE );
    psi_set_crc( p_output->p_sdt_section );
    sdt_table_print( &p_output->p_sdt_section,
                     output_Dbg, p_output, demux_Iconv, NULL, PRINT_TEXT );
}

/*****************************************************************************
 * UpdatePAT/PMT/SDT
 *****************************************************************************/
#define DECLARE_UPDATE_FUNC( table )                                        \
static void Update##table( uint16_t i_sid )                                 \
{                                                                           \
    ts_sid_t *sid = sid_find( &sids, i_sid );                               \
    if ( sid )                                                              \
        sid_out_list_each( &sid->outputs, sid_out )                         \
            New##table( sid_out->output );                                  \
}

DECLARE_UPDATE_FUNC(PAT)
DECLARE_UPDATE_FUNC(SDT)

/*****************************************************************************
 * UpdatePMT
 *****************************************************************************/
static void UpdatePMT( uint16_t i_sid )
{
    ts_sid_t *sid = sid_find( &sids, i_sid );
    sid_out_list_each( &sid->outputs, sid_out )
        NewPMT( sid_out );
}

/*****************************************************************************
 * UpdateTSID
 *****************************************************************************/
static void UpdateTSID(void)
{
    uint16_t i_tsid = psi_table_get_tableidext(pp_current_pat_sections);

    output_list_each( &outputs, output )
        if ( output->config.i_tsid == -1 && !b_random_tsid )
        {
            output->i_tsid = i_tsid;
            NewNIT( output );
        }
}

/*****************************************************************************
 * SIDIsSelected
 *****************************************************************************/
static bool SIDIsSelected( ts_sid_t *sid )
{
    return sid && !sid_out_list_is_empty( &sid->outputs );
}

/*****************************************************************************
 * demux_PIDIsSelected
 *****************************************************************************/
bool demux_PIDIsSelected( uint16_t i_pid )
{
    return !pid_out_list_is_empty( &p_pids[i_pid].outputs );
}

/*****************************************************************************
 * PMTNeedsDescrambling
 *****************************************************************************/
static bool PMTNeedsDescrambling( uint8_t *p_pmt )
{
    if ( !p_pmt )
        return false;

    pmt_each_desc( p_pmt, p_desc )
        if ( desc_get_tag( p_desc ) == 0x9 )
            return true;

    pmt_each_es( p_pmt, p_es )
        pmtn_each_desc( p_es, p_desc )
            if ( desc_get_tag( p_desc ) == 0x9 )
                return true;

    return false;
}

/*****************************************************************************
 * demux_ResendCAPMTs
 *****************************************************************************/
void demux_ResendCAPMTs( void )
{
    sid_list_each( &sids, p_sid )
        if ( SIDIsSelected( p_sid )
             && PMTNeedsDescrambling( p_sid->current_pmt ) )
            en50221_AddPMT( p_sid->current_pmt );
}

/*****************************************************************************
 * demux_Iconv
 *****************************************************************************
 * This code is from biTStream's examples and is under the WTFPL (see
 * LICENSE.WTFPL).
 *****************************************************************************/
static char *iconv_append_null(const char *p_string, size_t i_length)
{
    char *psz_string = malloc(i_length + 1);
    memcpy(psz_string, p_string, i_length);
    psz_string[i_length] = '\0';
    return psz_string;
}

char *demux_Iconv(void *_unused, const char *psz_encoding,
                  char *p_string, size_t i_length)
{
#ifdef HAVE_ICONV
    static const char *psz_current_encoding = "";

    char *psz_string, *p;
    size_t i_out_length;

    if (!strcmp(psz_encoding, psz_native_charset))
        return iconv_append_null(p_string, i_length);

    if (iconv_handle != (iconv_t)-1 &&
        strcmp(psz_encoding, psz_current_encoding)) {
        iconv_close(iconv_handle);
        iconv_handle = (iconv_t)-1;
    }

    if (iconv_handle == (iconv_t)-1)
        iconv_handle = iconv_open(psz_native_charset, psz_encoding);
    if (iconv_handle == (iconv_t)-1) {
        Warn( "couldn't open converter from %s to %s (%m)", psz_encoding,
              psz_native_charset);
        return iconv_append_null(p_string, i_length);
    }
    psz_current_encoding = psz_encoding;

    /* converted strings can be up to six times larger */
    i_out_length = i_length * 6;
    p = psz_string = malloc(i_out_length);
    if (iconv(iconv_handle, &p_string, &i_length, &p, &i_out_length) == (size_t)-1) {
        Warn( "couldn't convert from %s to %s (%m)", psz_encoding,
              psz_native_charset);
        free(psz_string);
        return iconv_append_null(p_string, i_length);
    }
    if (i_length)
        Warn( "partial conversion from %s to %s", psz_encoding,
              psz_native_charset);

    *p = '\0';
    return psz_string;
#else
    return iconv_append_null(p_string, i_length);
#endif
}

/*****************************************************************************
 * demux_Print
 *****************************************************************************
 * This code is from biTStream's examples and is under the WTFPL (see
 * LICENSE.WTFPL).
 *****************************************************************************/
__attribute__ ((format(printf, 2, 3)))
static void demux_Print(void *_unused, const char *psz_format, ...)
{
    char psz_fmt[strlen(psz_format) + 2];
    va_list args;
    va_start(args, psz_format);
    strcpy(psz_fmt, psz_format);
    if ( i_print_type != PRINT_XML )
        strcat(psz_fmt, "\n");
    vprintf(psz_fmt, args);
    va_end(args);
}

/*****************************************************************************
 * HandlePAT
 *****************************************************************************/
static void HandlePAT( mtime_t i_dts )
{
    PSI_TABLE_DECLARE( pp_old_pat_sections );
    uint8_t i_last_section = psi_table_get_lastsection( pp_next_pat_sections );
    uint8_t i;

    if ( psi_table_validate( pp_current_pat_sections ) &&
         psi_table_compare( pp_current_pat_sections, pp_next_pat_sections ) )
    {
        /* Identical PAT. Shortcut. */
        psi_table_free( pp_next_pat_sections );
        psi_table_init( pp_next_pat_sections );
        goto out_pat;
    }

    if ( !pat_table_validate( pp_next_pat_sections ) )
    {
        Warn( "invalid PAT received" );
        switch (i_print_type) {
        case PRINT_XML:
            fprintf(print_fh, "<ERROR type=\"invalid_pat\"/>\n");
            break;
        case PRINT_TEXT:
            fprintf(print_fh, "error type: invalid_pat\n");
            break;
        default:
            break;
        }
        psi_table_free( pp_next_pat_sections );
        psi_table_init( pp_next_pat_sections );
        goto out_pat;
    }

    /* Switch tables. */
    psi_table_copy( pp_old_pat_sections, pp_current_pat_sections );
    psi_table_copy( pp_current_pat_sections, pp_next_pat_sections );
    psi_table_init( pp_next_pat_sections );

    if ( !psi_table_validate( pp_old_pat_sections )
          || psi_table_get_tableidext( pp_current_pat_sections )
              != psi_table_get_tableidext( pp_old_pat_sections ) )
    {
        UpdateTSID();
        /* This will trigger a universal reset of everything. */
    }

    sid_list_t old_sids;
    sid_list_move( &sids, &old_sids );

    for ( i = 0; i <= i_last_section; i++ )
    {
        uint8_t *p_section =
            psi_table_get_section( pp_current_pat_sections, i );
        const uint8_t *p_program;
        int j = 0;

        while ( (p_program = pat_get_program( p_section, j )) != NULL )
        {
            uint16_t i_sid = patn_get_program( p_program );
            uint16_t i_pid = patn_get_pid( p_program );
            j++;

            if ( i_sid == 0 )
            {
                if ( i_pid != NIT_PID )
                    Warn( "NIT is carried on PID %hu which isn't DVB compliant",
                          i_pid );
                continue; /* NIT */
            }


            ts_sid_t *ts_sid = sid_link( &old_sids, i_sid, i_pid );
            if ( ts_sid )
                sid_list_add( &sids, ts_sid );
        }
    }
    sid_list_flush( &old_sids, ts_sid )
    {
        UpdatePAT( ts_sid->id );
        sid_free( ts_sid );
    }

    sid_list_each( &sids, ts_sid )
        if ( ts_sid->changed )
            UpdatePAT( ts_sid->id );

    pat_table_print( pp_current_pat_sections, msg_Dbg, NULL, PRINT_TEXT );
    if ( b_print_enabled )
    {
        pat_table_print( pp_current_pat_sections, demux_Print, NULL,
                         i_print_type );
        if ( i_print_type == PRINT_XML )
            fprintf(print_fh, "\n");
    }

    output_list_each( &outputs, output )
    {
        output_sid_list_t old_sids;
        output_sid_list_move( &output->sids, &old_sids );
        bool b_sid_change = false;
        bool b_service_name_change = false;

        config_sid_list_each( &output->config.sids, conf )
        {
            ts_sid_t *sid = NULL;
            do {
                sid = sid_find_from_config( &sids, &output->sids, conf );
                if ( !sid ) continue;

                sid_out_t *l = sid_out_link( &old_sids, output, sid );
                if ( !l ) continue;
                if ( l->conf ) {
                    if ( dvb_string_cmp( &l->conf->new_name, &conf->new_name ) ||
                         dvb_string_cmp( &l->conf->new_provider, &conf->new_provider ) )
                        b_service_name_change = true;
                }
                l->conf = conf;
                sid_out_remap( l, conf->new_sid, conf->pmt_pid );
                output_sid_list_add( &output->sids, l );
                b_sid_change = b_sid_change || l->changed;
            } while ( conf->type == OUTPUT_CONFIG_TYPE_ALL && sid );
        }

        b_sid_change = b_sid_change || !output_sid_list_is_empty( &old_sids );
        output_sid_list_clean( &old_sids );

        if ( b_sid_change ) {
            NewSDT( output );
            NewNIT( output );
            NewPAT( output );
            output_sid_list_each( &output->sids, sid_out )
                NewPMT( sid_out );
        } else if ( b_service_name_change ) {
            NewSDT( output );
        }
    }

out_pat:
    SendPAT( i_dts );
}

/*****************************************************************************
 * HandlePATSection
 *****************************************************************************/
static void HandlePATSection( uint16_t i_pid, uint8_t *p_section,
                              mtime_t i_dts )
{
    if ( i_pid != PAT_PID || !pat_validate( p_section ) )
    {
        Warn( "invalid PAT section received on PID %hu", i_pid );
        switch (i_print_type) {
        case PRINT_XML:
            fprintf(print_fh, "<ERROR type=\"invalid_pat_section\"/>\n");
            break;
        case PRINT_TEXT:
            fprintf(print_fh, "error type: invalid_pat_section\n");
            break;
        default:
            break;
        }
        free( p_section );
        return;
    }

    if ( !psi_table_section( pp_next_pat_sections, p_section ) )
        return;

    HandlePAT( i_dts );
}

/*****************************************************************************
 * HandleCAT
 *****************************************************************************/
static void HandleCAT( mtime_t i_dts )
{
    uint8_t i_last_section = psi_table_get_lastsection( pp_next_cat_sections );
    uint8_t i;

    if ( psi_table_validate( pp_current_cat_sections ) &&
         psi_table_compare( pp_current_cat_sections, pp_next_cat_sections ) )
    {
        /* Identical CAT. Shortcut. */
        psi_table_free( pp_next_cat_sections );
        psi_table_init( pp_next_cat_sections );
        goto out_cat;
    }

    if ( !cat_table_validate( pp_next_cat_sections ) )
    {
        Warn( "invalid CAT received" );
        switch (i_print_type) {
        case PRINT_XML:
            fprintf(print_fh, "<ERROR type=\"invalid_cat\"/>\n");
            break;
        case PRINT_TEXT:
            fprintf(print_fh, "error type: invalid_cat\n");
            break;
        default:
            break;
        }
        psi_table_free( pp_next_cat_sections );
        psi_table_init( pp_next_cat_sections );
        goto out_cat;
    }

    /* Switch tables. */
    psi_table_free( pp_current_cat_sections );
    psi_table_copy( pp_current_cat_sections, pp_next_cat_sections );
    psi_table_init( pp_next_cat_sections );

    pid_list_t old_emm_list;
    pid_list_move( &emm_list, &old_emm_list );
    for ( i = 0; i <= i_last_section; i++ )
    {
        uint8_t *p_section = psi_table_get_section( pp_current_cat_sections, i );

        descl_each_desc( cat_get_descl( p_section ), cat_get_desclength( p_section ), p_desc )
            if ( DESC_CHECK( 09,  p_desc ) )
            {
                pid_refcount_t *emm =
                    pid_use( &old_emm_list, desc09_get_pid( p_desc ),
                             NULL, ES_TYPE_EMM );
                pid_list_add( &emm_list, emm );
            }
    }
    pid_list_clean( &old_emm_list );

    cat_table_print( pp_current_cat_sections, msg_Dbg, NULL, PRINT_TEXT );
    if ( b_print_enabled )
    {
        cat_table_print( pp_current_cat_sections, demux_Print, NULL,
                         i_print_type );
        if ( i_print_type == PRINT_XML )
            fprintf(print_fh, "\n");
    }

out_cat:
    return;
}

/*****************************************************************************
 * HandleCATSection
 *****************************************************************************/
static void HandleCATSection( uint16_t i_pid, uint8_t *p_section,
                              mtime_t i_dts )
{
    if ( i_pid != CAT_PID || !cat_validate( p_section ) )
    {
        Warn( "invalid CAT section received on PID %hu", i_pid );
        switch (i_print_type) {
        case PRINT_XML:
            fprintf(print_fh, "<ERROR type=\"invalid_cat_section\"/>\n");
            break;
        case PRINT_TEXT:
            fprintf(print_fh, "error type: invalid_cat_section\n");
            break;
        default:
            break;
        }
        free( p_section );
        return;
    }

    if ( !psi_table_section( pp_next_cat_sections, p_section ) )
        return;

    HandleCAT( i_dts );
}

/*****************************************************************************
 * HandlePMT
 *****************************************************************************/
static void HandlePMT( uint16_t i_pid, uint8_t *p_pmt, mtime_t i_dts )
{
    uint16_t i_sid = pmt_get_program( p_pmt );
    uint16_t pcr_pid = pmt_get_pcrpid( p_pmt );
    bool b_needs_descrambling, b_needed_descrambling, b_is_selected;

    ts_sid_t *sid = sid_find( &sids, i_sid );
    if ( sid == NULL )
    {
        /* Unwanted SID (happens when the same PMT PID is used for several
         * programs). */
        free( p_pmt );
        return;
    }

    if ( i_pid != sid->pmt_pid )
    {
        Warn( "invalid PMT section received on PID %hu", i_pid );
        switch (i_print_type) {
        case PRINT_XML:
            fprintf(print_fh, "<ERROR type=\"ghost_pmt\" program=\"%hu\n pid=\"%hu\"/>\n",
                    i_sid, i_pid);
            break;
        case PRINT_TEXT:
            fprintf(print_fh, "error type: ghost_pmt program: %hu pid: %hu\n",
                    i_sid, i_pid);
            break;
        default:
            break;
        }
        free( p_pmt );
        return;
    }

    if ( sid->current_pmt != NULL &&
         psi_compare( sid->current_pmt, p_pmt ) )
    {
        /* Identical PMT. Shortcut. */
        free( p_pmt );
        goto out_pmt;
    }

    if ( !pmt_validate( p_pmt ) )
    {
        Warn( "invalid PMT section received on PID %hu", i_pid );
        switch (i_print_type) {
        case PRINT_XML:
            fprintf(print_fh, "<ERROR type=\"invalid_pmt_section\" pid=\"%hu\"/>\n",
                    i_pid);
            break;
        case PRINT_TEXT:
            fprintf(print_fh, "error type: invalid_pmt_section pid: %hu\n",
                    i_pid);
            break;
        default:
            break;
        }
        free( p_pmt );
        goto out_pmt;
    }

    b_needs_descrambling = PMTNeedsDescrambling( p_pmt );
    b_needed_descrambling = PMTNeedsDescrambling( sid->current_pmt );
    b_is_selected = SIDIsSelected( sid );

    if ( i_ca_handle && b_is_selected &&
         !b_needs_descrambling && b_needed_descrambling )
        en50221_DeletePMT( sid->current_pmt );

    if ( sid->current_pmt != NULL )
        free( sid->current_pmt );
    sid->current_pmt = p_pmt;
    sid->pcr_pid = pcr_pid;

    pid_list_t old_pids;
    pid_list_move( &sid->pids, &old_pids );

    pmt_each_es( p_pmt, es ) {
        uint16_t pid = pmtn_get_pid( es );
        es_type_t type = es_type_or_private( es );

        pid_refcount_t *ref = pid_use( &old_pids, pid, sid, type );
        if ( ref )
            pid_list_add( &sid->pids, ref );

        memset( &ref->desc, 0, sizeof(ref->desc));
        pid_list_t old_ecms;
        pid_list_move( &ref->ecms, &old_ecms );
        pmtn_each_desc( es, desc ) {
            if ( DESC_CHECK( 0a, desc ) ) {
                desc0a_each_language( desc, desc_n) {
                    const uint8_t *code = desc0an_get_code( desc_n );
                    snprintf( ref->desc.audio.lang,
                              sizeof(ref->desc.audio.lang),
                              "%3.3s", code );
                }
            } else if ( DESC_CHECK( 59, desc ) ) {
                desc59_each_language( desc, desc_n) {
                    const uint8_t *code = desc59n_get_code( desc_n );
                    snprintf( ref->desc.subtitle.lang,
                              sizeof(ref->desc.subtitle.lang),
                              "%3.3s", code );
                }
            } else if ( DESC_CHECK( 09, desc ) ) {
                uint16_t ecm_pid = desc09_get_pid( desc );
                pid_refcount_t *ecm =
                    pid_use( &old_ecms, ecm_pid, sid, ES_TYPE_ECM );
                pid_list_add( &ref->ecms, ecm );
            }
        }
        pid_list_clean( &old_ecms );
    }

    pid_list_t old_ecms;
    pid_list_move( &sid->ecms, &old_ecms );
    pmt_each_desc( p_pmt, desc ) {
        if ( !DESC_CHECK( 09,  desc ) )
            continue;

        uint16_t ecm_pid = desc09_get_pid( desc );
        pid_refcount_t *ecm = pid_use( &old_ecms, ecm_pid, sid, ES_TYPE_ECM );
        pid_list_add( &sid->ecms, ecm );
    }
    pid_list_clean( &old_ecms );

    pid_list_clean( &old_pids );

    sid_out_list_each( &sid->outputs, sid_out )
        GetPIDS( sid_out );

    if ( i_ca_handle && b_is_selected )
    {
        if ( b_needs_descrambling && !b_needed_descrambling )
            en50221_AddPMT( p_pmt );
        else if ( b_needs_descrambling && b_needed_descrambling )
            en50221_UpdatePMT( p_pmt );
    }

    UpdatePMT( i_sid );

    pmt_print( p_pmt, msg_Dbg, NULL, demux_Iconv, NULL, PRINT_TEXT );
    if ( b_print_enabled )
    {
        pmt_print( p_pmt, demux_Print, NULL, demux_Iconv, NULL,
                   i_print_type );
        if ( i_print_type == PRINT_XML )
            fprintf(print_fh, "\n");
    }

out_pmt:
    SendPMT( sid, i_dts );
}

/*****************************************************************************
 * HandleNIT
 *****************************************************************************/
static void HandleNIT( mtime_t i_dts )
{
    if ( psi_table_validate( pp_current_nit_sections ) &&
         psi_table_compare( pp_current_nit_sections, pp_next_nit_sections ) )
    {
        /* Identical NIT. Shortcut. */
        psi_table_free( pp_next_nit_sections );
        psi_table_init( pp_next_nit_sections );
        goto out_nit;
    }

    if ( !nit_table_validate( pp_next_nit_sections ) )
    {
        Warn( "invalid NIT received" );
        switch (i_print_type) {
        case PRINT_XML:
            fprintf(print_fh, "<ERROR type=\"invalid_nit\"/>\n");
            break;
        case PRINT_TEXT:
            fprintf(print_fh, "error type: invalid_nit\n");
            break;
        default:
            break;
        }
        psi_table_free( pp_next_nit_sections );
        psi_table_init( pp_next_nit_sections );
        goto out_nit;
    }

    /* Switch tables. */
    psi_table_free( pp_current_nit_sections );
    psi_table_copy( pp_current_nit_sections, pp_next_nit_sections );
    psi_table_init( pp_next_nit_sections );

    nit_table_print( pp_current_nit_sections, msg_Dbg, NULL,
                     demux_Iconv, NULL, PRINT_TEXT );
    if ( b_print_enabled )
    {
        nit_table_print( pp_current_nit_sections, demux_Print, NULL,
                         demux_Iconv, NULL, i_print_type );
        if ( i_print_type == PRINT_XML )
            fprintf(print_fh, "\n");
    }

out_nit:
    ;
}

/*****************************************************************************
 * HandleNITSection
 *****************************************************************************/
static void HandleNITSection( uint16_t i_pid, uint8_t *p_section,
                              mtime_t i_dts )
{
    if ( i_pid != NIT_PID || !nit_validate( p_section ) )
    {
        Warn( "invalid NIT section received on PID %hu", i_pid );
        switch (i_print_type) {
        case PRINT_XML:
            fprintf(print_fh, "<ERROR type=\"invalid_nit_section\" pid=\"%hu\"/>\n",
                    i_pid);
            break;
        case PRINT_TEXT:
            fprintf(print_fh, "error type: invalid_nit_section pid: %hu\n",
                    i_pid);
            break;
        default:
            break;
        }
        free( p_section );
        return;
    }

    if ( psi_table_section( pp_next_nit_sections, p_section ) )
        HandleNIT( i_dts );

    /* This case is different because DVB specifies a minimum bitrate for
     * PID 0x10, even if we don't have any thing to send (for cheap
     * transport over network boundaries). */
    SendNIT( i_dts );
}


/*****************************************************************************
 * HandleSDT
 *****************************************************************************/
static void HandleSDT( mtime_t i_dts )
{
    uint8_t i_last_section = psi_table_get_lastsection( pp_next_sdt_sections );

    if ( psi_table_validate( pp_current_sdt_sections ) &&
         psi_table_compare( pp_current_sdt_sections, pp_next_sdt_sections ) )
    {
        /* Identical SDT. Shortcut. */
        psi_table_free( pp_next_sdt_sections );
        psi_table_init( pp_next_sdt_sections );
        goto out_sdt;
    }

    if ( !sdt_table_validate( pp_next_sdt_sections ) )
    {
        Warn( "invalid SDT received" );
        switch (i_print_type) {
        case PRINT_XML:
            fprintf(print_fh, "<ERROR type=\"invalid_sdt\"/>\n");
            break;
        case PRINT_TEXT:
            fprintf(print_fh, "error type: invalid_sdt\n");
            break;
        default:
            break;
        }
        psi_table_free( pp_next_sdt_sections );
        psi_table_init( pp_next_sdt_sections );
        goto out_sdt;
    }

    /* Switch tables. */
    psi_table_free( pp_current_sdt_sections );
    psi_table_copy( pp_current_sdt_sections, pp_next_sdt_sections );
    psi_table_init( pp_next_sdt_sections );

    sdt_entries_t old_services;
    sdt_entries_move( &sdt_entries, &old_services );

    for ( uint8_t i = 0; i <= i_last_section; i++ )
    {
        uint8_t *p_section =
            psi_table_get_section( pp_current_sdt_sections, i );
        uint8_t *p_service;

        for (int j = 0; (p_service = sdt_get_service( p_section, j )); j++ )
        {
            uint16_t i_sid = sdtn_get_sid( p_service );
            sdt_entry_t *ref = sdt_entry_new( &old_services, i_sid );
            if ( !ref )
                continue;

            free( ref->provider );
            free( ref->name );
            ref->provider = NULL;
            ref->name = NULL;

            descs_each_desc( sdtn_get_descs( p_service ), desc ) {
                if ( DESC_CHECK( 48, desc ) ) {
                    uint8_t provider_length;
                    uint8_t service_length;
                    const uint8_t *provider =
                        desc48_get_provider(desc, &provider_length);
                    const uint8_t *service =
                        desc48_get_service(desc, &service_length);

                    free( ref->provider );
                    free( ref->name );
                    ref->provider = dvb_string_get(provider, provider_length,
                                                   demux_Iconv, NULL);
                    ref->name = dvb_string_get(service, service_length,
                                               demux_Iconv, NULL);
                }
            }

            sdt_entries_add( &sdt_entries, ref );
        }
    }

    sdt_entries_each( &old_services, s )
        UpdateSDT( s->sid->id );
    sdt_entries_clean( &old_services );

    sdt_entries_each( &sdt_entries, s )
        UpdateSDT( s->sid->id );

    sdt_table_print( pp_current_sdt_sections, msg_Dbg, NULL,
                     demux_Iconv, NULL, PRINT_TEXT );
    if ( b_print_enabled )
    {
        sdt_table_print( pp_current_sdt_sections, demux_Print, NULL,
                         demux_Iconv, NULL, i_print_type );
        if ( i_print_type == PRINT_XML )
            fprintf(print_fh, "\n");
    }

out_sdt:
    SendSDT( i_dts );
}

/*****************************************************************************
 * HandleSDTSection
 *****************************************************************************/
static void HandleSDTSection( uint16_t i_pid, uint8_t *p_section,
                              mtime_t i_dts )
{
    if ( i_pid != SDT_PID || !sdt_validate( p_section ) )
    {
        Warn( "invalid SDT section received on PID %hu", i_pid );
        switch (i_print_type) {
        case PRINT_XML:
            fprintf(print_fh, "<ERROR type=\"invalid_sdt_section\" pid=\"%hu\"/>\n",
                    i_pid);
            break;
        case PRINT_TEXT:
            fprintf(print_fh, "error type: invalid_sdt_section pid: %hu\n",
                    i_pid);
            break;
        default:
            break;
        }
        free( p_section );
        return;
    }

    if ( !psi_table_section( pp_next_sdt_sections, p_section ) )
        return;

    HandleSDT( i_dts );
}

/*****************************************************************************
 * HandleEITSection
 *****************************************************************************/
static void HandleEIT( uint16_t i_pid, uint8_t *p_eit, mtime_t i_dts )
{
    uint8_t i_table_id = psi_get_tableid( p_eit );
    uint16_t i_sid = eit_get_sid( p_eit );
    ts_sid_t *p_sid;

    p_sid = sid_find( &sids, i_sid );
    if ( p_sid == NULL )
    {
        /* Not a selected program. */
        free( p_eit );
        return;
    }

    if ( i_pid != EIT_PID || !eit_validate( p_eit ) )
    {
        Warn( "invalid EIT section received on PID %hu", i_pid );
        switch (i_print_type) {
        case PRINT_XML:
            fprintf(print_fh, "<ERROR type=\"invalid_eit_section\" pid=\"%hu\"/>\n",
                    i_pid);
            break;
        case PRINT_TEXT:
            fprintf(print_fh, "error type: invalid_eit_section pid: %hu\n",
                    i_pid);
            break;
        default:
            break;
        }
        free( p_eit );
        return;
    }

    /* We do not use psi_table_* primitives as the spec allows for holes in
     * section numbering, and there is no sure way to know whether you have
     * gathered all sections. */
    uint8_t i_section = psi_get_section(p_eit);
    uint8_t eit_table_id = i_table_id - EIT_TABLE_ID_PF_ACTUAL;
    if (eit_table_id >= MAX_EIT_TABLES)
        goto out_eit; /* can't happen */
    if (p_sid->eit_table[eit_table_id].data[i_section] != NULL &&
        psi_compare(p_sid->eit_table[eit_table_id].data[i_section], p_eit)) {
        /* Identical section. Shortcut. */
        free(p_sid->eit_table[eit_table_id].data[i_section]);
        p_sid->eit_table[eit_table_id].data[i_section] = p_eit;
        goto out_eit;
    }

    free(p_sid->eit_table[eit_table_id].data[i_section]);
    p_sid->eit_table[eit_table_id].data[i_section] = p_eit;

    if ( b_print_enabled && psi_get_tableid( p_eit ) == EIT_TABLE_ID_PF_ACTUAL )
    {
        eit_print( p_eit, demux_Print, NULL,
                   demux_Iconv, NULL, i_print_type );
        if ( i_print_type == PRINT_XML )
            fprintf(print_fh, "\n");
    }

out_eit:
    SendEIT( p_sid, i_dts, p_eit );
}

/*****************************************************************************
 * HandleSection
 *****************************************************************************/
static void HandleSection( uint16_t i_pid, uint8_t *p_section, mtime_t i_dts )
{
    uint8_t i_table_id = psi_get_tableid( p_section );

    if ( !psi_validate( p_section ) )
    {
        Warn( "invalid section on PID %hu", i_pid );
        switch (i_print_type) {
        case PRINT_XML:
            fprintf(print_fh, "<ERROR type=\"invalid_section\" pid=\"%hu\"/>\n", i_pid);
            break;
        case PRINT_TEXT:
            fprintf(print_fh, "error type: invalid_section pid: %hu\n", i_pid);
            break;
        default:
            break;
        }
        free( p_section );
        return;
    }

    if ( !psi_get_current( p_section ) )
    {
        /* Ignore sections which are not in use yet. */
        free( p_section );
        return;
    }

    switch ( i_table_id )
    {
    case PAT_TABLE_ID:
        HandlePATSection( i_pid, p_section, i_dts );
        break;

    case CAT_TABLE_ID:
        if ( b_enable_emm )
            HandleCATSection( i_pid, p_section, i_dts );
        break;

    case PMT_TABLE_ID:
        HandlePMT( i_pid, p_section, i_dts );
        break;

    case NIT_TABLE_ID_ACTUAL:
        HandleNITSection( i_pid, p_section, i_dts );
        break;

    case SDT_TABLE_ID_ACTUAL:
        HandleSDTSection( i_pid, p_section, i_dts );
        break;

    default:
        if ( IsEITpf( i_table_id ) || IsEPG( i_table_id ) )
        {
            HandleEIT( i_pid, p_section, i_dts );
            break;
        }
        free( p_section );
        break;
    }
}

/*****************************************************************************
 * HandlePSIPacket
 *****************************************************************************/
static void HandlePSIPacket( uint8_t *p_ts, mtime_t i_dts )
{
    uint16_t i_pid = ts_get_pid( p_ts );
    ts_pid_t *p_pid = &p_pids[i_pid];
    uint8_t i_cc = ts_get_cc( p_ts );
    const uint8_t *p_payload;
    uint8_t i_length;

    if ( ts_check_duplicate( i_cc, p_pid->i_last_cc )
          || !ts_has_payload( p_ts ) )
        return;

    if ( p_pid->i_last_cc != -1
          && ts_check_discontinuity( i_cc, p_pid->i_last_cc ) )
        psi_assemble_reset( &p_pid->p_psi_buffer, &p_pid->i_psi_buffer_used );

    p_payload = ts_section( p_ts );
    i_length = p_ts + TS_SIZE - p_payload;

    if ( !psi_assemble_empty( &p_pid->p_psi_buffer,
                              &p_pid->i_psi_buffer_used ) )
    {
        uint8_t *p_section = psi_assemble_payload( &p_pid->p_psi_buffer,
                                                   &p_pid->i_psi_buffer_used,
                                                   &p_payload, &i_length );
        if ( p_section != NULL )
            HandleSection( i_pid, p_section, i_dts );
    }

    p_payload = ts_next_section( p_ts );
    i_length = p_ts + TS_SIZE - p_payload;

    while ( i_length )
    {
        uint8_t *p_section = psi_assemble_payload( &p_pid->p_psi_buffer,
                                                   &p_pid->i_psi_buffer_used,
                                                   &p_payload, &i_length );
        if ( p_section != NULL )
            HandleSection( i_pid, p_section, i_dts );
    }
}

/*****************************************************************************
 * PID info functions
 *****************************************************************************/
static const char *h222_stream_type_desc(uint8_t i_stream_type) {
    /* See ISO/IEC 13818-1 : 2000 (E) | Table 2-29 - Stream type assignments, Page 66 (48) */
    if (i_stream_type == 0)
        return "Reserved stream";
    switch (i_stream_type) {
        case 0x01: return "11172-2 video (MPEG-1)";
        case 0x02: return "H.262/13818-2 video (MPEG-2) or 11172-2 constrained video";
        case 0x03: return "11172-3 audio (MPEG-1)";
        case 0x04: return "13818-3 audio (MPEG-2)";
        case 0x05: return "H.222.0/13818-1  private sections";
        case 0x06: return "H.222.0/13818-1 PES private data";
        case 0x07: return "13522 MHEG";
        case 0x08: return "H.222.0/13818-1 Annex A - DSM CC";
        case 0x09: return "H.222.1";
        case 0x0A: return "13818-6 type A";
        case 0x0B: return "13818-6 type B";
        case 0x0C: return "13818-6 type C";
        case 0x0D: return "13818-6 type D";
        case 0x0E: return "H.222.0/13818-1 auxiliary";
        case 0x0F: return "13818-7 Audio with ADTS transport syntax";
        case 0x10: return "14496-2 Visual (MPEG-4 part 2 video)";
        case 0x11: return "14496-3 Audio with LATM transport syntax (14496-3/AMD 1)";
        case 0x12: return "14496-1 SL-packetized or FlexMux stream in PES packets";
        case 0x13: return "14496-1 SL-packetized or FlexMux stream in 14496 sections";
        case 0x14: return "ISO/IEC 13818-6 Synchronized Download Protocol";
        case 0x15: return "Metadata in PES packets";
        case 0x16: return "Metadata in metadata_sections";
        case 0x17: return "Metadata in 13818-6 Data Carousel";
        case 0x18: return "Metadata in 13818-6 Object Carousel";
        case 0x19: return "Metadata in 13818-6 Synchronized Download Protocol";
        case 0x1A: return "13818-11 MPEG-2 IPMP stream";
        case 0x1B: return "H.264/14496-10 video (MPEG-4/AVC)";
        case 0x24: return "H.265/23008-2 video (HEVC)";
        case 0x42: return "AVS Video";
        case 0x7F: return "IPMP stream";
        default  : return "Unknown stream";
    }
}

static const char *get_pid_desc(uint16_t i_pid, uint16_t *i_sid) {
    int i, j;
    uint8_t i_last_section;
    uint16_t i_nit_pid = NIT_PID, i_pcr_pid = 0;

    /* Simple cases */
    switch (i_pid)
    {
        case 0x00: return "PAT";
        case 0x01: return "CAT";
        case 0x11: return "SDT";
        case 0x12: return "EPG";
        case 0x14: return "TDT/TOT";
    }

    /* Detect NIT pid */
    if ( psi_table_validate( pp_current_pat_sections ) )
    {
        i_last_section = psi_table_get_lastsection( pp_current_pat_sections );
        for ( i = 0; i <= i_last_section; i++ )
        {
            uint8_t *p_section = psi_table_get_section( pp_current_pat_sections, i );
            uint8_t *p_program;

            j = 0;
            while ( (p_program = pat_get_program( p_section, j++ )) != NULL )
            {
                /* Programs with PID == 0 are actually NIT */
                if ( patn_get_program( p_program ) == 0 )
                {
                    i_nit_pid = patn_get_pid( p_program );
                    break;
                }
            }
        }
    }

    /* Detect EMM pids */
    if ( b_enable_emm && psi_table_validate( pp_current_cat_sections ) )
    {
        i_last_section = psi_table_get_lastsection( pp_current_cat_sections );
        for ( i = 0; i <= i_last_section; i++ )
        {
            uint8_t *p_section = psi_table_get_section( pp_current_cat_sections, i );

            descl_each_desc( cat_get_descl( p_section ), cat_get_desclength( p_section ), p_desc )
                if ( DESC_CHECK( 09,  p_desc ) && desc09_get_pid( p_desc ) == i_pid )
                    return "EMM";
        }
    }

    /* Detect streams in PMT */
    sid_list_each( &sids, p_sid )
    {
        if ( p_sid->pmt_pid == i_pid )
        {
            if ( i_sid )
                *i_sid = p_sid->id;
            return "PMT";
        }

        if ( p_sid->id && p_sid->current_pmt != NULL )
        {
            uint8_t *p_current_pmt = p_sid->current_pmt;
            uint8_t *p_current_es;

            /* The PCR PID can be alone or PCR can be carried in some other PIDs (mostly video)
               so just remember the pid and if it is alone it will be reported as PCR, otherwise
               stream type of the PID will be reported */
            if ( i_pid == pmt_get_pcrpid( p_current_pmt ) ) {
                if ( i_sid )
                    *i_sid = p_sid->id;
                i_pcr_pid = pmt_get_pcrpid( p_current_pmt );
            }

            /* Look for ECMs */
            pmt_each_desc( p_current_pmt, p_desc )
                if ( DESC_CHECK( 09,  p_desc ) && desc09_get_pid( p_desc ) == i_pid )
                {
                    if ( i_sid )
                        *i_sid = p_sid->id;
                    return "ECM";
                }

            /* Detect stream types */
            j = 0;
            while ( (p_current_es = pmt_get_es( p_current_pmt, j++ )) != NULL )
            {
                if ( pmtn_get_pid( p_current_es ) == i_pid )
                {
                    if ( i_sid )
                        *i_sid = p_sid->id;
                    return h222_stream_type_desc( pmtn_get_streamtype( p_current_es ) );
                }
            }
        }
    }

    /* Are there any other PIDs? */
    if (i_pid == i_nit_pid)
        return "NIT";

    if (i_pid == i_pcr_pid)
        return "PCR";

    return "...";
}

/*****************************************************************************
 * Functions that return packed sections
 *****************************************************************************/
uint8_t *demux_get_current_packed_PAT( unsigned int *pi_pack_size ) {
    return psi_pack_sections( pp_current_pat_sections, pi_pack_size );
}

uint8_t *demux_get_current_packed_CAT( unsigned int *pi_pack_size ) {
    return psi_pack_sections( pp_current_cat_sections, pi_pack_size );
}

uint8_t *demux_get_current_packed_NIT( unsigned int *pi_pack_size ) {
    return psi_pack_sections( pp_current_nit_sections, pi_pack_size );
}

uint8_t *demux_get_current_packed_SDT( unsigned int *pi_pack_size ) {
    return psi_pack_sections( pp_current_sdt_sections, pi_pack_size );
}

uint8_t *demux_get_packed_EIT( uint16_t i_sid, uint8_t start_table, uint8_t end_table, unsigned int *eit_size ) {
    unsigned int i, r;

    *eit_size = 0;
    ts_sid_t *p_sid = sid_find( &sids, i_sid );
    if ( p_sid == NULL )
        return NULL;

    /* Calculate eit table size (sum of all sections in all tables between start_start and end_table) */
    for ( i = start_table; i <= end_table; i++ ) {
        uint8_t eit_table_idx = i - EIT_TABLE_ID_PF_ACTUAL;
        if ( eit_table_idx >= MAX_EIT_TABLES )
            continue;
        uint8_t **eit_sections = p_sid->eit_table[eit_table_idx].data;
        for ( r = 0; r < PSI_TABLE_MAX_SECTIONS; r++ ) {
            uint8_t *p_eit = eit_sections[r];
            if ( !p_eit )
                continue;
            uint16_t psi_length = psi_get_length( p_eit ) + PSI_HEADER_SIZE;
            *eit_size += psi_length;
        }
    }

    uint8_t *p_flat_section = malloc( *eit_size );
    if ( !p_flat_section )
        return NULL;

    /* Copy sections */
    unsigned int i_pos = 0;
    for ( i = start_table; i <= end_table; i++ ) {
        uint8_t eit_table_idx = i - EIT_TABLE_ID_PF_ACTUAL;
        if ( eit_table_idx >= MAX_EIT_TABLES )
            continue;
        uint8_t **eit_sections = p_sid->eit_table[eit_table_idx].data;
        for ( r = 0; r < PSI_TABLE_MAX_SECTIONS; r++ ) {
            uint8_t *p_eit = eit_sections[r];
            if ( !p_eit )
                continue;
            uint16_t psi_length = psi_get_length( p_eit ) + PSI_HEADER_SIZE;
            memcpy( p_flat_section + i_pos, p_eit, psi_length );
            i_pos += psi_length;
            /* eit_print( p_eit, msg_Dbg, NULL, demux_Iconv, NULL, PRINT_TEXT ); */
        }
    }
    return p_flat_section;
}

uint8_t *demux_get_packed_EIT_pf( uint16_t service_id, unsigned int *pi_pack_size ) {
    return demux_get_packed_EIT( service_id, EIT_TABLE_ID_PF_ACTUAL, EIT_TABLE_ID_PF_ACTUAL, pi_pack_size );
}

uint8_t *demux_get_packed_EIT_schedule( uint16_t service_id, unsigned int *pi_pack_size ) {
    return demux_get_packed_EIT( service_id, EIT_TABLE_ID_SCHED_ACTUAL_FIRST, EIT_TABLE_ID_SCHED_ACTUAL_LAST, pi_pack_size );
}

uint8_t *demux_get_packed_PMT( uint16_t i_sid, unsigned int *pi_pack_size ) {
    ts_sid_t *p_sid = sid_find( &sids, i_sid );
    if ( p_sid != NULL && p_sid->current_pmt && pmt_validate( p_sid->current_pmt ) )
        return psi_pack_section( p_sid->current_pmt, pi_pack_size );
    return NULL;
}

inline void demux_get_PID_info( uint16_t i_pid, uint8_t *p_data ) {
    ts_pid_info_t *p_info = (ts_pid_info_t *)p_data;
    *p_info = p_pids[i_pid].info;
}

inline void demux_get_PIDS_info( uint8_t *p_data ) {
    int i_pid;
    for (i_pid = 0; i_pid < MAX_PIDS; i_pid++ )
        demux_get_PID_info( i_pid, p_data + ( i_pid * sizeof(ts_pid_info_t) ) );
}
