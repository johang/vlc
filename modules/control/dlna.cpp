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

#include <string>
#include <map>
#include <iostream>
#include <regex>

#include "../services_discovery/upnp-wrapper.hpp"

#include <vlc_plugin.h>
#include <vlc_interface.h>
#include <vlc_playlist.h>
#include <vlc_player.h>
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

struct intf_sys_t
{
    UpnpInstanceWrapper *p_upnp;
    std::shared_ptr<DLNA::EventHandler> p_listener;
    vlc_playlist_t *playlist;
    vlc_player_t *player;
    vlc_player_listener_id *p_player_listener;
    vlc_player_aout_listener_id *p_player_aout_listener;
};

typedef std::map<std::string,std::string> parammap;

typedef bool (*ActionRequestHandler)( parammap&, parammap&, intf_thread_t* );

namespace DLNA
{

static bool
handle_AVT_SetAVTransportURI( parammap& in_params, parammap& out_params,
                              intf_thread_t *p_intf )
{
    VLC_UNUSED(out_params);

    std::string s = in_params["CurrentURI"];

    input_item_t *item = input_item_New(s.c_str(), NULL);
    if( !item )
    {
        printf("failed to parse URL?");
        return -1;
    }

    vlc_player_t *player = vlc_playlist_GetPlayer( p_intf->p_sys->playlist );

    vlc_player_Lock( player );
    vlc_player_SetCurrentMedia( player, item );
    vlc_player_Start( player );
    vlc_player_Unlock( player );

    input_item_Release( item );

    return true;
}

static bool
handle_AVT_GetMediaInfo( parammap& in_params, parammap& out_params,
                         intf_thread_t *p_intf )
{
    VLC_UNUSED(in_params);
    VLC_UNUSED(p_intf);

    out_params["MediaDuration"] = "00:00";

    return true;
}


static bool
handle_AVT_GetTransportInfo( parammap& in_params, parammap& out_params,
                             intf_thread_t *p_intf )
{
    VLC_UNUSED(in_params);

    vlc_player_t *player = vlc_playlist_GetPlayer( p_intf->p_sys->playlist );

    vlc_player_Lock( player );
    enum vlc_player_state state = vlc_player_GetState( player );
    vlc_player_Unlock( player );

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

    out_params["CurrentTransportStatus"] = "";
    out_params["CurrentSpeed"] = "";

    return true;
}

static bool
handle_AVT_Stop( parammap& in_params, parammap& out_params,
                 intf_thread_t *p_intf )
{
    VLC_UNUSED(in_params);
    VLC_UNUSED(out_params);

    vlc_player_t *player = vlc_playlist_GetPlayer( p_intf->p_sys->playlist );

    vlc_player_Lock( player );
    vlc_player_Stop( player );
    vlc_player_Unlock( player );

    return true;
}

static bool
handle_AVT_Play( parammap& in_params, parammap& out_params,
                 intf_thread_t *p_intf )
{
    VLC_UNUSED(in_params);
    VLC_UNUSED(out_params);

    vlc_player_t *player = vlc_playlist_GetPlayer( p_intf->p_sys->playlist );

    vlc_player_Lock( player );
    vlc_player_Resume( player );
    vlc_player_Unlock( player );

    return true;
}

static bool
handle_AVT_Pause( parammap& in_params, parammap& out_params,
                  intf_thread_t *p_intf )
{
    VLC_UNUSED(in_params);
    VLC_UNUSED(out_params);

    vlc_player_t *player = vlc_playlist_GetPlayer( p_intf->p_sys->playlist );

    vlc_player_Lock( player );
    vlc_player_Pause( player );
    vlc_player_Unlock( player );

    return true;
}

static bool
handle_CM_GetProtocolInfo( parammap& in_params, parammap& out_params,
                           intf_thread_t *p_intf )
{
    VLC_UNUSED(p_intf);
    VLC_UNUSED(in_params);

    out_params["Source"] = "";
    out_params["Sink"] = SINK_PROTOCOL_INFO;

    return true;
}

static bool
handle_RC_GetVolume( parammap& in_params, parammap& out_params,
                     intf_thread_t *p_intf )
{
    VLC_UNUSED(in_params);

    /* Volume as in range [0.0, 2.0] or -1.0 if no audio */
    float volume = vlc_player_aout_GetVolume( p_intf->p_sys->player );
    if( volume < 0.0 )
        volume = 0.0;
    else if( volume > 2.0 )
        volume = 2.0;

    std::string v = std::to_string( std::round( volume * 50 ) );

    out_params["CurrentVolume"] = v;

    return true;
}

static bool
handle_RC_SetVolume( parammap& in_params, parammap& out_params,
                     intf_thread_t *p_intf )
{
    VLC_UNUSED(out_params);

    /* Volume in range [0, 100] */
    unsigned long volume = std::stoul( in_params["DesiredVolume"] );
    if( volume > 100 )
        volume = 100;

    int ret = vlc_player_aout_SetVolume( p_intf->p_sys->player, volume / 50.0 );
    if ( ret != UPNP_E_SUCCESS )
    {
    }

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
    { SRV_AVT, "Stop", handle_AVT_Stop },
    { SRV_AVT, "Play", handle_AVT_Play },
    { SRV_AVT, "Pause", handle_AVT_Pause },
    { SRV_CM, "GetProtocolInfo", handle_CM_GetProtocolInfo },
    { SRV_RC, "GetVolume", handle_RC_GetVolume },
    { SRV_RC, "SetVolume", handle_RC_SetVolume },
    { NULL, NULL, NULL }
};

static parammap
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

