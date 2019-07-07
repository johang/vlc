/*****************************************************************************
 * dlna.hpp :  DLNA MediaRenderer module
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

#ifndef CONTROL_DLNA_H
#define CONTROL_DLNA_H

#include "../services_discovery/upnp-wrapper.hpp"

#include <vlc_plugin.h>
#include <vlc_interface.h>

#if UPNP_VERSION < 10623
/* For compatibility with libupnp older than 1.6.23 */
typedef struct Upnp_Action_Request UpnpActionRequest;
#define UpnpActionRequest_get_ActionName_cstr(x) ((x)->ActionName)
#define UpnpActionRequest_get_ServiceID_cstr(x) ((x)->ServiceID)
#define UpnpActionRequest_get_DevUDN_cstr(x) ((x)->DevUDN)
#define UpnpActionRequest_get_ActionRequest(x) ((x)->ActionRequest)
typedef struct Upnp_State_Var_Request UpnpStateVarRequest;
#define UpnpStateVarRequest_get_StateVarName_cstr(x) ((x)->StateVarName)
#define UpnpStateVarRequest_get_ServiceID_cstr(x) ((x)->ServiceID)
#define UpnpStateVarRequest_get_DevUDN_cstr(x) ((x)->DevUDN)
typedef struct Upnp_Subscription_Request UpnpSubscriptionRequest;
#define UpnpSubscriptionRequest_get_ServiceId_cstr(x) ((x)->ServiceId)
#define UpnpSubscriptionRequest_get_SID_cstr(x) ((x)->Sid)
#define UpnpSubscriptionRequest_get_UDN_cstr(x) ((x)->UDN)
#endif

#if UPNP_VERSION < 10800
/* For compatibility with libupnp older than 1.8.0 */
#define UpnpActionRequest_set_ErrCode(x, y) ((x)->ErrCode = (y))
#define UpnpActionRequest_set_ErrStr_cstr(x, y) ((x)->ErrStr = (y))
#define UpnpActionRequest_set_ActionResult(x, y) ((x)->ActionResult = (y))
#endif

namespace
{

class EventHandler : public UpnpInstanceWrapper::Listener
{
public:
    EventHandler( intf_thread_t *_p_intf )
        : p_intf( _p_intf )
    {
    }

    int onEvent( Upnp_EventType event_type,
                 UpnpEventPtr p_event,
                 void *p_user_data ) override;

private:
    intf_thread_t *p_intf;

    int onActionRequest( UpnpActionRequest *p_event,
                         void *p_user_data );

    int onGetVarRequest( UpnpStateVarRequest *p_event,
                         void *p_user_data );

    int onSubscriptionRequest( UpnpSubscriptionRequest *p_event,
                               void *p_user_data );
};

} // namespace anonymous

namespace DLNA
{

int OpenControl( vlc_object_t *p_this );
void CloseControl( vlc_object_t *p_this );

} // namespace DLNA

#endif
