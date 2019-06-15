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

#include "../services_discovery/upnp-wrapper.hpp"

#include <vlc_plugin.h>
#include <vlc_interface.h>

#include "dlna.hpp"

struct intf_sys_t
{
    UpnpInstanceWrapper *p_upnp;
    std::shared_ptr<DLNA::EventHandler> p_listener;
};

typedef std::map<std::string,std::string> parammap;

typedef int (*ActionRequestHandler)(UpnpActionRequest*, parammap,
                                    intf_thread_t*);

namespace DLNA
{

static int
handle_AVT_SetAVTransportURI( UpnpActionRequest *p_req, parammap params,
                              intf_thread_t *p_intf )
{
    std::string s = params["CurrentURI"];
    printf("%s %s\n", __func__, s.c_str());

    // TODO: get data, get player, set uri in player
    return 0;
}

static int
handle_AVT_Stop( UpnpActionRequest *p_req, parammap params,
                 intf_thread_t *p_intf )
{
    printf("%s\n", __func__);
    return 0;
}

static int
handle_AVT_Play( UpnpActionRequest *p_req, parammap params,
                 intf_thread_t *p_intf )
{
    printf("%s\n", __func__);
    return 0;
}

static int
handle_AVT_Pause( UpnpActionRequest *p_req, parammap params,
                  intf_thread_t *p_intf )
{
    printf("%s\n", __func__);
    return 0;
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
    /*
    { SRV_AVT, "SetNextAVTransportURI", handle_AVT_SetNextAVTransportURI },
    { SRV_AVT, "GetMediaInfo", handle_AVT_GetMediaInfo },
    { SRV_AVT, "GetTransportInfo", handle_AVT_GetTransportInfo },
    { SRV_AVT, "GetPositionInfo", handle_AVT_GetPositionInfo },
    { SRV_AVT, "GetDeviceCapabilities", handle_AVT_GetDeviceCapabilities },
    { SRV_AVT, "GetTransportSettings", handle_AVT_GetTransportSettings },
    */
    { SRV_AVT, "Stop", handle_AVT_Stop },
    { SRV_AVT, "Play", handle_AVT_Play },
    { SRV_AVT, "Pause", handle_AVT_Pause },
    /*
    { SRV_AVT, "Seek", handle_AVT_Seek },
    { SRV_AVT, "Previous", handle_AVT_Previous },
    { SRV_AVT, "SetPlayMode", handle_AVT_SetPlayMode },
    { SRV_AVT, "GetCurrentTransport", handle_AVT_GetCurrentTransport },
    */
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

int EventHandler::onActionRequest( UpnpActionRequest *p_event,
                                   void *p_user_data )
{
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
        for( size_t i = 0; actions[i].handler; i++ )
        {
            if( !strcmp( actions[i].service, service_id ) &&
                !strcmp( actions[i].action, action_name ) )
            {
                //printf("%s\n", xml_getChildElementValue(action, "CurrentURI"));

                return actions[i].handler( p_event,
                                           build_param_map( action ),
                                           p_intf );
            }
        }
    }

    // TODO: return "not implemented"

    return 0;
}

int EventHandler::onGetVarRequest( UpnpStateVarRequest *p_event,
                                   void *p_user_data )
{
#if 0
    /* For example urn:upnp-org:serviceId:AVTransport */
    char *service_id = UpnpStateVarRequest_get_ServiceID_cstr( p_event );

    /* For example ... */
    char *state_var_name = UpnpStateVarRequest_get_StateVarName_cstr( p_event );

    printf("%s %s %s %s\n", __func__, service_id, state_var_name, p_event->CurrentVal);

    p_event->CurrentVal = strdup("HELLLLLO");
    p_event->ErrCode = UPNP_E_SUCCESS;
#endif

    return 0;
}

int EventHandler::onSubscriptionRequest( UpnpSubscriptionRequest *p_event,
                                         void *p_user_data )
{
    /* For example urn:upnp-org:serviceId:AVTransport */
    char *service_id = UpnpSubscriptionRequest_get_ServiceId_cstr( p_event );

    /* For example ... */
    char *udn = UpnpSubscriptionRequest_get_UDN_cstr( p_event );

    // TODO: look up real eventable state variables
    const char *varnames[1] = { "LastChange" };
    const char *newval[1] = { "test" };

    int ret = UpnpAcceptSubscription( p_intf->p_sys->p_upnp->device_handle(),
                                      udn,
                                      service_id,
                                      (const char **) &varnames,
                                      (const char **) &newval,
                                      1,
                                      p_event->Sid );
    if ( ret != UPNP_E_SUCCESS )
    {
        printf("UpnpAcceptSubscription failed\n");
    }

    return 0;
}

int EventHandler::onEvent( Upnp_EventType event_type,
                           UpnpEventPtr p_event,
                           void* p_user_data )
{
    switch( event_type )
    {
    /* Control */
    case UPNP_CONTROL_ACTION_REQUEST:
        return onActionRequest( (UpnpActionRequest *) p_event, p_user_data );
    case UPNP_CONTROL_GET_VAR_REQUEST:
        return onGetVarRequest( (UpnpStateVarRequest *) p_event, p_user_data );
#if 0
    /* Discovery */
    case UPNP_DISCOVERY_ADVERTISEMENT_ALIVE:
        printf("UPNP_DISCOVERY_ADVERTISEMENT_ALIVE\n");
        break;
    case UPNP_DISCOVERY_ADVERTISEMENT_BYEBYE:
        printf("UPNP_DISCOVERY_ADVERTISEMENT_BYEBYE\n");
        break;
#endif
    /* Eventing */
    case UPNP_EVENT_SUBSCRIPTION_REQUEST:
        return onSubscriptionRequest( (UpnpSubscriptionRequest *) p_event,
                                      p_user_data );
        break;
    default:
        printf("got unknown event %d\n", event_type);
        break;
    }

    return 0;
}

int OpenControl( vlc_object_t *p_this )
{
    intf_thread_t *p_intf = (intf_thread_t *)p_this;

    p_intf->p_sys = (intf_sys_t *)malloc( sizeof( intf_sys_t ) );
    if( unlikely(p_intf->p_sys == NULL) )
        return VLC_ENOMEM;

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

    msg_Info( p_this, "Started MediaRenderer service");

    return VLC_SUCCESS;

error2:
    p_intf->p_sys->p_upnp->release();

error1:
    free( p_intf->p_sys );

    return VLC_EGENERIC;
}

void CloseControl( vlc_object_t *p_this )
{
    intf_thread_t *p_intf = (intf_thread_t *)p_this;

    /* Stop the UPnP MediaRenderer service */
    p_intf->p_sys->p_upnp->stopMediaRenderer( p_this );

    p_intf->p_sys->p_upnp->release();

    free( p_intf->p_sys );

    msg_Info( p_this, "Stopped MediaRenderer service");
}

} // namespace DLNA