        params[std::string(key)] = std::string(value);
    }

    return params;
}

static std::string 
build_event_xml( const char **keys, const char **values, int count )
{
    IXML_Document *doc = NULL;
    IXML_Element *event = NULL;
    IXML_Element *instance = NULL;
    DOMString xmlcstr = NULL;
    std::string xmlstr;

    doc = ixmlDocument_createDocument();
    if( !doc )
        return xmlstr;

    event = ixmlDocument_createElement( doc, "Event" );
    if( !event )
        goto err1;

    if( ixmlNode_appendChild( (IXML_Node *)doc,
                              (IXML_Node *)event ) != IXML_SUCCESS )
    {
        ixmlElement_free( event );
        goto err1;
    }

    instance = ixmlDocument_createElement( doc, "InstanceID" );
    if( !instance )
        goto err1;

    if( ixmlNode_appendChild( (IXML_Node *)event,
                              (IXML_Node *)instance ) != IXML_SUCCESS )
    {
        ixmlElement_free( instance );
        goto err1;
    }

    if( ixmlElement_setAttribute( instance, "val", "0") != IXML_SUCCESS )
        goto err1;

    for( int i = 0; i < count; i++ )
    {
        IXML_Element *arg = ixmlDocument_createElement( doc, keys[i] );
        if( !arg )
            goto err1;

        if( ixmlNode_appendChild( (IXML_Node *)instance,
                                  (IXML_Node *)arg ) != IXML_SUCCESS )
        {
            ixmlElement_free( arg );
            goto err1;
        }

        if( ixmlElement_setAttribute( arg, "val", values[i]) != IXML_SUCCESS )
            goto err1;
    }

    xmlcstr = ixmlNodetoString( (IXML_Node *)doc );
    if( !xmlcstr )
        goto err1;

    xmlstr = std::string(xmlcstr);

    free( xmlcstr );

    ixmlDocument_free( doc );

    xmlstr = std::regex_replace(xmlstr, std::regex("&"), "&amp;");
    xmlstr = std::regex_replace(xmlstr, std::regex("\""), "&quot;");
    xmlstr = std::regex_replace(xmlstr, std::regex("\'"), "&apos;");
    xmlstr = std::regex_replace(xmlstr, std::regex("<"), "&lt;");
    xmlstr = std::regex_replace(xmlstr, std::regex(">"), "&gt;");

    return xmlstr;

err1:
    ixmlDocument_free( doc );

    return xmlstr;
}

