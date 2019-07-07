/*****************************************************************************
 * dlna.cpp :  DLNA MediaRenderer module
 *****************************************************************************
 * Copyright (C) 2019 VLC authors and VideoLAN
 *
 * Authors: Johan Gunnarsson <johan.gunnarsson@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <cstring>
#include <ctime>
#include <map>
#include <unordered_map>
#include <iostream>
#include <functional>

#include "../services_discovery/upnp-wrapper.hpp"

#include <vlc_plugin.h>
#include <vlc_interface.h>
#include <vlc_playlist.h>
#include <vlc_player.h>
#include <vlc_strings.h>
#include <vlc_input.h>

#include "dlna.hpp"

#define SINK_PROTOCOL_INFO ("http-get:*:video/mpeg:*," \
                            "http-get:*:video/mp4:*," \
                            "http-get:*:video/vnd.dlna.mpeg-tts:*," \
                            "http-get:*:video/avi:*," \
                            "http-get:*:video/x-matroska:*," \
                            "http-get:*:video/x-ms-wmv:*," \
                            "http-get:*:video/wtv:*," \
                            "http-get:*:audio/mpeg:*," \
                            "http-get:*:audio/mp3:*," \
                            "http-get:*:audio/mp4:*," \
                            "http-get:*:audio/x-ms-wma*," \
                            "http-get:*:audio/wav:*," \
                            "http-get:*:audio/L16:*," \
                            "http-get:*image/jpeg:*," \
                            "http-get:*image/png:*," \
                            "http-get:*image/gif:*," \
                            "http-get:*image/tiff:*")

using upnp_ptr =
    std::unique_ptr<UpnpInstanceWrapper,
                    std::function<void(UpnpInstanceWrapper*)>>;

using player_listener_ptr =
    std::unique_ptr<vlc_player_listener_id,
                    std::function<void(vlc_player_listener_id*)>>;

using player_aout_listener_ptr =
    std::unique_ptr<vlc_player_aout_listener_id,
                    std::function<void(vlc_player_aout_listener_id*)>>;

using ixml_document_ptr =
    std::unique_ptr<IXML_Document,
                    void(*)(IXML_Document*)>;

using parammap =
    std::unordered_map<std::string,
                       std::string>;

using ActionRequestHandler =
    bool (*)( const parammap&, parammap&, intf_thread_t* );

struct intf_sys_t
{
    vlc_player_t *player;
    upnp_ptr p_upnp;
    std::shared_ptr<EventHandler> p_listener;
    player_listener_ptr p_player_listener;
    player_aout_listener_ptr p_player_aout_listener;
};

namespace
{

/**
 * Convert ticks (in microseconds) to a string in the form H:MM:SS. Can't use
 * secstotimestr since it will omit hours if time is less than 1 hour. Can't
 * use strftime since it will limit the H part to 0-23. A 25 hour long media
 * should produce the string "25:00:00".
 */
std::string
time_to_string( vlc_tick_t ticks )
{
    auto time_in_seconds = SEC_FROM_VLC_TICK(ticks);
    unsigned int s = time_in_seconds % 60;
    time_in_seconds -= s;
    unsigned int m = (time_in_seconds / 60) % 60;
    time_in_seconds -= 60 * m;
    unsigned int h = time_in_seconds / (60 * 60);

    char str[20] = {};
    if( snprintf( str, sizeof( str ) - 1, "%u:%02u:%02u", h, m, s ) < 0 )
        return std::string( "0:00:00" );

    return std::string( str );
}

float
frac_to_float( const std::string& frac )
{
    int n = 0;
    unsigned int d = 1;
    if( !(sscanf( frac.c_str(), "%d/%u", &n, &d ) == 2 ||
          sscanf( frac.c_str(), "%d", &n ) == 1) )
        return 1.0; /* invalid input */
    if( n == 0 || d == 0 )
        return 1.0; /* invalid input */

    return (float) n / (float) d;
}

