#ifdef USE_UPNP
#include <string>
#include <thread>

#include <boost/thread/thread.hpp>
#include <boost/asio.hpp>
#include <boost/bind.hpp>

#ifdef _WIN32
#include <windows.h>
#define dlsym GetProcAddress
#else
#include <dlfcn.h>
#endif

#include "util/Log.h"

#include "RouterContext.h"
#include "UPnP.h"
#include "NetworkDatabase.h"
#include "util/util.h"

#include <miniupnpc/miniupnpc.h>
#include <miniupnpc/upnpcommands.h>

// These are per-process and are safe to reuse for all threads
#ifndef UPNPDISCOVER_SUCCESS
/* miniupnpc 1.5 */
UPNPDev* (*upnpDiscoverFunc) (int, const char *, const char *, int);
int (*UPNP_AddPortMappingFunc) (const char *, const char *, const char *, const char *,
                                             const char *, const char *, const char *, const char *);
#else
/* miniupnpc 1.6 */
UPNPDev* (*upnpDiscoverFunc) (int, const char *, const char *, int, int, int *);
int (*UPNP_AddPortMappingFunc) (const char *, const char *, const char *, const char *,
                                             const char *, const char *, const char *, const char *, const char *);
#endif
int (*UPNP_GetValidIGDFunc) (struct UPNPDev *, struct UPNPUrls *, struct IGDdatas *, char *, int);
int (*UPNP_GetExternalIPAddressFunc) (const char *, const char *, char *);
int (*UPNP_DeletePortMappingFunc) (const char *, const char *, const char *, const char *, const char *);
void (*freeUPNPDevlistFunc) (struct UPNPDev *);
void (*FreeUPNPUrlsFunc) (struct UPNPUrls *);

// Nice approach http://stackoverflow.com/a/21517513/673826
template<class M, typename F>
F GetKnownProcAddressImpl(M hmod, const char *name, F) {
    auto proc = reinterpret_cast<F>(dlsym(hmod, name));
    if (!proc) {
        LogPrint("Error resolving ", name, " from UPNP library. This often happens if there is version mismatch!");
    }
    return proc;
}
#define GetKnownProcAddress(hmod, func) GetKnownProcAddressImpl(hmod, #func, func##Func);


namespace i2p
{
namespace transport
{
    UPnP::UPnP () : m_Thread (nullptr) , m_IsModuleLoaded (false)
    {
    }

    void UPnP::Stop ()
    {
        if (m_Thread)
        {   
            m_Thread->join (); 
            delete m_Thread;
            m_Thread = nullptr;
        }
    }

    void UPnP::Start()
    {
        if (!m_IsModuleLoaded) {
#ifdef MAC_OSX
            m_Module = dlopen ("libminiupnpc.dylib", RTLD_LAZY);
#elif _WIN32
            m_Module = LoadLibrary ("miniupnpc.dll"); // official prebuilt binary, e.g., in upnpc-exe-win32-20140422.zip
#else
            m_Module = dlopen ("libminiupnpc.so", RTLD_LAZY);
#endif
            if (m_Module == NULL)
            {
                LogPrint ("Error loading UPNP library. This often happens if there is version mismatch!");
                return;
            }
            else
            {
                upnpDiscoverFunc = GetKnownProcAddress (m_Module, upnpDiscover);
                UPNP_GetValidIGDFunc = GetKnownProcAddress (m_Module, UPNP_GetValidIGD);
                UPNP_GetExternalIPAddressFunc = GetKnownProcAddress (m_Module, UPNP_GetExternalIPAddress);
                UPNP_AddPortMappingFunc = GetKnownProcAddress (m_Module, UPNP_AddPortMapping);
                UPNP_DeletePortMappingFunc = GetKnownProcAddress (m_Module, UPNP_DeletePortMapping);
                freeUPNPDevlistFunc = GetKnownProcAddress (m_Module, freeUPNPDevlist);
                FreeUPNPUrlsFunc = GetKnownProcAddress (m_Module, FreeUPNPUrls);
                if (upnpDiscoverFunc && UPNP_GetValidIGDFunc && UPNP_GetExternalIPAddressFunc && UPNP_AddPortMappingFunc &&
                    UPNP_DeletePortMappingFunc && freeUPNPDevlistFunc && FreeUPNPUrlsFunc)
                    m_IsModuleLoaded = true;
            }
        }
        m_Thread = new std::thread (std::bind (&UPnP::Run, this));
    }
    
    UPnP::~UPnP ()
    {
    } 