int EventHandler::onActionRequest( UpnpActionRequest *p_event,
                                   void *p_user_data )
{
    (void) p_user_data;

    /* For example urn:upnp-org:serviceId:AVTransport */
    char *service_id = UpnpActionRequest_get_ServiceID_cstr( p_event );

    /* For example SetAVTransportURI */
    char *action_name = UpnpActionRequest_get_ActionName_cstr( p_event );

    /* "Body" XML node in the request */
    IXML_Document *body = UpnpActionRequest_get_ActionRequest( p_event );
    if( !body )
        return 0;

    for( IXML_Node *action = ixmlNode_getFirstChild( (IXML_Node*) body );
         action != NULL;
         action = ixmlNode_getNextSibling( action ) )
    {
        parammap in_params = build_param_map( action );

        for( size_t i = 0; actions[i].handler; i++ )
        {
            if( strcmp( actions[i].service, service_id ) ||
                strcmp( actions[i].action, action_name ) )
                continue;

            parammap out_params;

            p_event->ErrCode = UPNP_E_SUCCESS;
            p_event->ActionResult = NULL;

            int r = actions[i].handler( in_params, out_params, p_intf );
            if( !r )
                continue;

            p_event->ActionResult = UpnpMakeActionResponse( action_name,
                                                            service_id,
                                                            0,
                                                            NULL );
            if( !p_event->ActionResult )
                continue;

            for( auto& x : out_params )
            {
                int r = UpnpAddToActionResponse( &p_event->ActionResult,
                                                 action_name,
                                                 service_id,
                                                 x.first.c_str(),
                                                 x.second.c_str() );
                if( r != UPNP_E_SUCCESS )
                {
                    if( p_event->ActionResult != NULL )
                        ixmlDocument_free( p_event->ActionResult );
                    p_event->ActionResult = NULL;

                    return r;
                }
            }

            return UPNP_E_SUCCESS;
        }
    }

    // TODO: return "not implemented"

    return UPNP_E_INTERNAL_ERROR;
}

int EventHandler::onGetVarRequest( UpnpStateVarRequest *p_event,
                                   void *p_user_data )
{
    (void) p_event;
    (void) p_user_data;

    // TODO

    return -124;
}

int EventHandler::onSubscriptionRequest( UpnpSubscriptionRequest *p_event,
                                         void *p_user_data )
{
    (void) p_user_data;

    /* For example urn:upnp-org:serviceId:AVTransport */
    char *service_id = UpnpSubscriptionRequest_get_ServiceId_cstr( p_event );

    /* For example ... */
    char *udn = UpnpSubscriptionRequest_get_UDN_cstr( p_event );

    std::string event_xml = build_event_xml( NULL, NULL, 0 );
std::cout << event_xml << std::endl;

    const char *var_keys[1] = { "LastChange" };
    const char *var_values[1] = { event_xml.c_str() };

    int ret = UpnpAcceptSubscription( p_intf->p_sys->p_upnp->device_handle(),
                                      udn,
                                      service_id,
                                      (const char **) &var_keys,
                                      (const char **) &var_values,
                                      1,
                                      p_event->Sid );
    if( ret != UPNP_E_SUCCESS )
    {
        printf("UpnpAcceptSubscription failed\n");
    }

    return ret;
}

int EventHandler::onEvent( Upnp_EventType event_type,
                           UpnpEventPtr p_event,
                           void* p_user_data )
{
    switch( event_type )
    {
    case UPNP_CONTROL_ACTION_REQUEST:
        return onActionRequest( (UpnpActionRequest *) p_event, p_user_data );
    case UPNP_CONTROL_GET_VAR_REQUEST:
        return onGetVarRequest( (UpnpStateVarRequest *) p_event, p_user_data );
    case UPNP_EVENT_SUBSCRIPTION_REQUEST:
        return onSubscriptionRequest( (UpnpSubscriptionRequest *) p_event,
                                      p_user_data );
    default:
        break;
    }

    return -123;
}

static void
player_state_changed( vlc_player_t *player, enum vlc_player_state state,
                      void *data )
{
    (void) player;

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

    const char *event_keys[1] = { "TransportState" };
    const char *event_values[1] = { new_state };

    std::string event_xml = build_event_xml( event_keys, event_values, 1 );
std::cout << event_xml << std::endl;

    const char *var_keys[1] = { "LastChange" };
    const char *var_values[1] = { event_xml.c_str() };

    int ret = UpnpNotify( p_intf->p_sys->p_upnp->device_handle(),
                          UPNP_UDN,
                          SRV_AVT,
                          (const char **) &var_keys,
                          (const char **) &var_values,
                          1 );
    if ( ret != UPNP_E_SUCCESS )
    {
        printf("UpnpNotify failed\n");
    }
}

