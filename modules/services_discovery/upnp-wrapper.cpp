/*****************************************************************************
 * upnp-wrapper.cpp :  UPnP Instance Wrapper class
 *****************************************************************************
 * Copyright © 2004-2018 VLC authors and VideoLAN
 *
 * Authors: Rémi Denis-Courmont (original plugin)
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
#include <vlc_md5.h>

static const char *mediarenderer_desc_template   =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
    "<root xmlns=\"urn:schemas-upnp-org:device-1-0\">"
      "<specVersion>"
        "<major>1</major>"
        "<minor>0</minor>"
      "</specVersion>"
      "<device>"
        "<deviceType>urn:schemas-upnp-org:device:MediaRenderer:1</deviceType>"
        "<friendlyName>%s</friendlyName>"
        "<manufacturer>VideoLAN</manufacturer>"
        "<modelName>" PACKAGE_NAME "</modelName>"
        "<modelNumber>" PACKAGE_VERSION "</modelNumber>"
        "<modelURL>https://www.videolan.org/vlc/</modelURL>"
        "<UDN>%s</UDN>"
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

std::string generateUDN()
{
    struct md5_s hash;
    InitMD5( &hash );

    const char *ipv4addr = UpnpGetServerIpAddress();
    if( ipv4addr )
        AddMD5( &hash, ipv4addr, strlen( ipv4addr ) );
    char ipv4port[7] = {};
    snprintf( ipv4port, 7, ":%hu\n", UpnpGetServerPort() );
    AddMD5( &hash, ipv4port, strlen( ipv4port ) );

#ifdef UPNP_ENABLE_IPV6
    const char *ipv6addr = UpnpGetServerIp6Address();
    if( ipv6addr )
        AddMD5( &hash, ipv6addr, strlen( ipv6addr ) );
    char ipv6port[7] = {};
    snprintf( ipv6port, 7, ":%hu\n", UpnpGetServerPort6() );
    AddMD5( &hash, ipv6port, strlen( ipv6port ) );
#endif

    EndMD5( &hash );

    std::unique_ptr<char> hexhash( psz_md5_hash( &hash ) );
    std::string udn( hexhash.get() );
    udn.insert( 20, "-" );
    udn.insert( 16, "-" );
    udn.insert( 12, "-" );
    udn.insert( 8, "-" );
    udn.insert( 0, "uuid:" );

    return udn;
}

UpnpInstanceWrapper::UpnpInstanceWrapper()
    : m_client_handle( -1 )
    , m_device_handle( -1 )
    , m_refcount( 0 )
    , m_mediarenderer_refcount( 0 )
{
}

UpnpInstanceWrapper::~UpnpInstanceWrapper()
{
    UpnpUnRegisterClient( m_client_handle );
    if( m_device_handle != -1 )
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

        /* Build pseudo random UUID-like string based on listening addresses */
        instance->m_udn = generateUDN();

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
        if( root )
        {
            i_res = UpnpSetWebServerRootDir( root );
            if( i_res != UPNP_E_SUCCESS)
            {
                msg_Warn( p_obj, "UpnpSetWebServerRootDir failed: %s",
                          UpnpGetErrorMessage( i_res ));
            }
            free( root );
        }

        s_instance = instance;
    }
    s_instance->m_refcount++;
    return s_instance;
}

void UpnpInstanceWrapper::release()
{
    UpnpInstanceWrapper *p_delete = NULL;
    vlc::threads::mutex_locker lock( &s_lock );
    if (--s_instance->m_refcount == 0)
    {
        p_delete = s_instance;
        s_instance = NULL;
    }
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

std::string UpnpInstanceWrapper::udn() const
{
    return m_udn;
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

bool UpnpInstanceWrapper::startMediaRenderer( vlc_object_t *p_obj )
{
    vlc::threads::mutex_locker lock( &s_lock );
    if( m_mediarenderer_refcount == 0 )
    {
        std::string friendly_name( "VLC media player" );
        char hostname[128] = "";
        if( gethostname( hostname, sizeof( hostname ) ) == 0 &&
            strlen( hostname ) > 0 )
        {
            friendly_name = friendly_name + ": " + hostname;
        }

        char mediarenderer_desc[4096] = {};
        int i_res;
        i_res = snprintf( mediarenderer_desc, sizeof( mediarenderer_desc ),
                          mediarenderer_desc_template, friendly_name.c_str(),
                          udn().c_str() );
        if( i_res < 0 || i_res >= (int) sizeof( mediarenderer_desc ) )
        {
            msg_Err( p_obj, "Failed to build MediaRenderer XML description" );
            return false;
        }

        if( ( i_res = UpnpEnableWebserver( TRUE ) ) != UPNP_E_SUCCESS )
        {
            msg_Err( p_obj, "Failed to enable webserver: %s", UpnpGetErrorMessage( i_res ) );
            return false;
        }

        i_res = UpnpRegisterRootDevice2( UPNPREG_BUF_DESC,
                                         mediarenderer_desc,
                                         strlen( mediarenderer_desc ),
                                         1,
                                         Callback,
                                         nullptr,
                                         &m_device_handle );
        if( i_res != UPNP_E_SUCCESS )
        {
            msg_Err( p_obj, "Device registration failed: %s", UpnpGetErrorMessage( i_res ) );
            /* Disable web server again in case register device failed */
            UpnpEnableWebserver( FALSE );
            return false;
        }
    }
    m_mediarenderer_refcount++;

    return true;
}

bool UpnpInstanceWrapper::stopMediaRenderer( vlc_object_t *p_obj )
{
    vlc::threads::mutex_locker lock( &s_lock_lock );
    if( m_mediarenderer_refcount == 1 )
    {
        int i_res;
        if( ( i_res = UpnpEnableWebserver( FALSE ) ) != UPNP_E_SUCCESS )
        {
            msg_Err( p_obj, "Failed to disable webserver: %s", UpnpGetErrorMessage( i_res ) );
            return false;
        }

        i_res = UpnpUnRegisterRootDevice( m_device_handle );
        if( i_res != UPNP_E_SUCCESS )
        {
            msg_Err( p_obj, "Device unregistration failed: %s", UpnpGetErrorMessage( i_res ) );
            /* Enable web server again in case unregister device failed */
            UpnpEnableWebserver( TRUE );
            return false;
        }
        m_device_handle = -1;
    }
    m_mediarenderer_refcount--;

    return true;
}
