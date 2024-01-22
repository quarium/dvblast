/*****************************************************************************
 * config.c
 *****************************************************************************
 * Copyright (C) 2004, 2008-2010, 2015-2016 VideoLAN
 * Copyright (C) 2022 EasyTools
 *
 * Authors: Christophe Massiot <massiot@via.ecp.fr>
 *          Andy Gatward <a.j.gatward@reading.ac.uk>
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

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ctype.h>

#include <bitstream/ietf/rtp.h>

#include "config.h"
#include "dvblast.h"

#ifdef HAVE_ICONV
#include <iconv.h>
#endif

#define DEFINE_CONFIG_GLOBAL(Type, Name, ...)                           \
static Type Name##_global = __VA_ARGS__;                                \
                                                                        \
void config_set_##Name( Type value )                                    \
{                                                                       \
    Name##_global = value;                                              \
}                                                                       \
                                                                        \
Type config_get_##Name( void )                                          \
{                                                                       \
    return Name##_global;                                               \
}

DEFINE_CONFIG_GLOBAL(const char *, conf_file, NULL);
DEFINE_CONFIG_GLOBAL(bool, udp, false);
DEFINE_CONFIG_GLOBAL(bool, dvb, false);
DEFINE_CONFIG_GLOBAL(bool, epg, false);
DEFINE_CONFIG_GLOBAL(mtime_t, latency, DEFAULT_OUTPUT_LATENCY);
DEFINE_CONFIG_GLOBAL(mtime_t, retention, DEFAULT_MAX_RETENTION);
DEFINE_CONFIG_GLOBAL(int, ttl, 64);
DEFINE_CONFIG_GLOBAL(in_addr_t, ssrc, 0);
DEFINE_CONFIG_GLOBAL(uint16_t, network_id, DEFAULT_NETWORK_ID);
DEFINE_CONFIG_GLOBAL(const char *, dvb_charset, DEFAULT_DVB_CHARSET);
DEFINE_CONFIG_GLOBAL(bool, passthrough, false);
static const char *network_name_global = DEFAULT_NETWORK_NAME;
static const char *provider_name_global = NULL;

static dvb_string_t network_name;
static dvb_string_t provider_name;
static iconv_t conf_iconv = (iconv_t)-1;

char *config_stropt( const char *psz_string )
{
    char *ret, *tmp;
    if ( !psz_string || strlen( psz_string ) == 0 )
        return NULL;
    ret = tmp = strdup( psz_string );
    while (*tmp) {
        if (*tmp == '_')
            *tmp = ' ';
        if (*tmp == '/') {
            *tmp = '\0';
            break;
        }
        tmp++;
    }
    return ret;
}

static uint8_t *config_striconv( const char *psz_string,
                                 const char *psz_charset, size_t *pi_length )
{
    char *psz_input = config_stropt(psz_string);
    *pi_length = strlen(psz_input);

    /* do not convert ASCII strings */
    const char *c = psz_string;
    while (*c)
        if (!isascii(*c++))
            break;
    if (!*c)
        return (uint8_t *)psz_input;

    if ( !strcasecmp( psz_native_charset, psz_charset ) )
        return (uint8_t *)psz_input;

#ifdef HAVE_ICONV
    if ( conf_iconv == (iconv_t)-1 )
    {
        conf_iconv = iconv_open( psz_charset, psz_native_charset );
        if ( conf_iconv == (iconv_t)-1 )
            return (uint8_t *)psz_input;
    }

    char *psz_tmp = psz_input;
    size_t i_input = *pi_length;
    size_t i_output = i_input * 6;
    size_t i_available = i_output;
    char *p_output = malloc( i_output );
    char *p = p_output;
    if ( iconv( conf_iconv, &psz_tmp, &i_input, &p, &i_available ) == (size_t)-1 )
    {
        free( p_output );

        return (uint8_t *)psz_input;
    }

    free(psz_input);
    *pi_length = i_output - i_available;
    return (uint8_t *)p_output;
#else
    Warn( "unable to convert from %s to %s (iconv is not available)",
          psz_native_charset, psz_charset );
    return (uint8_t *)psz_input;
#endif
}