long
gcd( long n, long d ) {
    if( d == 0 )
        return n;

    return gcd( d, n % d );
}

std::string
float_to_frac( const float frac )
{
    long n = std::lround( frac * 100 );
    long d = 100;
    long common_denominator = gcd( n, d );

    return std::to_string( n / common_denominator ) + "/" +
           std::to_string( d / common_denominator );
}

bool
handle_AVT_SetAVTransportURI( const parammap& in_params, parammap&,
                              intf_thread_t *p_intf )
{
    auto s = in_params.find( "CurrentURI" );
    if( s == in_params.end() )
        return false;

    input_item_t *item = input_item_New(s->second.c_str(), nullptr);
    if( !item )
    {
        msg_Err( p_intf, "Failed to parse URL" );
        return false;
    }

    vlc_player_Lock( p_intf->p_sys->player );
    bool start = vlc_player_IsStarted( p_intf->p_sys->player );
    vlc_player_SetCurrentMedia( p_intf->p_sys->player, item );
    if( start )
        vlc_player_Start( p_intf->p_sys->player );
    vlc_player_Unlock( p_intf->p_sys->player );

    input_item_Release( item );

    return true;
}

bool
handle_AVT_GetMediaInfo( const parammap&, parammap& out_params,
                         intf_thread_t *p_intf )
{
    vlc_player_Lock( p_intf->p_sys->player );
    vlc_tick_t length = vlc_player_GetLength( p_intf->p_sys->player );
    vlc_player_Unlock( p_intf->p_sys->player );

    out_params["MediaDuration"] = time_to_string( length );

    return true;
}


bool
handle_AVT_GetTransportInfo( const parammap&, parammap& out_params,
                             intf_thread_t *p_intf )
{
    vlc_player_Lock( p_intf->p_sys->player );
    enum vlc_player_state state = vlc_player_GetState( p_intf->p_sys->player );
    enum vlc_player_error error = vlc_player_GetError( p_intf->p_sys->player );
    float rate = vlc_player_GetRate( p_intf->p_sys->player );
    vlc_player_Unlock( p_intf->p_sys->player );

    switch( state )
    {
    case VLC_PLAYER_STATE_STOPPED:
        out_params["CurrentTransportState"] = "STOPPED";
        break;
    case VLC_PLAYER_STATE_PLAYING:
        out_params["CurrentTransportState"] = "PLAYING";
        break;
    case VLC_PLAYER_STATE_PAUSED:
        out_params["CurrentTransportState"] = "PAUSED_PLAYBACK";
        break;
    case VLC_PLAYER_STATE_STARTED: /* fall through */
    case VLC_PLAYER_STATE_STOPPING:
        out_params["CurrentTransportState"] = "TRANSITIONING";
        break;
    default:
        out_params["CurrentTransportState"] = "UNKNOWN";
        break;
    }

    switch( error )
    {
    case VLC_PLAYER_ERROR_NONE:
        out_params["CurrentTransportStatus"] = "OK";
        break;
    case VLC_PLAYER_ERROR_GENERIC: /* fall through */
    default:
        out_params["CurrentTransportStatus"] = "ERROR_OCCURRED";
        break;
    }

    out_params["CurrentSpeed"] = float_to_frac( rate );

    return true;
}

bool
handle_AVT_GetPositionInfo( const parammap&, parammap& out_params,
                            intf_thread_t *p_intf )
{
    vlc_player_Lock( p_intf->p_sys->player );
    vlc_tick_t length = vlc_player_GetLength( p_intf->p_sys->player );
    vlc_tick_t time = vlc_player_GetTime( p_intf->p_sys->player );
    vlc_player_Unlock( p_intf->p_sys->player );

    out_params["Track"] = "0";
    out_params["TrackDuration"] = time_to_string( length );
    out_params["TrackMetaData"] = "";
    out_params["TrackURI"] = "";
    out_params["RelTime"] = time_to_string( time );
    out_params["AbsTime"] = time_to_string( time );
    out_params["RelCount"] = std::to_string( time );
    out_params["AbsCount"] = std::to_string( time );

    return true;
}