    void UPnP::Run ()
    {
        for (auto& address : context.GetRouterInfo ().GetAddresses ())
        {
            if (!address.host.is_v6 ())
            {
                Discover ();
                if (address.transportStyle == data::RouterInfo::eTransportSSU )
                {
                    TryPortMapping (I2P_UPNP_UDP, address.port);
                }
                else if (address.transportStyle == data::RouterInfo::eTransportNTCP )
                {
                    TryPortMapping (I2P_UPNP_TCP, address.port);
                }
            }
        }
    } 
        
    void UPnP::Discover ()
    {
#ifndef UPNPDISCOVER_SUCCESS
        /* miniupnpc 1.5 */
        m_Devlist = upnpDiscoverFunc (2000, m_MulticastIf, m_Minissdpdpath, 0);
#else
        /* miniupnpc 1.6 */
        int nerror = 0;
        m_Devlist = upnpDiscoverFunc (2000, m_MulticastIf, m_Minissdpdpath, 0, 0, &nerror);
#endif

        int r;
        r = UPNP_GetValidIGDFunc (m_Devlist, &m_upnpUrls, &m_upnpData, m_NetworkAddr, sizeof (m_NetworkAddr));
        if (r == 1)
        {
            r = UPNP_GetExternalIPAddressFunc (m_upnpUrls.controlURL, m_upnpData.first.servicetype, m_externalIPAddress);
            if(r != UPNPCOMMAND_SUCCESS)
            {
                LogPrint ("UPnP: UPNP_GetExternalIPAddress () returned ", r);
                return;
            }
            else
            {
                if (m_externalIPAddress[0])
                {
                    LogPrint ("UPnP: ExternalIPAddress = ", m_externalIPAddress);
                    i2p::context.UpdateAddress (boost::asio::ip::address::from_string (m_externalIPAddress));
                    return;
                }
                else
                {
                    LogPrint ("UPnP: GetExternalIPAddress failed.");
                    return;
                }
            }
        }
    }

    void UPnP::TryPortMapping (int type, int port)
    {
        std::string strType, strPort (std::to_string (port));
        switch (type)
        {
            case I2P_UPNP_TCP:
                strType = "TCP";
                break;
            case I2P_UPNP_UDP:
            default:
                strType = "UDP";
        }
        int r;
        std::string strDesc = "I2Pd";
        try {
            for (;;) {
#ifndef UPNPDISCOVER_SUCCESS
                /* miniupnpc 1.5 */
                r = UPNP_AddPortMappingFunc (m_upnpUrls.controlURL, m_upnpData.first.servicetype, strPort.c_str (), strPort.c_str (), m_NetworkAddr, strDesc.c_str (), strType.c_str (), 0);
#else
                /* miniupnpc 1.6 */
                r = UPNP_AddPortMappingFunc (m_upnpUrls.controlURL, m_upnpData.first.servicetype, strPort.c_str (), strPort.c_str (), m_NetworkAddr, strDesc.c_str (), strType.c_str (), 0, "0");
#endif
                if (r!=UPNPCOMMAND_SUCCESS)
                {
                    LogPrint ("AddPortMapping (", strPort.c_str () ,", ", strPort.c_str () ,", ", m_NetworkAddr, ") failed with code ", r);
                    return;
                }
                else
                {
                    LogPrint ("UPnP Port Mapping successful. (", m_NetworkAddr ,":", strPort.c_str(), " type ", strType.c_str () ," -> ", m_externalIPAddress ,":", strPort.c_str() ,")");
                    return;
                }
                std::this_thread::sleep_for(std::chrono::minutes(20)); // c++11
                //boost::this_thread::sleep_for(); // pre c++11
                //sleep(20*60); // non-portable
            }
        }
        catch (boost::thread_interrupted)
        {
            CloseMapping(type, port);
            Close();
            throw;
        }
    }

    void UPnP::CloseMapping (int type, int port)
    {
        std::string strType, strPort (std::to_string (port));
        switch (type)
        {
            case I2P_UPNP_TCP:
                strType = "TCP";
                break;
            case I2P_UPNP_UDP:
            default:
                strType = "UDP";
        }
        int r = 0;
        r = UPNP_DeletePortMappingFunc (m_upnpUrls.controlURL, m_upnpData.first.servicetype, strPort.c_str (), strType.c_str (), 0);
        LogPrint ("UPNP_DeletePortMapping() returned : ", r, "\n");
    }

    void UPnP::Close ()
    {
        freeUPNPDevlistFunc (m_Devlist);
        m_Devlist = 0;
        FreeUPNPUrlsFunc (&m_upnpUrls);
#ifndef _WIN32
        dlclose (m_Module);
#else
        FreeLibrary (m_Module);
#endif
    }

}
}


#endif