static void
player_aout_volume_changed( vlc_player_t *player, float new_volume,
                            void *data )
{
    (void) player;

    intf_thread_t *p_intf = (intf_thread_t *) data;

    if( new_volume < 0.0 )
        new_volume = 0.0;
    else if( new_volume > 2.0 )
        new_volume = 2.0;

    /* Volume in range [0, 100] */
    std::string v = std::to_string( std::round( new_volume * 50 ) );

    const char *event_keys[1] = { "Volume" };
    const char *event_values[1] = { v.c_str() };

    std::string event_xml = build_event_xml( event_keys, event_values, 1 );
std::cout << event_xml << std::endl;

    const char *var_keys[1] = { "LastChange" };
    const char *var_values[1] = { event_xml.c_str() };

    int ret = UpnpNotify( p_intf->p_sys->p_upnp->device_handle(),
                          UPNP_UDN,
                          SRV_RC,
                          (const char **) &var_keys,
                          (const char **) &var_values,
                          1 );
    if ( ret != UPNP_E_SUCCESS )
    {
        printf("UpnpNotify failed\n");
    }
}

int OpenControl( vlc_object_t *p_this )
{
    intf_thread_t *p_intf = (intf_thread_t *)p_this;

    p_intf->p_sys = (intf_sys_t *)calloc ( 1, sizeof( intf_sys_t ) );
    if( unlikely(p_intf->p_sys == NULL) )
        return VLC_ENOMEM;

    p_intf->p_sys->playlist = vlc_intf_GetMainPlaylist( p_intf );
    if( !p_intf->p_sys->playlist )
        goto error1;

    p_intf->p_sys->p_upnp = UpnpInstanceWrapper::get( p_this );
    if( !p_intf->p_sys->p_upnp )
        goto error1;

    /* Start the UPnP MediaRenderer service */
    p_intf->p_sys->p_upnp->startMediaRenderer( p_this );
    try
    {
        p_intf->p_sys->p_listener =
            std::make_shared<DLNA::EventHandler>( p_intf );
    }
    catch ( const std::bad_alloc& )
    {
        msg_Err( p_this, "Failed to alloc");
        goto error2;
    }

    p_intf->p_sys->p_upnp->addListener( p_intf->p_sys->p_listener );

    static struct vlc_player_cbs player_cbs = {};
    player_cbs.on_state_changed = player_state_changed;

    static struct vlc_player_aout_cbs player_aout_cbs = {};
    player_aout_cbs.on_volume_changed = player_aout_volume_changed;

    p_intf->p_sys->player = vlc_playlist_GetPlayer( p_intf->p_sys->playlist );
    if( !p_intf->p_sys->player )
        goto error2;

    vlc_playlist_Lock( p_intf->p_sys->playlist );

    p_intf->p_sys->p_player_listener =
        vlc_player_AddListener( p_intf->p_sys->player, &player_cbs, p_intf );
    if ( !p_intf->p_sys->p_player_listener )
        goto error3;

    p_intf->p_sys->p_player_aout_listener =
        vlc_player_aout_AddListener( p_intf->p_sys->player, &player_aout_cbs,
                                     p_intf );
    if ( !p_intf->p_sys->p_player_aout_listener )
        goto error4;

    vlc_playlist_Unlock( p_intf->p_sys->playlist );

    msg_Info( p_this, "Started MediaRenderer service");

    return VLC_SUCCESS;

error4:
    vlc_playlist_Lock( p_intf->p_sys->playlist );
    vlc_player_RemoveListener( p_intf->p_sys->player,
                               p_intf->p_sys->p_player_listener );
    vlc_playlist_Unlock( p_intf->p_sys->playlist );

error3:
    p_intf->p_sys->p_upnp->removeListener( p_intf->p_sys->p_listener );

error2:
    p_intf->p_sys->p_upnp->release();

error1:
    free( p_intf->p_sys );

    return VLC_EGENERIC;
}

void CloseControl( vlc_object_t *p_this )
{
    intf_thread_t *p_intf = (intf_thread_t *)p_this;

    /* Remove player listeners */
    vlc_playlist_Lock( p_intf->p_sys->playlist );
    vlc_player_aout_RemoveListener( p_intf->p_sys->player,
                                    p_intf->p_sys->p_player_aout_listener );
    vlc_player_RemoveListener( p_intf->p_sys->player,
                               p_intf->p_sys->p_player_listener );
    vlc_playlist_Unlock( p_intf->p_sys->playlist );

    /* Remove UPnP listener */
    p_intf->p_sys->p_upnp->removeListener( p_intf->p_sys->p_listener );

    /* Stop the UPnP MediaRenderer service */
    p_intf->p_sys->p_upnp->stopMediaRenderer( p_this );

    p_intf->p_sys->p_upnp->release();

    free( p_intf->p_sys );

    msg_Info( p_this, "Stopped MediaRenderer service");
}

} // namespace DLNA