void config_strdvb( dvb_string_t *dvb_string,
                    const char *str, const char *charset )
{
    if ( charset == NULL )
        charset = dvb_charset_global;

    if (str == NULL)
    {
        dvb_string_init(dvb_string);
        return;
    }
    dvb_string_clean(dvb_string);

    size_t i_iconv;
    uint8_t *p_iconv = config_striconv(str, charset, &i_iconv);
    dvb_string->p = dvb_string_set(p_iconv, i_iconv, charset, &dvb_string->i);
    free(p_iconv);
}

const char *config_get_network_name( void )
{
    return network_name_global;
}

void config_set_network_name( const char *name )
{
    network_name_global = name;
    config_strdvb( &network_name, name, dvb_charset_global );
}

const char *config_get_provider_name( void )
{
    return provider_name_global;
}

void config_set_provider_name( const char *provider )
{
    provider_name_global = provider;
    config_strdvb( &provider_name, provider, dvb_charset_global );
}

static void config_pid_init( config_pid_t *v )
{
    if ( v ) {
        config_pid_init_link( v );
        v->type = OUTPUT_CONFIG_TYPE_STATIC;
        v->es_type = ES_TYPE_ANY;
        memset( &v->lang, 0, sizeof(v->lang) );
        v->value = 0;
        v->new_pid = 0;
    }
}

config_pid_t *config_pid_new( void )
{
    config_pid_t *pid = malloc( sizeof (*pid) );
    config_pid_init( pid );
    return pid;
}

bool config_pid_cmp( const config_pid_t *v1, const config_pid_t *v2 )
{
    if (v1 == NULL || v2 == NULL) return !(v1 == v2);
    return !( v1->type == v2->type
             && v1->es_type == v2->es_type
             && v1->value == v2->value );
}

void config_pid_free( config_pid_t *pid )
{
    if ( pid ) {
        config_pid_del_link( pid );
        free( pid );
    }
}

static void config_sid_init( config_sid_t *sid )
{
    if ( sid ) {
        config_sid_init_link( sid );
        config_pid_list_init( &sid->pids );
        sid->type = OUTPUT_CONFIG_TYPE_STATIC;
        sid->value = 0;
        sid->new_sid = 0;
        sid->pmt_pid = 0;
        dvb_string_init( &sid->new_name );
        dvb_string_init( &sid->new_provider );
        dvb_string_copy( &sid->new_provider, &provider_name );
        sid->name = NULL;
        sid->provider = NULL;
    }
}

config_sid_t *config_sid_new( void )
{
    config_sid_t *sid = malloc( sizeof (*sid) );
    config_sid_init( sid );
    return sid;
}

bool config_sid_cmp( const config_sid_t *v1, const config_sid_t *v2 )
{
    if ( v1 == NULL || v2 == NULL ) return !( v1 == v2 );
    return !( v1->type == v2->type && v1->value == v2->value );
}

void config_sid_free( config_sid_t *sid )
{
    if ( sid ) {
        config_sid_del_link( sid );
        free( sid->name );
        free( sid->provider );
        dvb_string_clean( &sid->new_name );
        dvb_string_clean( &sid->new_provider );
        config_pid_list_clean( &sid->pids );
        free( sid );
    }
}

void config_Init( output_config_t *p_config )
{
    memset( p_config, 0, sizeof(output_config_t) );

    p_config->psz_displayname = NULL;
    p_config->i_network_id = network_id_global;
    dvb_string_init(&p_config->network_name);
    p_config->psz_srcaddr = NULL;

    p_config->i_family = AF_UNSPEC;
    p_config->connect_addr.ss_family = AF_UNSPEC;
    p_config->bind_addr.ss_family = AF_UNSPEC;
    p_config->i_if_index_v6 = -1;
    p_config->i_srcport = 0;

    config_sid_list_init( &p_config->sids );
    config_pid_list_init( &p_config->pids );
    p_config->b_do_remap = false;
    unsigned int i;
    for ( i = 0; i < N_MAP_PIDS; i++ ) {
        p_config->pi_confpids[i]  = UNUSED_PID;
    }
}

void config_Free( output_config_t *p_config )
{
    free( p_config->psz_displayname );
    dvb_string_clean( &p_config->network_name );
    config_pid_list_clean( &p_config->pids );
    config_sid_list_clean( &p_config->sids );
    free( p_config->psz_srcaddr );
}