bool
handle_AVT_Stop( const parammap&, parammap&, intf_thread_t *p_intf )
{
    vlc_player_Lock( p_intf->p_sys->player );
    vlc_player_Stop( p_intf->p_sys->player );
    vlc_player_Unlock( p_intf->p_sys->player );

    return true;
}

bool
handle_AVT_Play( const parammap& in_params, parammap&, intf_thread_t *p_intf )
{
    auto speed = in_params.find( "Speed" );
    if( speed == in_params.end() )
        return false;

    float new_rate = frac_to_float( speed->second );

    vlc_player_Lock( p_intf->p_sys->player );
    if( vlc_player_GetRate( p_intf->p_sys->player ) != new_rate )
        vlc_player_ChangeRate( p_intf->p_sys->player, new_rate );
    if( vlc_player_IsStarted( p_intf->p_sys->player ) )
        vlc_player_Resume( p_intf->p_sys->player );
    else
        vlc_player_Start( p_intf->p_sys->player );
    vlc_player_Unlock( p_intf->p_sys->player );

    return true;
}

bool
handle_AVT_Pause( const parammap&, parammap&, intf_thread_t *p_intf )
{
    vlc_player_Lock( p_intf->p_sys->player );
    vlc_player_Pause( p_intf->p_sys->player );
    vlc_player_Unlock( p_intf->p_sys->player );

    return true;
}

bool
handle_AVT_Seek( const parammap& in_params, parammap&, intf_thread_t *p_intf )
{
    auto unit = in_params.find( "Unit" );
    if( unit == in_params.end() )
        return false;

    auto target = in_params.find( "Target" );
    if( target == in_params.end() )
        return false;

    if( unit->second == "ABS_TIME" || unit->second == "REL_TIME" )
    {
        unsigned int h = 0, m = 0, s = 0;
        if( !(sscanf( target->second.c_str(), "%u:%u:%u", &h, &m, &s ) == 3 ||
              sscanf( target->second.c_str(), "%u:%u", &m, &s ) == 2) )
            return false;
        if( m >= 60 || s >= 60 )
            return false;

        vlc_player_Lock( p_intf->p_sys->player );
        vlc_player_SeekByTime( p_intf->p_sys->player,
                               VLC_TICK_FROM_SEC(h*60*60 + m*60 + s),
                               VLC_PLAYER_SEEK_FAST,
                               VLC_PLAYER_WHENCE_ABSOLUTE );
        vlc_player_Unlock( p_intf->p_sys->player );
    } else {
        msg_Warn( p_intf, "Unsupported seek unit: %s", unit->second.c_str() );
        return false;
    }

    return true;
}

bool
handle_CM_GetProtocolInfo( const parammap&, parammap& out_params,
                           intf_thread_t * )
{
    out_params["Source"] = "";
    out_params["Sink"] = SINK_PROTOCOL_INFO;

    return true;
}

bool
handle_RC_GetVolume( const parammap&, parammap& out_params,
                     intf_thread_t *p_intf )
{
    /* Volume in range [0.0, 2.0] or -1.0 if no audio */
    float volume = vlc_player_aout_GetVolume( p_intf->p_sys->player );

    /* Enforce [0.0, 1.0] range */
    volume = VLC_CLIP(volume, 0.0, 1.0);

    /* Output [0, 100] range */
    out_params["CurrentVolume"] = std::to_string( std::lround( volume * 100 ) );

    return true;
}

