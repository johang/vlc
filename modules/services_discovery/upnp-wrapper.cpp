/*****************************************************************************
 * upnp-wrapper.cpp :  UPnP Instance Wrapper class
 *****************************************************************************
 * Copyright © 2004-2018 VLC authors and VideoLAN
 *
 * Authors: Rémi Denis-Courmont <rem # videolan.org> (original plugin)
 *          Christian Henz <henz # c-lab.de>
 *          Mirsal Ennaime <mirsal dot ennaime at gmail dot com>
 *          Hugo Beauzée-Luyssen <hugo@beauzee.fr>
 *          Shaleen Jain <shaleen@jain.sh>
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

#ifdef _WIN32
#include <windows.h>
#include <wincrypt.h>
#endif

#include "upnp-wrapper.hpp"
#include <vlc_cxx_helpers.hpp>

static const char *mediarenderer_desc =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
    "<root xmlns=\"urn:schemas-upnp-org:device-1-0\">"
      "<specVersion>"
        "<major>1</major>"
        "<minor>0</minor>"
      "</specVersion>"
      "<device>"
        "<deviceType>urn:schemas-upnp-org:device:MediaServer:1</deviceType>"
        "<friendlyName>VLC media player</friendlyName>" /* TODO: include hostname */
        "<manufacturer>VideoLAN</manufacturer>"
        "<modelName>" PACKAGE_NAME "</modelName>"
        "<modelNumber>" PACKAGE_VERSION "</modelNumber>"
        "<modelURL>https://www.videolan.org/vlc/</modelURL>"
        "<UDN>uuid:034fc8dc-ec22-44e5-a79b-38c935f11663</UDN>" /* TODO: generate at each startup */
        "<serviceList>"
          "<service>"
            "<serviceType>urn:schemas-upnp-org:service:RenderingControl:1</serviceType>"
            "<serviceId>urn:upnp-org:serviceId:RenderingControl</serviceId>"
            "<SCPDURL>/RenderingControlSCPD.xml</SCPDURL>"
            "<controlURL>/upnp/control/RenderingControl</controlURL>"
            "<eventSubURL>/upnp/event/RenderingControl</eventSubURL>"
          "</service>"
          "<service>"
            "<serviceType>urn:schemas-upnp-org:service:ConnectionManager:1</serviceType>"
            "<serviceId>urn:upnp-org:serviceId:ConnectionManager</serviceId>"
            "<SCPDURL>/ConnectionManagerSCPD.xml</SCPDURL>"
            "<controlURL>/upnp/control/ConnectionManager</controlURL>"
            "<eventSubURL>/upnp/event/ConnectionManager</eventSubURL>"
          "</service>"
          "<service>"
            "<serviceType>urn:schemas-upnp-org:service:AVTransport:1</serviceType>"
            "<serviceId>urn:upnp-org:serviceId:AVTransport</serviceId>"
            "<SCPDURL>/AVTransportSCPD.xml</SCPDURL>"
            "<controlURL>/upnp/control/AVTransport</controlURL>"
            "<eventSubURL>/upnp/event/AVTransport</eventSubURL>"
          "</service>"
        "</serviceList>"
      "</device>"
    "</root>";

UpnpInstanceWrapper* UpnpInstanceWrapper::s_instance;
UpnpInstanceWrapper::Listeners UpnpInstanceWrapper::s_listeners;
vlc_mutex_t UpnpInstanceWrapper::s_lock = VLC_STATIC_MUTEX;

UpnpInstanceWrapper::UpnpInstanceWrapper()
    : m_client_handle( -1 )
    , m_device_handle( -1 )
    , m_refcount( 0 )
    , m_mediarenderer_refcount( 0 )
{
}

UpnpInstanceWrapper::~UpnpInstanceWrapper()
{
    if( m_client_handle > 0 )
        UpnpUnRegisterClient( m_client_handle );
    if( m_device_handle > 0 )
        UpnpUnRegisterRootDevice( m_device_handle );
    UpnpFinish();
}