void config_Defaults( output_config_t *p_config )
{
    config_Init( p_config );

    p_config->i_config = (udp_global ? OUTPUT_UDP : 0) |
                         (dvb_global ? OUTPUT_DVB : 0) |
                         (epg_global ? OUTPUT_EPG : 0);
    p_config->i_max_retention = retention_global;
    p_config->i_output_latency = latency_global;
    p_config->i_tsid = -1;
    p_config->i_ttl = ttl_global;
    p_config->ssrc = ssrc_global;
    dvb_string_copy(&p_config->network_name, &network_name);
}

static char *option_next( char **str_p, const char *delim,
                          char *name, char **value)
{
    char *str = str_p ? *str_p : NULL;
    char *input = !name && str && *str == '/' ? str : NULL;
    char *saveptr = name ? str : NULL;
    char *token = NULL;

    if ( (input || saveptr) && delim )
        token = strtok_r( input, delim, &saveptr );
    char *sep = token ? index( token, '=' ) : NULL;
    if ( sep )
        *sep++ = '\0';
    if ( value )
        *value = sep;
    if ( str_p )
        *str_p = token ? saveptr : NULL;
    return token;
}

#define option_each( Str, Delim, Name, Value )                              \
    for ( char *Value, *Name = option_next ( &Str, Delim, NULL, &Value );   \
          Name != NULL;                                                     \
          Name = option_next( &Str, Delim, Name, &Value ) )

#define IS_OPTION( option ) (!strcasecmp( name, option ))