bool
handle_RC_SetVolume( const parammap& in_params, parammap&,
                     intf_thread_t *p_intf )
{
    /* Volume in range [0, 100] */
    auto volume = in_params.find( "DesiredVolume" );
    if( volume == in_params.end() )
        return false;

    /* Enforce [0, 100] range */
    unsigned long v = VLC_CLIP(std::stoul( volume->second ), 0, 100);

    /* Outputs [0.0, 1.0] range */
    vlc_player_aout_SetVolume( p_intf->p_sys->player, v / 100.0 );

    return true;
}

bool
handle_RC_GetMute( const parammap&, parammap& out_params,
                   intf_thread_t *p_intf )
{
    bool muted = vlc_player_aout_IsMuted( p_intf->p_sys->player );

    if( muted )
        out_params["CurrentMute"] = "1";
    else
        out_params["CurrentMute"] = "0";

    return true;
}

bool
handle_RC_SetMute( const parammap& in_params, parammap&,
                   intf_thread_t *p_intf )
{
    auto mute = in_params.find( "DesiredMute" );
    if( mute == in_params.end() )
        return false;

    std::string m = mute->second;

    if( m == "1" || m == "true" || m == "yes" )
        vlc_player_aout_Mute( p_intf->p_sys->player, true );
    else if( m == "0" || m == "false" || m == "no" )
        vlc_player_aout_Mute( p_intf->p_sys->player, false );

    return true;
}

#define SRV_AVT "urn:upnp-org:serviceId:AVTransport"
#define SRV_RC  "urn:upnp-org:serviceId:RenderingControl"
#define SRV_CM  "urn:upnp-org:serviceId:ConnectionManager"

static struct {
    const char *service;
    const char *action;
    ActionRequestHandler handler;
} actions[] = {
    { SRV_AVT, "SetAVTransportURI", handle_AVT_SetAVTransportURI },
    { SRV_AVT, "GetMediaInfo", handle_AVT_GetMediaInfo },
    { SRV_AVT, "GetTransportInfo", handle_AVT_GetTransportInfo },
    { SRV_AVT, "GetPositionInfo", handle_AVT_GetPositionInfo },
    { SRV_AVT, "Stop", handle_AVT_Stop },
    { SRV_AVT, "Play", handle_AVT_Play },
    { SRV_AVT, "Pause", handle_AVT_Pause },
    { SRV_AVT, "Seek", handle_AVT_Seek },
    { SRV_CM, "GetProtocolInfo", handle_CM_GetProtocolInfo },
    { SRV_RC, "GetVolume", handle_RC_GetVolume },
    { SRV_RC, "SetVolume", handle_RC_SetVolume },
    { SRV_RC, "GetMute", handle_RC_GetMute },
    { SRV_RC, "SetMute", handle_RC_SetMute },
    { NULL, NULL, NULL }
};

parammap
build_param_map( IXML_Node *p_node )
{
    parammap params;

    for( IXML_Node *param = ixmlNode_getFirstChild( p_node );
         param != NULL;
         param = ixmlNode_getNextSibling( param ) )
    {
        const DOMString key = ixmlNode_getNodeName(param);
        if( !key )
            continue;

        IXML_Node *vnode = ixmlNode_getFirstChild( param );
        if( !vnode )
            continue;

        const DOMString value = ixmlNode_getNodeValue( vnode );
        if( !value )
            continue;

        params[key] = value;
    }

    return params;
}