UpnpInstanceWrapper *UpnpInstanceWrapper::get(vlc_object_t *p_obj)
{
    vlc_mutex_locker lock( &s_lock );
    if ( s_instance == NULL )
    {
        UpnpInstanceWrapper* instance = new(std::nothrow) UpnpInstanceWrapper;
        if ( unlikely( !instance ) )
        {
            return NULL;
        }

    #ifdef UPNP_ENABLE_IPV6
        char* psz_miface = var_InheritString( p_obj, "miface" );
        if (psz_miface == NULL)
            psz_miface = getPreferedAdapter();
        msg_Info( p_obj, "Initializing libupnp on '%s' interface", psz_miface ? psz_miface : "default" );
        int i_res = UpnpInit2( psz_miface, 0 );
        free( psz_miface );
    #else
        /* If UpnpInit2 isnt available, initialize on first IPv4-capable interface */
        char *psz_hostip = getIpv4ForMulticast();
        int i_res = UpnpInit( psz_hostip, 0 );
        free(psz_hostip);
    #endif /* UPNP_ENABLE_IPV6 */
        if( i_res != UPNP_E_SUCCESS )
        {
            msg_Err( p_obj, "Initialization failed: %s", UpnpGetErrorMessage( i_res ) );
            delete instance;
            return NULL;
        }

        ixmlRelaxParser( 1 );

        /* Register a control point */
        i_res = UpnpRegisterClient( Callback, NULL, &instance->m_client_handle );
        if( i_res != UPNP_E_SUCCESS )
        {
            msg_Err( p_obj, "Client registration failed: %s", UpnpGetErrorMessage( i_res ) );
            delete instance;
            return NULL;
        }

        /* libupnp does not treat a maximum content length of 0 as unlimited
         * until 64dedf (~ pupnp v1.6.7) and provides no sane way to discriminate
         * between versions */
        if( (i_res = UpnpSetMaxContentLength( INT_MAX )) != UPNP_E_SUCCESS )
        {
            msg_Err( p_obj, "Failed to set maximum content length: %s",
                    UpnpGetErrorMessage( i_res ));
            delete instance;
            return NULL;
        }

        char *root = config_GetSysPath( VLC_PKG_DATA_DIR, "upnp" );
        if( (i_res = UpnpSetWebServerRootDir( root )) != UPNP_E_SUCCESS)
        {
            msg_Warn( p_obj, "UpnpSetWebServerRootDir failed: %s",
                      UpnpGetErrorMessage( i_res ));
        }
        free( root );

        s_instance = instance;
    }
    s_instance->m_refcount++;
    return s_instance;
}

void UpnpInstanceWrapper::release()
{
    UpnpInstanceWrapper *p_delete = NULL;
    vlc_mutex_lock( &s_lock );
    if (--s_instance->m_refcount == 0)
    {
        p_delete = s_instance;
        s_instance = NULL;
    }
    vlc_mutex_unlock( &s_lock );
    delete p_delete;
}

UpnpClient_Handle UpnpInstanceWrapper::client_handle() const
{
    return m_client_handle;
}

UpnpDevice_Handle UpnpInstanceWrapper::device_handle() const
{
    return m_device_handle;
}

int UpnpInstanceWrapper::Callback(Upnp_EventType event_type, UpnpEventPtr p_event, void *p_user_data)
{
    vlc::threads::mutex_locker lock( &s_lock );
    for (Listeners::iterator iter = s_listeners.begin(); iter != s_listeners.end(); ++iter)
    {
        (*iter)->onEvent(event_type, p_event, p_user_data);
    }

    return 0;
}

void UpnpInstanceWrapper::addListener(ListenerPtr listener)
{
    vlc::threads::mutex_locker lock( &s_lock );
    if ( std::find( s_listeners.begin(), s_listeners.end(), listener) == s_listeners.end() )
        s_listeners.push_back( std::move(listener) );
}

void UpnpInstanceWrapper::removeListener(ListenerPtr listener)
{
    vlc::threads::mutex_locker lock( &s_lock );
    Listeners::iterator iter = std::find( s_listeners.begin(), s_listeners.end(), listener );
    if ( iter != s_listeners.end() )
        s_listeners.erase( iter );
}

void UpnpInstanceWrapper::startMediaRenderer( vlc_object_t *p_obj )
{
    vlc::threads::mutex_locker lock( &s_lock );
    if( m_mediarenderer_refcount == 0 )
    {
        int i_res;
        if( (i_res = UpnpEnableWebserver( TRUE )) != UPNP_E_SUCCESS)
        {
            msg_Err( p_obj, "Failed to enable webserver: %s", UpnpGetErrorMessage( i_res ) );
            return;
        }
        i_res = UpnpRegisterRootDevice2( UPNPREG_BUF_DESC,
                                         mediarenderer_desc,
                                         strlen(mediarenderer_desc),
                                         1,
                                         Callback,
                                         NULL,
                                         &m_device_handle );
        if( i_res != UPNP_E_SUCCESS )
        {
            msg_Err( p_obj, "Device registration failed: %s", UpnpGetErrorMessage( i_res ) );
        }
    }
    m_mediarenderer_refcount++;
}

void UpnpInstanceWrapper::stopMediaRenderer( vlc_object_t *p_obj )
{
    vlc::threads::mutex_locker lock( &s_lock );
    m_mediarenderer_refcount--;
    if( m_mediarenderer_refcount == 0 )
    {
        int i_res;
        i_res = UpnpUnRegisterRootDevice( m_device_handle );
        if( i_res != UPNP_E_SUCCESS )
        {
            msg_Err( p_obj, "Device unregistration failed: %s", UpnpGetErrorMessage( i_res ) );
        }
        m_device_handle = -1;
        i_res = UpnpEnableWebserver( FALSE );
        if( i_res != UPNP_E_SUCCESS )
        {
            msg_Warn( p_obj, "Failed to disable webserver: %s", UpnpGetErrorMessage( i_res ) );
        }
    }
}