bool config_ParseHost( output_config_t *p_config, char *psz_string )
{
    config_sid_t *sid = NULL;
    struct addrinfo *p_ai;
    int i_mtu;

    p_config->psz_displayname = strdup( psz_string );
    char *tmp = index( p_config->psz_displayname, '/' );
    if ( tmp )
        *tmp = '\0';

    p_ai = ParseNodeService( psz_string, &psz_string, DEFAULT_PORT );
    if ( p_ai == NULL ) return false;
    memcpy( &p_config->connect_addr, p_ai->ai_addr, p_ai->ai_addrlen );
    freeaddrinfo( p_ai );

    p_config->i_family = p_config->connect_addr.ss_family;
    if ( p_config->i_family == AF_UNSPEC ) return false;

    if ( psz_string == NULL || !*psz_string ) goto end;

    if ( *psz_string == '@' )
    {
        psz_string++;
        p_ai = ParseNodeService( psz_string, &psz_string, 0 );
        if ( p_ai == NULL || p_ai->ai_family != p_config->i_family )
            Warn( "invalid bind address" );
        else
            memcpy( &p_config->bind_addr, p_ai->ai_addr, p_ai->ai_addrlen );
        freeaddrinfo( p_ai );
    }

    const char *psz_charset = NULL;
    const char *psz_network_name = NULL;
    const char *psz_service_name = NULL;
    const char *psz_provider_name = NULL;
    sid = config_sid_new( );

    option_each( psz_string, "/", name, value )
    {
        if ( IS_OPTION("udp") )
            p_config->i_config |= OUTPUT_UDP;
        else if ( IS_OPTION("dvb") )
            p_config->i_config |= OUTPUT_DVB;
        else if ( IS_OPTION("epg") )
            p_config->i_config |= OUTPUT_EPG;
        else if ( IS_OPTION("tsid") && value )
            p_config->i_tsid = strtol( value, NULL, 0 );
        else if ( IS_OPTION("retention") && value )
            p_config->i_max_retention = strtoll( value, NULL, 0 ) * 1000;
        else if ( IS_OPTION("latency") && value )
            p_config->i_output_latency = strtoll( value, NULL, 0 ) * 1000;
        else if ( IS_OPTION("ttl") && value )
            p_config->i_ttl = strtol( value, NULL, 0 );
        else if ( IS_OPTION("tos") && value )
            p_config->i_tos = strtol( value, NULL, 0 );
        else if ( IS_OPTION("mtu") && value )
            p_config->i_mtu = strtol( value, NULL, 0 );
        else if ( IS_OPTION("ifindex") && value )
            p_config->i_if_index_v6 = strtol( value, NULL, 0 );
        else if ( IS_OPTION("networkid") && value )
            p_config->i_network_id = strtol( value, NULL, 0 );
        else if ( IS_OPTION("onid") && value)
            p_config->i_onid = strtol( value, NULL, 0 );
        else if ( IS_OPTION("charset") && value )
            psz_charset = value;
        else if ( IS_OPTION("networkname") && value )
            psz_network_name = value;
        else if ( IS_OPTION("srvname") && value )
            psz_service_name = value;
        else if ( IS_OPTION("srvprovider") && value )
            psz_provider_name = value;
        else if ( IS_OPTION("srcaddr") && value )
        {
            if ( p_config->i_family != AF_INET ) {
                Err( "RAW sockets currently implemented for ipv4 only");
                return false;
            }
            free( p_config->psz_srcaddr );
            p_config->psz_srcaddr = config_stropt( value );
            p_config->i_config |= OUTPUT_RAW;
        }
        else if ( IS_OPTION("srcport") && value )
            p_config->i_srcport = strtol( value, NULL, 0 );
        else if ( IS_OPTION("ssrc") && value )
            p_config->ssrc = inet_addr( value );
        else if ( IS_OPTION("pidmap") && value )
        {
            for (int i = 0; i < N_MAP_PIDS; i++)
            {
                char *endptr;
                unsigned long newpid = strtol( value, &endptr, 0 );
                if ( endptr != value && ( *endptr == ',' || *endptr == '\0' ) )
                    p_config->pi_confpids[i] = newpid;
                value = *endptr == ',' ? endptr + 1 : "";
            }
            p_config->b_do_remap = true;
        }
        else if ( IS_OPTION("newsid") && value )
            sid->new_sid = strtol( value, NULL, 0 );
        else
            Warn( "unrecognized option %s%s%s",
                  name, value ? "=" : "", value ?: "" );
    }

    if (psz_network_name != NULL)
        config_strdvb( &p_config->network_name, psz_network_name, psz_charset );
    if (psz_service_name != NULL)
        config_strdvb( &sid->new_name, psz_service_name, psz_charset );
    if (psz_provider_name != NULL)
        config_strdvb( &sid->new_provider, psz_provider_name, psz_charset );

end:
    i_mtu = p_config->i_family == AF_INET6 ? DEFAULT_IPV6_MTU :
            DEFAULT_IPV4_MTU;

    if ( !p_config->i_mtu )
        p_config->i_mtu = i_mtu;
    else if ( p_config->i_mtu < TS_SIZE + RTP_HEADER_SIZE )
    {
        Warn( "invalid MTU %d, setting %d", p_config->i_mtu, i_mtu );
        p_config->i_mtu = i_mtu;
    }

    if ( sid )
        config_sid_list_add( &p_config->sids, sid );

    return true;
}

static bool config_ParsePid( output_config_t *p_config,
                             config_pid_list_t *list, char *psz_string )
{
    while ( isspace( *psz_string ) )
        psz_string++;

    config_pid_t *config_pid = config_pid_new( );

    if ( !isdigit ( *psz_string ) )
    {
        config_pid->type = *psz_string++;

        if ( !isdigit( *psz_string ) &&
             *psz_string != '=' &&
             *psz_string != '\0')
        {
            config_pid->es_type = *psz_string++;

            if ( *psz_string == 'l' )
            {
                psz_string++;
                if ( strlen( psz_string ) < 3 ) {
                    Err( "invalid language code %s", psz_string );
                    config_pid_free( config_pid );
                    return false;
                }

                config_pid->lang[0] = psz_string[0];
                config_pid->lang[1] = psz_string[1];
                config_pid->lang[2] = psz_string[2];
                config_pid->lang[3] = '\0';

                psz_string += 3;
            }
        }
    }

    char *endptr = psz_string;
    if ( config_pid->type != OUTPUT_CONFIG_TYPE_ANY &&
         config_pid->type != OUTPUT_CONFIG_TYPE_ALL )
    {
        config_pid->value = strtol( psz_string, &endptr, 0 );
        if ( endptr == psz_string )
        {
            Err( "invalid pid selection %s", psz_string );
            config_pid_free( config_pid );
            return false;
        }
    }

    if ( *endptr == '=' )
    {
        psz_string = endptr + 1;
        config_pid->new_pid = strtol( psz_string, &endptr, 0 );
        if ( endptr == psz_string )
        {
            Err(" invalid pid remap %s", psz_string );
            config_pid_free( config_pid );
            return false;
        }
    }

    if ( strlen ( endptr ) )
    {
        Err(" invalid pid selection %s", psz_string );
        config_pid_free( config_pid );
        return false;
    }

    config_pid_list_add( list, config_pid );
    return true;
}