std::unique_ptr<char>
build_event_xml( const char **keys, const char **values, int count )
{
    IXML_Document *p_doc = ixmlDocument_createDocument();
    if( !p_doc )
        return nullptr;

    ixml_document_ptr doc( p_doc, &ixmlDocument_free );

    IXML_Element *event = ixmlDocument_createElement( doc.get(), "Event" );
    if( !event )
        return nullptr;

    if( ixmlNode_appendChild( (IXML_Node *)doc.get(),
                              (IXML_Node *)event ) != IXML_SUCCESS )
    {
        ixmlElement_free( event );
        return nullptr;
    }

    IXML_Element *instance = ixmlDocument_createElement( doc.get(),
                                                         "InstanceID" );
    if( !instance )
        return nullptr;

    if( ixmlNode_appendChild( (IXML_Node *)event,
                              (IXML_Node *)instance ) != IXML_SUCCESS )
    {
        ixmlElement_free( instance );
        return nullptr;
    }

    if( ixmlElement_setAttribute( instance, "val", "0") != IXML_SUCCESS )
        return nullptr;

    for( int i = 0; i < count; i++ )
    {
        IXML_Element *arg = ixmlDocument_createElement( doc.get(), keys[i] );
        if( !arg )
            return nullptr;

        if( ixmlNode_appendChild( (IXML_Node *)instance,
                                  (IXML_Node *)arg ) != IXML_SUCCESS )
        {
            ixmlElement_free( arg );
            return nullptr;
        }

        if( ixmlElement_setAttribute( arg, "val", values[i] ) != IXML_SUCCESS )
            return nullptr;
    }

    std::unique_ptr<char> xmlbuf( ixmlNodetoString( (IXML_Node *)doc.get() ) );
    if( !xmlbuf )
        return nullptr;

    return std::unique_ptr<char>( vlc_xml_encode( xmlbuf.get() ) );
}

void
emit_event( intf_thread_t *p_intf, const std::string& sid,
            const std::string& key, const std::string& value )
{
    const char *event_keys[1] = { key.c_str() };
    const char *event_values[1] = { value.c_str() };

    std::unique_ptr<char> event_xml = build_event_xml( event_keys,
                                                       event_values, 1 );
    if( !event_xml )
    {
        /* If we failed to build XML for this event there we might as well
           return early here because there's nothing to send. */
        msg_Warn( p_intf, "Failed to build event XML" );
        return;
    }

    const char *var_keys[1] = { "LastChange" };
    const char *var_values[1] = { event_xml.get() };

    int ret = UpnpNotify( p_intf->p_sys->p_upnp->device_handle(),
                          p_intf->p_sys->p_upnp->udn().c_str(),
                          sid.c_str(),
                          (const char **) &var_keys,
                          (const char **) &var_values,
                          1 );
    if( ret != UPNP_E_SUCCESS )
        msg_Dbg( p_intf, "UpnpNotify failed" );
}

int
EventHandler::onActionRequest( UpnpActionRequest *p_event, void * )
{
    /* For example urn:upnp-org:serviceId:AVTransport */
    const char *service_id = UpnpActionRequest_get_ServiceID_cstr( p_event );

    /* For example SetAVTransportURI */
    const char *action_name = UpnpActionRequest_get_ActionName_cstr( p_event );

    /* "Body" XML node in the request */
    IXML_Document *body = UpnpActionRequest_get_ActionRequest( p_event );
    if( !body )
        return 0;

    for( IXML_Node *action = ixmlNode_getFirstChild( (IXML_Node*) body );
         action != NULL;
         action = ixmlNode_getNextSibling( action ) )
    {
        const parammap in_params = build_param_map( action );

        for( size_t i = 0; actions[i].handler; i++ )
        {
            if( strcmp( actions[i].service, service_id ) ||
                strcmp( actions[i].action, action_name ) )
                continue;

            parammap out_params;

            int r = actions[i].handler( in_params, out_params, p_intf );
            if( !r )
                continue;

            IXML_Document *d = UpnpMakeActionResponse( action_name,
                                                       service_id,
                                                       0,
                                                       NULL );
            if( !d )
                continue;

            UpnpActionRequest_set_ActionResult( p_event, d );

            for( auto& x : out_params )
            {
                int r = UpnpAddToActionResponse( &d,
                                                 action_name,
                                                 service_id,
                                                 x.first.c_str(),
                                                 x.second.c_str() );
                if( r != UPNP_E_SUCCESS )
                {
                    if( d )
                        ixmlDocument_free( d );

                    UpnpActionRequest_set_ActionResult( p_event, NULL );
                    UpnpActionRequest_set_ErrCode( p_event, r );
                    return r;
                }
            }

            UpnpActionRequest_set_ErrCode( p_event, UPNP_E_SUCCESS );
            return UPNP_E_SUCCESS;
        }
    }

    UpnpActionRequest_set_ErrCode( p_event, UPNP_E_INTERNAL_ERROR );
    return UPNP_E_INTERNAL_ERROR;
}

