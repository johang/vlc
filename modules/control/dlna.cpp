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

#include "../services_discovery/upnp-wrapper.hpp"

#include <vlc_plugin.h>
#include <vlc_interface.h>

#include "dlna.hpp"

struct intf_sys_t
{
    UpnpInstanceWrapper *p_upnp;
    std::shared_ptr<DLNA::EventHandler> p_listener;
};

namespace DLNA
{

int EventHandler::onEvent( Upnp_EventType event_type,
                           UpnpEventPtr p_event,
                           void* p_user_data )
{
    printf("got event\n");

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
    {
        goto error1;
    }

    /* Start the UPnP MediaRenderer service */
    p_intf->p_sys->p_upnp->startMediaRenderer( p_this );

    try
    {
        p_intf->p_sys->p_listener = std::make_shared<DLNA::EventHandler>();
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