static bool config_ParsePids( output_config_t *p_config, char *psz_string )
{
    if ( config_sid_list_is_empty( &p_config->sids ) || !psz_string )
        return true;

    char *psz_parser = NULL;
    char *psz_token = strtok_r( psz_string, ",", &psz_parser );
    while ( psz_token != NULL )
    {
        if ( !config_ParsePid( p_config, &p_config->pids, psz_token ) )
            return false;
        psz_token = strtok_r( NULL, ",", &psz_parser );
    }
    return true;
}

static bool config_ParseService( output_config_t *p_config, char *psz_string )
{
    while ( isspace( *psz_string ) )
        psz_string++;

    config_sid_t *config_sid = config_sid_new( );

    if ( !isdigit( *psz_string ) )
        config_sid->type = *psz_string++;

    char *endptr = psz_string;
    if ( config_sid->type != OUTPUT_CONFIG_TYPE_ANY &&
         config_sid->type != OUTPUT_CONFIG_TYPE_ALL )
    {
        config_sid->value = strtol( psz_string, &endptr, 0 );
        if ( endptr == psz_string )
        {
            Err( "invalid service selection %s", psz_string );
            config_sid_free( config_sid );
            return false;
        }
    }

    char *options = endptr;
    const char *charset = NULL;
    option_each( options, "/", name, value )
    {
        if ( IS_OPTION("charset") && value )
            charset = value;
        else if ( IS_OPTION( "name") && value ) {
            free( config_sid->name );
            config_sid->name = strdup( value );
        } else if ( IS_OPTION( "provider" ) && value ) {
            free( config_sid->provider );
            config_sid->provider = strdup( value );
        } else if ( IS_OPTION( "new_name" ) && value )
            config_strdvb( &config_sid->new_name, value, charset );
        else if ( IS_OPTION( "new_provider" ) && value )
            config_strdvb( &config_sid->new_provider, value, charset );
        else if ( IS_OPTION( "new_sid" ) && value )
            config_sid->new_sid = strtol( value, &endptr, 0 );
        else if ( IS_OPTION( "pmt_pid" ) && value )
            config_sid->pmt_pid = strtol( value, &endptr, 0 );
        else if ( IS_OPTION( "pid" ) && value )
        {
            if ( !config_ParsePid( p_config, &config_sid->pids, value ) ) {
                config_sid_free( config_sid );
                return false;
            }
        }
    }

    config_sid_list_add( &p_config->sids, config_sid );
    return true;
}

static bool config_ParseServices( output_config_t *p_config, char *psz_string )
{
    if ( !psz_string || !strlen( psz_string ) )
        return false;

    if ( !index( psz_string, ',' ) )
    {
        /* legacy configuration syntax */
        config_sid_t *sid = config_sid_list_first( &p_config->sids );
        char *endptr;
        uint16_t value = strtol( psz_string, &endptr, 0 );
        if ( sid && endptr != psz_string && !strlen( endptr ) ) {
            sid->value = value;
            return true;
        }
    }
    config_sid_list_clean( &p_config->sids );

    char *psz_parser = NULL;
    char *psz_token = strtok_r( psz_string, ",", &psz_parser );
    while ( psz_token != NULL )
    {
        if ( !config_ParseService( p_config, psz_token ) )
            return false;
        psz_token = strtok_r( NULL, ",", &psz_parser );
    }
    return true;
}