int
EventHandler::onSubscriptionRequest( UpnpSubscriptionRequest *p_event, void * )
{
    /* For example urn:upnmapp-org:serviceId:AVTransport */
    const char *service_id =
        UpnpSubscriptionRequest_get_ServiceId_cstr( p_event );

    /* For example uuid:034fc8dc-ec22-44e5-a79b-38c935f11663 */
    const char *udn = UpnpSubscriptionRequest_get_UDN_cstr( p_event );

    /* For example uuid:d0874e24-a80b-11e9-9fd4-bed70abd916c */
    const char *sid = UpnpSubscriptionRequest_get_SID_cstr( p_event );

    std::unique_ptr<char> event_xml = build_event_xml( nullptr, nullptr, 0 );
    if( !event_xml )
        msg_Warn( p_intf, "Failed to build event XML" );

    const char *var_keys[1] = { "LastChange" };
    const char *var_values[1] = { event_xml.get() };

    int ret = UpnpAcceptSubscription( p_intf->p_sys->p_upnp->device_handle(),
                                      udn,
                                      service_id,
                                      (const char **) &var_keys,
                                      (const char **) &var_values,
                                      event_xml ? 1 : 0,
                                      sid );
    if( ret != UPNP_E_SUCCESS )
        msg_Dbg( p_intf, "UpnpAcceptSubscription failed" );

    return ret;
}

int
EventHandler::onEvent( Upnp_EventType event_type, UpnpEventPtr p_event,
                       void* p_user_data )
{
    switch( event_type )
    {
    case UPNP_CONTROL_ACTION_REQUEST:
        return onActionRequest( (UpnpActionRequest *) p_event, p_user_data );
    case UPNP_EVENT_SUBSCRIPTION_REQUEST:
        return onSubscriptionRequest( (UpnpSubscriptionRequest *) p_event,
                                      p_user_data );
    default:
        return 0;
    }
}

void
player_state_changed( vlc_player_t *, enum vlc_player_state state, void *data )
{
    intf_thread_t *p_intf = (intf_thread_t *)data;

    const char *new_state;

    switch (state)
    {
    case VLC_PLAYER_STATE_STOPPED:
        new_state = "STOPPED";
        break;
    case VLC_PLAYER_STATE_PLAYING:
        new_state = "PLAYING";
        break;
    case VLC_PLAYER_STATE_PAUSED:
        new_state = "PAUSED_PLAYBACK";
        break;
    case VLC_PLAYER_STATE_STARTED: /* fall through */
    case VLC_PLAYER_STATE_STOPPING:
        new_state = "TRANSITIONING";
        break;
    default:
        new_state = "UNKNOWN";
        break;
    }

    emit_event( p_intf, SRV_AVT, "TransportState", new_state );
}

void
player_rate_changed( vlc_player_t *, float new_rate, void *data )
{
    intf_thread_t *p_intf = (intf_thread_t *)data;

    emit_event( p_intf, SRV_AVT, "TransportPlaySpeed",
                float_to_frac( new_rate ) );
}

void
player_aout_volume_changed( audio_output_t *, float new_volume, void *data )
{
    intf_thread_t *p_intf = (intf_thread_t *) data;

    new_volume = VLC_CLIP(new_volume, 0.0, 1.0);

    /* Volume in range [0, 100] */
    std::string v = std::to_string( std::lround( new_volume * 100 ) );

    emit_event( p_intf, SRV_RC, "Volume", v );
}