void config_ReadFile( void )
{
    FILE *p_file;
    char psz_line[2048];

    if ( conf_file_global == NULL )
    {
        Err( "no config file" );
        return;
    }

    if ( (p_file = fopen( conf_file_global, "r" )) == NULL )
    {
        Err( "can't fopen config file %s", conf_file_global );
        return;
    }

    output_list_t old_outputs;
    output_list_move( &outputs, &old_outputs );

    while ( fgets( psz_line, sizeof(psz_line), p_file ) != NULL )
    {
        output_config_t config;
        output_t *p_output;
        char *psz_token, *psz_parser;

        psz_parser = strchr( psz_line, '#' );
        if ( psz_parser != NULL )
            *psz_parser-- = '\0';
        while ( psz_parser >= psz_line && isblank( *psz_parser ) )
            *psz_parser-- = '\0';
        if ( psz_line[0] == '\0' )
            continue;

        config_Defaults( &config );

        psz_token = strtok_r( psz_line, "\t\n ", &psz_parser );
        if ( psz_token == NULL || !config_ParseHost( &config, psz_token ))
        {
            config_Free( &config );
            continue;
        }

        psz_token = strtok_r( NULL, "\t\n ", &psz_parser );
        if ( psz_token == NULL )
        {
            config_Free( &config );
            continue;
        }
        if( atoi( psz_token ) == 1 )
            config.i_config |= OUTPUT_WATCH;
        else
            config.i_config &= ~OUTPUT_WATCH;

        psz_token = strtok_r( NULL, "\t\n ", &psz_parser );
        if ( !config_ParseServices( &config, psz_token ) )
        {
            config_Free( &config );
            continue;
        }

        psz_token = strtok_r( NULL, "\t\n ", &psz_parser );
        if ( !config_ParsePids( &config, psz_token ) )
        {
            config_Free( &config );
            continue;
        }

        config_Print( &config );

        p_output = output_Find( &old_outputs, &config );

        if ( p_output == NULL )
            p_output = output_Create( &config );

        if ( p_output != NULL )
        {
            output_list_add( &outputs, p_output );
            output_Change( p_output, &config );
            demux_Change( p_output, &config );
        }

        config_Free( &config );
    }

    fclose( p_file );

    output_list_flush( &old_outputs, output )
    {
        output_config_t config;

        config_Init( &config );

        output_Dbg( output, "closing" );
        demux_Change( output, &config );
        output_Close( output );

        config_Free( &config );
    }
}

void config_Print( output_config_t *p_config )
{
    if ( config_sid_list_is_empty( &p_config->sids ) )
    {
        Dbg( "conf: %s config=0x%"PRIx64" sid=*",
                 p_config->psz_displayname, p_config->i_config);
        return;
    }

    const char *psz_base = "conf: %s config=0x%"PRIx64;
    unsigned nb_sids = 0;
    config_sid_list_each( &p_config->sids, sid )
        nb_sids++;
    unsigned nb_pids = 0;
    config_pid_list_each( &p_config->pids, pid )
        nb_pids++;

    size_t i_len = strlen(psz_base);
    if (nb_sids)
        i_len += strlen(" sids[]=") + 6 + (2 + 6) * nb_sids;
    if (nb_pids)
        i_len += strlen(" pids[]=") + 6 + (3 + 6) * nb_pids;
    i_len += 1;
    char psz_format[i_len];
    int i = strlen(psz_base);

    strcpy( psz_format, psz_base );

    if (nb_sids)
    {
        bool first = true;

        i += sprintf( psz_format + i, " sids[%u]=", nb_sids );
        config_sid_list_each( &p_config->sids, sid )
        {
            i += sprintf( psz_format + i, "%s%c%u",
                          first ? "" : ",",
                          sid->type, sid->value);
            first = false;
        }
    }

    if (nb_pids)
    {
        bool first = true;

        i += sprintf( psz_format + i, " pids[%u]=", nb_pids );
        config_pid_list_each( &p_config->pids, pid )
        {
            i += sprintf( psz_format + i, "%s%c%c%u",
                          first ? "" : ",",
                          pid->type, pid->es_type, pid->value );
            first = false;
        }
    }

    Dbg( psz_format, p_config->psz_displayname,
             p_config->i_config,
             nb_pids );
}

void config_Close( void )
{
    dvb_string_clean( &network_name );
    dvb_string_clean( &provider_name );
    if ( conf_iconv != (iconv_t)-1 )
        iconv_close( conf_iconv );
}