void
player_aout_mute_changed( audio_output_t *, bool new_mute, void *data )
{
    intf_thread_t *p_intf = (intf_thread_t *) data;

    std::string m = new_mute ? "1" : "0";

    emit_event( p_intf, SRV_RC, "Mute", m );
}

} // namespace anonymous

namespace DLNA
{

int
OpenControl( vlc_object_t *p_this )
{
    intf_thread_t *p_intf = (intf_thread_t *)p_this;

    std::unique_ptr<intf_sys_t> p_sys = std::make_unique<intf_sys_t>();
    if( !p_sys )
        return VLC_EGENERIC;

    vlc_playlist_t *playlist = vlc_intf_GetMainPlaylist( p_intf );
    vlc_player_t *player = p_sys->player = vlc_playlist_GetPlayer( playlist );

    {
        auto p_upnp = UpnpInstanceWrapper::get( p_this );
        if( !p_upnp )
            return VLC_EGENERIC;

        auto deleter = [](auto *p_upnp) {
            p_upnp->release();
        };
        /* Setup std::unique_ptr with deleter */
        p_sys->p_upnp = upnp_ptr( p_upnp, deleter );
    }

    try
    {
        p_sys->p_listener = std::make_shared<EventHandler>( p_intf );
    }
    catch( const std::bad_alloc& )
    {
        return VLC_EGENERIC;
    }

    p_sys->p_upnp->addListener( p_sys->p_listener );

    /* Start the UPnP MediaRenderer service */
    if( !p_sys->p_upnp->startMediaRenderer( p_this ) )
        return VLC_EGENERIC;

    static struct vlc_player_cbs player_cbs = {};
    player_cbs.on_state_changed = player_state_changed;
    player_cbs.on_rate_changed = player_rate_changed;

    {
        vlc_playlist_Lock( playlist );
        auto p_listener_id =
            vlc_player_AddListener( player, &player_cbs, p_intf );
        vlc_playlist_Unlock( playlist );
        if( !p_listener_id )
            return VLC_EGENERIC;

        auto deleter = [playlist, player](auto *p_listener_id) {
            vlc_playlist_Lock( playlist );
            vlc_player_RemoveListener( player, p_listener_id );
            vlc_playlist_Unlock( playlist );
        };
        /* Setup std::unique_ptr with custom deleter */
        p_sys->p_player_listener =
            player_listener_ptr(p_listener_id, deleter);
    }

    static struct vlc_player_aout_cbs player_aout_cbs = {};
    player_aout_cbs.on_volume_changed = player_aout_volume_changed;
    player_aout_cbs.on_mute_changed = player_aout_mute_changed;

    {
        vlc_playlist_Lock( playlist );
        auto p_listener_id =
            vlc_player_aout_AddListener( player, &player_aout_cbs, p_intf );
        vlc_playlist_Unlock( playlist );
        if( !p_listener_id )
            return VLC_EGENERIC;

        auto deleter = [playlist, player](auto *p_listener_id) {
            vlc_playlist_Lock( playlist );
            vlc_player_aout_RemoveListener( player, p_listener_id );
            vlc_playlist_Unlock( playlist );
        };
        /* Setup std::unique_ptr with custom deleter */
        p_sys->p_player_aout_listener =
            player_aout_listener_ptr(p_listener_id, deleter);
    }

    p_intf->p_sys = p_sys.release();

    msg_Info( p_this, "Started MediaRenderer service" );

    return VLC_SUCCESS;
}

void
CloseControl( vlc_object_t *p_this )
{
    intf_thread_t *p_intf = (intf_thread_t *)p_this;

    intf_sys_t *p_sys = p_intf->p_sys;

    p_sys->p_upnp->removeListener( p_sys->p_listener );

    /* Stop the UPnP MediaRenderer service */
    p_sys->p_upnp->stopMediaRenderer( p_this );

    delete p_sys;

    msg_Info( p_this, "Stopped MediaRenderer service" );
}

} // namespace DLNA
