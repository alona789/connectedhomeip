/*
 *
 *    Copyright (c) 2020-2022 Project CHIP Authors
 *    Copyright (c) 2019 Nest Labs, Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include <platform/internal/CHIPDeviceLayerInternal.h>

#include <platform/CommissionableDataProvider.h>
#include <platform/ConnectivityManager.h>
#include <platform/DeviceControlServer.h>
#include <platform/DiagnosticDataProvider.h>
#include <platform/Linux/ConnectivityUtils.h>
#include <platform/Linux/DiagnosticDataProviderImpl.h>
#include <platform/Linux/NetworkCommissioningDriver.h>
#include <platform/Linux/WirelessDefs.h>
#include <platform/internal/BLEManager.h>

#include <algorithm>
#include <cstdlib>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include <ifaddrs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include <lib/support/BytesToHex.h>
#include <lib/support/CHIPMemString.h>
#include <lib/support/CodeUtils.h>
#include <lib/support/logging/CHIPLogging.h>

#include <platform/internal/GenericConnectivityManagerImpl_UDP.ipp>

#if INET_CONFIG_ENABLE_TCP_ENDPOINT
#include <platform/internal/GenericConnectivityManagerImpl_TCP.ipp>
#endif

#if CHIP_DEVICE_CONFIG_ENABLE_CHIPOBLE
#include <platform/internal/GenericConnectivityManagerImpl_BLE.ipp>
#endif

#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
#include <platform/internal/GenericConnectivityManagerImpl_Thread.ipp>
#endif

#if CHIP_DEVICE_CONFIG_ENABLE_WPA
#include <credentials/CHIPCert.h>
#include <platform/GLibTypeDeleter.h>
#include <platform/internal/GenericConnectivityManagerImpl_WiFi.ipp>
#endif

using namespace ::chip;
using namespace ::chip::TLV;
using namespace ::chip::Credentials;
using namespace ::chip::DeviceLayer;
using namespace ::chip::DeviceLayer::Internal;
using namespace ::chip::app::Clusters::GeneralDiagnostics;
using namespace ::chip::app::Clusters::WiFiNetworkDiagnostics;

using namespace ::chip::DeviceLayer::NetworkCommissioning;

namespace chip {

#if CHIP_DEVICE_CONFIG_ENABLE_WPA

template <>
struct GAutoPtrDeleter<WpaFiW1Wpa_supplicant1BSS>
{
    using deleter = GObjectDeleter;
};

template <>
struct GAutoPtrDeleter<WpaFiW1Wpa_supplicant1Network>
{
    using deleter = GObjectDeleter;
};

#endif // CHIP_DEVICE_CONFIG_ENABLE_WPA

namespace DeviceLayer {

ConnectivityManagerImpl ConnectivityManagerImpl::sInstance;

#if CHIP_DEVICE_CONFIG_ENABLE_WIFI
char ConnectivityManagerImpl::sWiFiIfName[];
#endif

WiFiDriver::ScanCallback * ConnectivityManagerImpl::mpScanCallback;
NetworkCommissioning::Internal::WirelessDriver::ConnectCallback * ConnectivityManagerImpl::mpConnectCallback;
uint8_t ConnectivityManagerImpl::sInterestedSSID[Internal::kMaxWiFiSSIDLength];
uint8_t ConnectivityManagerImpl::sInterestedSSIDLen;

CHIP_ERROR ConnectivityManagerImpl::_Init()
{
#if CHIP_DEVICE_CONFIG_ENABLE_WPA
    mWiFiStationMode              = kWiFiStationMode_Disabled;
    mWiFiStationReconnectInterval = System::Clock::Milliseconds32(CHIP_DEVICE_CONFIG_WIFI_STATION_RECONNECT_INTERVAL);
#endif
    mpConnectCallback = nullptr;
    mpScanCallback    = nullptr;

    if (ConnectivityUtils::GetEthInterfaceName(mEthIfName, IFNAMSIZ) == CHIP_NO_ERROR)
    {
        ChipLogProgress(DeviceLayer, "Got Ethernet interface: %s", mEthIfName);
    }
    else
    {
        ChipLogError(DeviceLayer, "Failed to get Ethernet interface");
        mEthIfName[0] = '\0';
    }

    if (GetDiagnosticDataProvider().ResetEthNetworkDiagnosticsCounts() != CHIP_NO_ERROR)
    {
        ChipLogError(DeviceLayer, "Failed to reset Ethernet statistic counts");
    }

    // Initialize the generic base classes that require it.
#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
    GenericConnectivityManagerImpl_Thread<ConnectivityManagerImpl>::_Init();
#endif

#if CHIP_DEVICE_CONFIG_ENABLE_WIFI
    if (ConnectivityUtils::GetWiFiInterfaceName(sWiFiIfName, IFNAMSIZ) == CHIP_NO_ERROR)
    {
        ChipLogProgress(DeviceLayer, "Got WiFi interface: %s", sWiFiIfName);
    }
    else
    {
        ChipLogError(DeviceLayer, "Failed to get WiFi interface");
        sWiFiIfName[0] = '\0';
    }

    if (GetDiagnosticDataProvider().ResetWiFiNetworkDiagnosticsCounts() != CHIP_NO_ERROR)
    {
        ChipLogError(DeviceLayer, "Failed to reset WiFi statistic counts");
    }
#endif

    return CHIP_NO_ERROR;
}

void ConnectivityManagerImpl::_OnPlatformEvent(const ChipDeviceEvent * event)
{
    // Forward the event to the generic base classes as needed.
#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
    GenericConnectivityManagerImpl_Thread<ConnectivityManagerImpl>::_OnPlatformEvent(event);
#endif
}

#if CHIP_DEVICE_CONFIG_ENABLE_WPA

bool ConnectivityManagerImpl::mAssociationStarted = false;
BitFlags<Internal::GenericConnectivityManagerImpl_WiFi<ConnectivityManagerImpl>::ConnectivityFlags>
    ConnectivityManagerImpl::mConnectivityFlag;
struct GDBusWpaSupplicant ConnectivityManagerImpl::mWpaSupplicant;
std::mutex ConnectivityManagerImpl::mWpaSupplicantMutex;

ConnectivityManager::WiFiStationMode ConnectivityManagerImpl::_GetWiFiStationMode()
{
    if (mWiFiStationMode != kWiFiStationMode_ApplicationControlled)
    {
        std::lock_guard<std::mutex> lock(mWpaSupplicantMutex);
        mWiFiStationMode = (mWpaSupplicant.iface != nullptr) ? kWiFiStationMode_Enabled : kWiFiStationMode_Disabled;
    }

    return mWiFiStationMode;
}

CHIP_ERROR ConnectivityManagerImpl::_SetWiFiStationMode(ConnectivityManager::WiFiStationMode val)
{
    CHIP_ERROR err = CHIP_NO_ERROR;

    VerifyOrExit(val != ConnectivityManager::kWiFiStationMode_NotSupported, err = CHIP_ERROR_INVALID_ARGUMENT);

    if (mWiFiStationMode != val)
    {
        ChipLogProgress(DeviceLayer, "WiFi station mode change: %s -> %s", WiFiStationModeToStr(mWiFiStationMode),
                        WiFiStationModeToStr(val));
    }

    mWiFiStationMode = val;
exit:
    return err;
}

System::Clock::Timeout ConnectivityManagerImpl::_GetWiFiStationReconnectInterval()
{
    return mWiFiStationReconnectInterval;
}

CHIP_ERROR ConnectivityManagerImpl::_SetWiFiStationReconnectInterval(System::Clock::Timeout val)
{
    mWiFiStationReconnectInterval = val;

    return CHIP_NO_ERROR;
}

bool ConnectivityManagerImpl::_IsWiFiStationEnabled()
{
    return GetWiFiStationMode() == kWiFiStationMode_Enabled;
}

bool ConnectivityManagerImpl::_IsWiFiStationConnected()
{
    bool ret            = false;
    const gchar * state = nullptr;

    std::lock_guard<std::mutex> lock(mWpaSupplicantMutex);

    if (mWpaSupplicant.state != GDBusWpaSupplicant::WPA_INTERFACE_CONNECTED)
    {
        ChipLogProgress(DeviceLayer, "wpa_supplicant: _IsWiFiStationConnected: interface not connected");
        return false;
    }

    state = wpa_fi_w1_wpa_supplicant1_interface_get_state(mWpaSupplicant.iface);
    if (g_strcmp0(state, "completed") == 0)
    {
        mConnectivityFlag.Set(ConnectivityFlags::kHaveIPv4InternetConnectivity)
            .Set(ConnectivityFlags::kHaveIPv6InternetConnectivity);
        ret = true;
    }

    return ret;
}

bool ConnectivityManagerImpl::_IsWiFiStationApplicationControlled()
{
    return mWiFiStationMode == ConnectivityManager::kWiFiStationMode_ApplicationControlled;
}

bool ConnectivityManagerImpl::_IsWiFiStationProvisioned()
{
    bool ret          = false;
    const gchar * bss = nullptr;

    std::lock_guard<std::mutex> lock(mWpaSupplicantMutex);

    if (mWpaSupplicant.state != GDBusWpaSupplicant::WPA_INTERFACE_CONNECTED)
    {
        ChipLogProgress(DeviceLayer, "wpa_supplicant: _IsWiFiStationProvisioned: interface not connected");
        return false;
    }

    bss = wpa_fi_w1_wpa_supplicant1_interface_get_current_bss(mWpaSupplicant.iface);
    if (g_str_match_string("BSSs", bss, true))
    {
        ret = true;
    }

    return ret;
}

void ConnectivityManagerImpl::_ClearWiFiStationProvision()
{
    std::lock_guard<std::mutex> lock(mWpaSupplicantMutex);

    if (mWpaSupplicant.state != GDBusWpaSupplicant::WPA_INTERFACE_CONNECTED)
    {
        ChipLogProgress(DeviceLayer, "wpa_supplicant: _ClearWiFiStationProvision: interface not connected");
        return;
    }

    if (mWiFiStationMode != kWiFiStationMode_ApplicationControlled)
    {
        GAutoPtr<GError> err;
        wpa_fi_w1_wpa_supplicant1_interface_call_remove_all_networks_sync(mWpaSupplicant.iface, nullptr,
                                                                          &MakeUniquePointerReceiver(err).Get());

        if (err != nullptr)
        {
            ChipLogProgress(DeviceLayer, "wpa_supplicant: failed to remove all networks with error: %s",
                            err ? err->message : "unknown error");
        }
    }
}

bool ConnectivityManagerImpl::_CanStartWiFiScan()
{
    std::lock_guard<std::mutex> lock(mWpaSupplicantMutex);

    bool ret = mWpaSupplicant.state == GDBusWpaSupplicant::WPA_INTERFACE_CONNECTED &&
        mWpaSupplicant.scanState == GDBusWpaSupplicant::WIFI_SCANNING_IDLE;

    return ret;
}

CHIP_ERROR ConnectivityManagerImpl::_SetWiFiAPMode(WiFiAPMode val)
{
    CHIP_ERROR err = CHIP_NO_ERROR;

    VerifyOrExit(val != kWiFiAPMode_NotSupported, err = CHIP_ERROR_INVALID_ARGUMENT);

    if (mWiFiAPMode != val)
    {
        ChipLogProgress(DeviceLayer, "WiFi AP mode change: %s -> %s", WiFiAPModeToStr(mWiFiAPMode), WiFiAPModeToStr(val));
        mWiFiAPMode = val;

        DeviceLayer::SystemLayer().ScheduleWork(DriveAPState, nullptr);
    }

exit:
    return err;
}

void ConnectivityManagerImpl::_DemandStartWiFiAP()
{
    if (mWiFiAPMode == kWiFiAPMode_OnDemand || mWiFiAPMode == kWiFiAPMode_OnDemand_NoStationProvision)
    {
        ChipLogProgress(DeviceLayer, "wpa_supplicant: Demand start WiFi AP");
        mLastAPDemandTime = System::SystemClock().GetMonotonicTimestamp();
        DeviceLayer::SystemLayer().ScheduleWork(DriveAPState, nullptr);
    }
    else
    {
        ChipLogProgress(DeviceLayer, "wpa_supplicant: Demand start WiFi AP ignored, mode: %s", WiFiAPModeToStr(mWiFiAPMode));
    }
}

void ConnectivityManagerImpl::_StopOnDemandWiFiAP()
{
    if (mWiFiAPMode == kWiFiAPMode_OnDemand || mWiFiAPMode == kWiFiAPMode_OnDemand_NoStationProvision)
    {
        ChipLogProgress(DeviceLayer, "wpa_supplicant: Demand stop WiFi AP");
        mLastAPDemandTime = System::Clock::kZero;
        DeviceLayer::SystemLayer().ScheduleWork(DriveAPState, nullptr);
    }
    else
    {
        ChipLogProgress(DeviceLayer, "wpa_supplicant: Demand stop WiFi AP ignored, mode: %s", WiFiAPModeToStr(mWiFiAPMode));
    }
}

void ConnectivityManagerImpl::_MaintainOnDemandWiFiAP()
{
    if (mWiFiAPMode == kWiFiAPMode_OnDemand || mWiFiAPMode == kWiFiAPMode_OnDemand_NoStationProvision)
    {
        if (mWiFiAPState == kWiFiAPState_Active)
        {
            mLastAPDemandTime = System::SystemClock().GetMonotonicTimestamp();
        }
    }
}

void ConnectivityManagerImpl::_SetWiFiAPIdleTimeout(System::Clock::Timeout val)
{
    mWiFiAPIdleTimeout = val;
    DeviceLayer::SystemLayer().ScheduleWork(DriveAPState, nullptr);
}

void ConnectivityManagerImpl::UpdateNetworkStatus()
{
    Network configuredNetwork;

    VerifyOrReturn(IsWiFiStationEnabled() && mpStatusChangeCallback != nullptr);

    CHIP_ERROR err = GetConfiguredNetwork(configuredNetwork);
    if (err != CHIP_NO_ERROR)
    {
        ChipLogError(DeviceLayer, "Failed to get configured network when updating network status: %s", err.AsString());
        return;
    }

    // If we have already connected to the WiFi AP, then return null to indicate a success state.
    if (IsWiFiStationConnected())
    {
        mpStatusChangeCallback->OnNetworkingStatusChange(
            Status::kSuccess, MakeOptional(ByteSpan(configuredNetwork.networkID, configuredNetwork.networkIDLen)), NullOptional);
        return;
    }

    mpStatusChangeCallback->OnNetworkingStatusChange(
        Status::kUnknownError, MakeOptional(ByteSpan(configuredNetwork.networkID, configuredNetwork.networkIDLen)),
        MakeOptional(GetDisconnectReason()));
}

void ConnectivityManagerImpl::_OnWpaPropertiesChanged(WpaFiW1Wpa_supplicant1Interface * proxy, GVariant * changed_properties,
                                                      const gchar * const * invalidated_properties, gpointer user_data)
{
    std::lock_guard<std::mutex> lock(mWpaSupplicantMutex);

    if (g_variant_n_children(changed_properties) > 0)
    {
        GAutoPtr<GVariantIter> iter;
        const gchar * key;
        GVariant * value;

        WiFiDiagnosticsDelegate * delegate = GetDiagnosticDataProvider().GetWiFiDiagnosticsDelegate();

        g_variant_get(changed_properties, "a{sv}", &MakeUniquePointerReceiver(iter).Get());

        while (g_variant_iter_loop(iter.get(), "{&sv}", &key, &value))
        {
            GAutoPtr<gchar> value_str(g_variant_print(value, TRUE));
            ChipLogProgress(DeviceLayer, "wpa_supplicant:PropertiesChanged:key:%s -> %s", StringOrNullMarker(key),
                            StringOrNullMarker(value_str.get()));

            if (g_strcmp0(key, "State") == 0)
            {
                if (g_strcmp0(value_str.get(), "\'associating\'") == 0)
                {
                    mAssociationStarted = true;
                }
                else if (g_strcmp0(value_str.get(), "\'disconnected\'") == 0)
                {
                    gint reason = wpa_fi_w1_wpa_supplicant1_interface_get_disconnect_reason(mWpaSupplicant.iface);

                    if (delegate)
                    {
                        delegate->OnDisconnectionDetected(reason);
                        delegate->OnConnectionStatusChanged(static_cast<uint8_t>(ConnectionStatusEnum::kConnected));
                    }

                    if (mAssociationStarted)
                    {
                        uint8_t associationFailureCause = static_cast<uint8_t>(AssociationFailureCauseEnum::kUnknown);
                        uint16_t status                 = WLAN_STATUS_UNSPECIFIED_FAILURE;

                        switch (abs(reason))
                        {
                        case WLAN_REASON_DISASSOC_DUE_TO_INACTIVITY:
                        case WLAN_REASON_DISASSOC_AP_BUSY:
                        case WLAN_REASON_DISASSOC_STA_HAS_LEFT:
                        case WLAN_REASON_DISASSOC_LOW_ACK:
                        case WLAN_REASON_BSS_TRANSITION_DISASSOC:
                            associationFailureCause = static_cast<uint8_t>(AssociationFailureCauseEnum::kAssociationFailed);
                            status = wpa_fi_w1_wpa_supplicant1_interface_get_assoc_status_code(mWpaSupplicant.iface);
                            break;
                        case WLAN_REASON_PREV_AUTH_NOT_VALID:
                        case WLAN_REASON_DEAUTH_LEAVING:
                        case WLAN_REASON_IEEE_802_1X_AUTH_FAILED:
                            associationFailureCause = static_cast<uint8_t>(AssociationFailureCauseEnum::kAuthenticationFailed);
                            status = wpa_fi_w1_wpa_supplicant1_interface_get_auth_status_code(mWpaSupplicant.iface);
                            break;
                        default:
                            break;
                        }

                        DeviceLayer::SystemLayer().ScheduleLambda([reason]() {
                            if (mpConnectCallback != nullptr)
                            {
                                mpConnectCallback->OnResult(NetworkCommissioning::Status::kUnknownError, CharSpan(), reason);
                                mpConnectCallback = nullptr;
                            }
                        });

                        if (delegate)
                        {
                            delegate->OnAssociationFailureDetected(associationFailureCause, status);
                        }
                    }

                    DeviceLayer::SystemLayer().ScheduleLambda([]() { ConnectivityMgrImpl().UpdateNetworkStatus(); });

                    mAssociationStarted = false;
                }
                else if (g_strcmp0(value_str.get(), "\'associated\'") == 0)
                {
                    if (delegate)
                    {
                        delegate->OnConnectionStatusChanged(static_cast<uint8_t>(ConnectionStatusEnum::kNotConnected));
                    }

                    DeviceLayer::SystemLayer().ScheduleLambda([]() { ConnectivityMgrImpl().UpdateNetworkStatus(); });
                }
                else if (g_strcmp0(value_str.get(), "\'completed\'") == 0)
                {
                    if (mAssociationStarted)
                    {
                        DeviceLayer::SystemLayer().ScheduleLambda([]() {
                            if (mpConnectCallback != nullptr)
                            {
                                mpConnectCallback->OnResult(NetworkCommissioning::Status::kSuccess, CharSpan(), 0);
                                mpConnectCallback = nullptr;
                            }
                            ConnectivityMgrImpl().PostNetworkConnect();
                        });
                    }
                    mAssociationStarted = false;
                }
            }
        }
    }
}

void ConnectivityManagerImpl::_OnWpaInterfaceProxyReady(GObject * source_object, GAsyncResult * res, gpointer user_data)
{
    // When creating D-Bus proxy object, the thread default context must be initialized. Otherwise,
    // all D-Bus signals will be delivered to the GLib global default main context.
    VerifyOrDie(g_main_context_get_thread_default() != nullptr);

    GAutoPtr<GError> err;

    std::lock_guard<std::mutex> lock(mWpaSupplicantMutex);

    WpaFiW1Wpa_supplicant1Interface * iface =
        wpa_fi_w1_wpa_supplicant1_interface_proxy_new_for_bus_finish(res, &MakeUniquePointerReceiver(err).Get());

    if (mWpaSupplicant.iface)
    {
        g_object_unref(mWpaSupplicant.iface);
        mWpaSupplicant.iface = nullptr;
    }

    if (iface != nullptr && err == nullptr)
    {
        mWpaSupplicant.iface = iface;
        mWpaSupplicant.state = GDBusWpaSupplicant::WPA_INTERFACE_CONNECTED;
        ChipLogProgress(DeviceLayer, "wpa_supplicant: connected to wpa_supplicant interface proxy");

        g_signal_connect(mWpaSupplicant.iface, "properties-changed", G_CALLBACK(_OnWpaPropertiesChanged), NULL);
        g_signal_connect(mWpaSupplicant.iface, "scan-done", G_CALLBACK(_OnWpaInterfaceScanDone), NULL);
    }
    else
    {
        ChipLogProgress(DeviceLayer, "wpa_supplicant: failed to create wpa_supplicant interface proxy %s: %s",
                        mWpaSupplicant.interfacePath, err ? err->message : "unknown error");

        mWpaSupplicant.state = GDBusWpaSupplicant::WPA_NOT_CONNECTED;
    }

    // We need to stop auto scan or it will block our network scan.
    DeviceLayer::SystemLayer().ScheduleLambda([]() {
        CHIP_ERROR errInner = StopAutoScan();
        if (errInner != CHIP_NO_ERROR)
        {
            ChipLogError(DeviceLayer, "wpa_supplicant: Failed to stop auto scan: %s", ErrorStr(errInner));
        }
    });
}

void ConnectivityManagerImpl::_OnWpaBssProxyReady(GObject * source_object, GAsyncResult * res, gpointer user_data)
{
    // When creating D-Bus proxy object, the thread default context must be initialized. Otherwise,
    // all D-Bus signals will be delivered to the GLib global default main context.
    VerifyOrDie(g_main_context_get_thread_default() != nullptr);

    GAutoPtr<GError> err;

    std::lock_guard<std::mutex> lock(mWpaSupplicantMutex);

    WpaFiW1Wpa_supplicant1BSS * bss =
        wpa_fi_w1_wpa_supplicant1_bss_proxy_new_for_bus_finish(res, &MakeUniquePointerReceiver(err).Get());

    if (mWpaSupplicant.bss)
    {
        g_object_unref(mWpaSupplicant.bss);
        mWpaSupplicant.bss = nullptr;
    }

    if (bss != nullptr && err.get() == nullptr)
    {
        mWpaSupplicant.bss = bss;
        ChipLogProgress(DeviceLayer, "wpa_supplicant: connected to wpa_supplicant bss proxy");
    }
    else
    {
        ChipLogProgress(DeviceLayer, "wpa_supplicant: failed to create wpa_supplicant bss proxy %s: %s",
                        mWpaSupplicant.interfacePath, err ? err->message : "unknown error");
    }
}

void ConnectivityManagerImpl::_OnWpaInterfaceReady(GObject * source_object, GAsyncResult * res, gpointer user_data)
{
    // When creating D-Bus proxy object, the thread default context must be initialized. Otherwise,
    // all D-Bus signals will be delivered to the GLib global default main context.
    VerifyOrDie(g_main_context_get_thread_default() != nullptr);

    GAutoPtr<GError> err;

    std::lock_guard<std::mutex> lock(mWpaSupplicantMutex);

    gboolean result = wpa_fi_w1_wpa_supplicant1_call_get_interface_finish(mWpaSupplicant.proxy, &mWpaSupplicant.interfacePath, res,
                                                                          &MakeUniquePointerReceiver(err).Get());
    if (result)
    {
        mWpaSupplicant.state = GDBusWpaSupplicant::WPA_GOT_INTERFACE_PATH;
        ChipLogProgress(DeviceLayer, "wpa_supplicant: WiFi interface: %s", mWpaSupplicant.interfacePath);

        wpa_fi_w1_wpa_supplicant1_interface_proxy_new_for_bus(G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, kWpaSupplicantServiceName,
                                                              mWpaSupplicant.interfacePath, nullptr, _OnWpaInterfaceProxyReady,
                                                              nullptr);

        wpa_fi_w1_wpa_supplicant1_bss_proxy_new_for_bus(G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, kWpaSupplicantServiceName,
                                                        mWpaSupplicant.interfacePath, nullptr, _OnWpaBssProxyReady, nullptr);
    }
    else
    {
        GAutoPtr<GError> error;
        GVariant * args = nullptr;
        GVariantBuilder builder;

        ChipLogProgress(DeviceLayer, "wpa_supplicant: can't find interface %s: %s", sWiFiIfName,
                        err ? err->message : "unknown error");

        ChipLogProgress(DeviceLayer, "wpa_supplicant: try to create interface %s", CHIP_DEVICE_CONFIG_WIFI_STATION_IF_NAME);

        g_variant_builder_init(&builder, G_VARIANT_TYPE_VARDICT);
        g_variant_builder_add(&builder, "{sv}", "Ifname", g_variant_new_string(CHIP_DEVICE_CONFIG_WIFI_STATION_IF_NAME));
        args = g_variant_builder_end(&builder);

        result = wpa_fi_w1_wpa_supplicant1_call_create_interface_sync(mWpaSupplicant.proxy, args, &mWpaSupplicant.interfacePath,
                                                                      nullptr, &MakeUniquePointerReceiver(error).Get());

        if (result)
        {
            mWpaSupplicant.state = GDBusWpaSupplicant::WPA_GOT_INTERFACE_PATH;
            ChipLogProgress(DeviceLayer, "wpa_supplicant: WiFi interface: %s", mWpaSupplicant.interfacePath);

            Platform::CopyString(sWiFiIfName, CHIP_DEVICE_CONFIG_WIFI_STATION_IF_NAME);

            wpa_fi_w1_wpa_supplicant1_interface_proxy_new_for_bus(G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE,
                                                                  kWpaSupplicantServiceName, mWpaSupplicant.interfacePath, nullptr,
                                                                  _OnWpaInterfaceProxyReady, nullptr);

            wpa_fi_w1_wpa_supplicant1_bss_proxy_new_for_bus(G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, kWpaSupplicantServiceName,
                                                            mWpaSupplicant.interfacePath, nullptr, _OnWpaBssProxyReady, nullptr);
        }
        else
        {
            ChipLogProgress(DeviceLayer, "wpa_supplicant: failed to create interface %s: %s",
                            CHIP_DEVICE_CONFIG_WIFI_STATION_IF_NAME, error ? error->message : "unknown error");

            mWpaSupplicant.state = GDBusWpaSupplicant::WPA_NO_INTERFACE_PATH;

            if (mWpaSupplicant.interfacePath)
            {
                g_free(mWpaSupplicant.interfacePath);
                mWpaSupplicant.interfacePath = nullptr;
            }
        }
    }
}

void ConnectivityManagerImpl::_OnWpaInterfaceAdded(WpaFiW1Wpa_supplicant1 * proxy, const gchar * path, GVariant * properties,
                                                   gpointer user_data)
{
    // When creating D-Bus proxy object, the thread default context must be initialized. Otherwise,
    // all D-Bus signals will be delivered to the GLib global default main context.
    VerifyOrDie(g_main_context_get_thread_default() != nullptr);

    std::lock_guard<std::mutex> lock(mWpaSupplicantMutex);

    if (mWpaSupplicant.interfacePath)
    {
        return;
    }

    mWpaSupplicant.interfacePath = const_cast<gchar *>(path);
    if (mWpaSupplicant.interfacePath)
    {
        mWpaSupplicant.state = GDBusWpaSupplicant::WPA_GOT_INTERFACE_PATH;
        ChipLogProgress(DeviceLayer, "wpa_supplicant: WiFi interface added: %s", mWpaSupplicant.interfacePath);

        wpa_fi_w1_wpa_supplicant1_interface_proxy_new_for_bus(G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, kWpaSupplicantServiceName,
                                                              mWpaSupplicant.interfacePath, nullptr, _OnWpaInterfaceProxyReady,
                                                              nullptr);

        wpa_fi_w1_wpa_supplicant1_bss_proxy_new_for_bus(G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, kWpaSupplicantServiceName,
                                                        mWpaSupplicant.interfacePath, nullptr, _OnWpaBssProxyReady, nullptr);
    }
}

void ConnectivityManagerImpl::_OnWpaInterfaceRemoved(WpaFiW1Wpa_supplicant1 * proxy, const gchar * path, GVariant * properties,
                                                     gpointer user_data)
{
    std::lock_guard<std::mutex> lock(mWpaSupplicantMutex);

    if (mWpaSupplicant.interfacePath == nullptr)
    {
        return;
    }

    if (g_strcmp0(mWpaSupplicant.interfacePath, path) == 0)
    {
        ChipLogProgress(DeviceLayer, "wpa_supplicant: WiFi interface removed: %s", StringOrNullMarker(path));

        mWpaSupplicant.state = GDBusWpaSupplicant::WPA_NO_INTERFACE_PATH;

        if (mWpaSupplicant.interfacePath)
        {
            g_free(mWpaSupplicant.interfacePath);
            mWpaSupplicant.interfacePath = nullptr;
        }

        if (mWpaSupplicant.iface)
        {
            g_object_unref(mWpaSupplicant.iface);
            mWpaSupplicant.iface = nullptr;
        }

        if (mWpaSupplicant.bss)
        {
            g_object_unref(mWpaSupplicant.bss);
            mWpaSupplicant.bss = nullptr;
        }

        mWpaSupplicant.scanState = GDBusWpaSupplicant::WIFI_SCANNING_IDLE;
    }
}

void ConnectivityManagerImpl::_OnWpaProxyReady(GObject * source_object, GAsyncResult * res, gpointer user_data)
{
    // When creating D-Bus proxy object, the thread default context must be initialized. Otherwise,
    // all D-Bus signals will be delivered to the GLib global default main context.
    VerifyOrDie(g_main_context_get_thread_default() != nullptr);

    GAutoPtr<GError> err;

    std::lock_guard<std::mutex> lock(mWpaSupplicantMutex);

    mWpaSupplicant.proxy = wpa_fi_w1_wpa_supplicant1_proxy_new_for_bus_finish(res, &MakeUniquePointerReceiver(err).Get());
    if (mWpaSupplicant.proxy != nullptr && err.get() == nullptr)
    {
        mWpaSupplicant.state = GDBusWpaSupplicant::WPA_CONNECTED;
        ChipLogProgress(DeviceLayer, "wpa_supplicant: connected to wpa_supplicant proxy");

        g_signal_connect(mWpaSupplicant.proxy, "interface-added", G_CALLBACK(_OnWpaInterfaceAdded), NULL);

        g_signal_connect(mWpaSupplicant.proxy, "interface-removed", G_CALLBACK(_OnWpaInterfaceRemoved), NULL);

        wpa_fi_w1_wpa_supplicant1_call_get_interface(mWpaSupplicant.proxy, sWiFiIfName, nullptr, _OnWpaInterfaceReady, nullptr);
    }
    else
    {
        ChipLogProgress(DeviceLayer, "wpa_supplicant: failed to create wpa_supplicant proxy %s",
                        err ? err->message : "unknown error");
        mWpaSupplicant.state = GDBusWpaSupplicant::WPA_NOT_CONNECTED;
    }
}

void ConnectivityManagerImpl::StartWiFiManagement()
{
    std::lock_guard<std::mutex> lock(mWpaSupplicantMutex);

    mConnectivityFlag.ClearAll();
    mWpaSupplicant = GDBusWpaSupplicant{};

    CHIP_ERROR err = PlatformMgrImpl().GLibMatterContextInvokeSync(_StartWiFiManagement, this);
    VerifyOrReturn(err == CHIP_NO_ERROR, ChipLogError(DeviceLayer, "Failed to start WiFi management"));
}

bool ConnectivityManagerImpl::IsWiFiManagementStarted()
{
    std::lock_guard<std::mutex> lock(mWpaSupplicantMutex);

    bool ret = mWpaSupplicant.state == GDBusWpaSupplicant::WPA_INTERFACE_CONNECTED;

    return ret;
}

void ConnectivityManagerImpl::StartNonConcurrentWiFiManagement()
{
    StartWiFiManagement();

    for (int cnt = 0; cnt < WIFI_START_CHECK_ATTEMPTS; cnt++)
    {
        if (IsWiFiManagementStarted())
        {
            DeviceControlServer::DeviceControlSvr().PostOperationalNetworkStartedEvent();
            ChipLogProgress(DeviceLayer, "Non-concurrent mode Wi-Fi Management Started.");
            return;
        }
        usleep(WIFI_START_CHECK_TIME_USEC);
    }
    ChipLogError(Ble, "Non-concurrent mode Wi-Fi Management taking too long to start.");
}

void ConnectivityManagerImpl::DriveAPState()
{
    CHIP_ERROR err = CHIP_NO_ERROR;
    WiFiAPState targetState;

    std::lock_guard<std::mutex> lock(mWpaSupplicantMutex);

    // If the AP interface is not under application control...
    if (mWiFiAPMode != kWiFiAPMode_ApplicationControlled)
    {
        // Determine the target (desired) state for AP interface...

        // The target state is 'NotActive' if the application has expressly disabled the AP interface.
        if (mWiFiAPMode == kWiFiAPMode_Disabled)
        {
            targetState = kWiFiAPState_NotActive;
        }

        // The target state is 'Active' if the application has expressly enabled the AP interface.
        else if (mWiFiAPMode == kWiFiAPMode_Enabled)
        {
            targetState = kWiFiAPState_Active;
        }

        // The target state is 'Active' if the AP mode is 'On demand, when no station is available'
        // and the station interface is not provisioned or the application has disabled the station
        // interface.
        else if (mWiFiAPMode == kWiFiAPMode_OnDemand_NoStationProvision &&
                 (!IsWiFiStationProvisioned() || GetWiFiStationMode() == kWiFiStationMode_Disabled))
        {
            targetState = kWiFiAPState_Active;
        }

        // The target state is 'Active' if the AP mode is one of the 'On demand' modes and there
        // has been demand for the AP within the idle timeout period.
        else if (mWiFiAPMode == kWiFiAPMode_OnDemand || mWiFiAPMode == kWiFiAPMode_OnDemand_NoStationProvision)
        {
            System::Clock::Timestamp now = System::SystemClock().GetMonotonicTimestamp();

            if (mLastAPDemandTime != System::Clock::kZero && now < (mLastAPDemandTime + mWiFiAPIdleTimeout))
            {
                targetState = kWiFiAPState_Active;

                // Compute the amount of idle time before the AP should be deactivated and
                // arm a timer to fire at that time.
                System::Clock::Timeout apTimeout = (mLastAPDemandTime + mWiFiAPIdleTimeout) - now;
                err                              = DeviceLayer::SystemLayer().StartTimer(apTimeout, DriveAPState, nullptr);
                SuccessOrExit(err);
                ChipLogProgress(DeviceLayer, "Next WiFi AP timeout in %" PRIu32 " s",
                                std::chrono::duration_cast<System::Clock::Seconds32>(apTimeout).count());
            }
            else
            {
                targetState = kWiFiAPState_NotActive;
            }
        }

        // Otherwise the target state is 'NotActive'.
        else
        {
            targetState = kWiFiAPState_NotActive;
        }

        // If the current AP state does not match the target state...
        if (mWiFiAPState != targetState)
        {
            if (targetState == kWiFiAPState_Active)
            {
                err = ConfigureWiFiAP();
                SuccessOrExit(err);

                ChangeWiFiAPState(kWiFiAPState_Active);
            }
            else
            {
                if (mWpaSupplicant.networkPath)
                {
                    GAutoPtr<GError> error(nullptr);

                    gboolean result = wpa_fi_w1_wpa_supplicant1_interface_call_remove_network_sync(
                        mWpaSupplicant.iface, mWpaSupplicant.networkPath, nullptr, &MakeUniquePointerReceiver(error).Get());

                    if (result)
                    {
                        ChipLogProgress(DeviceLayer, "wpa_supplicant: removed network: %s", mWpaSupplicant.networkPath);
                        g_free(mWpaSupplicant.networkPath);
                        mWpaSupplicant.networkPath = nullptr;
                        ChangeWiFiAPState(kWiFiAPState_NotActive);
                    }
                    else
                    {
                        ChipLogProgress(DeviceLayer, "wpa_supplicant: failed to stop AP mode with error: %s",
                                        error ? error->message : "unknown error");
                        err = CHIP_ERROR_INTERNAL;
                    }
                }
            }
        }
    }

exit:
    if (err != CHIP_NO_ERROR)
    {
        SetWiFiAPMode(kWiFiAPMode_Disabled);
        ChipLogError(DeviceLayer, "Drive AP state failed: %s", ErrorStr(err));
    }
}

CHIP_ERROR ConnectivityManagerImpl::ConfigureWiFiAP()
{
    CHIP_ERROR ret = CHIP_NO_ERROR;
    GAutoPtr<GError> err;
    GVariant * args = nullptr;
    GVariantBuilder builder;

    uint16_t channel       = 1;
    uint16_t discriminator = 0;
    char ssid[32];

    std::lock_guard<std::mutex> lock(mWpaSupplicantMutex);

    channel = ConnectivityUtils::MapChannelToFrequency(kWiFi_BAND_2_4_GHZ, CHIP_DEVICE_CONFIG_WIFI_AP_CHANNEL);

    if (GetCommissionableDataProvider()->GetSetupDiscriminator(discriminator) != CHIP_NO_ERROR)
        discriminator = 0;

    snprintf(ssid, 32, "%s%04u", CHIP_DEVICE_CONFIG_WIFI_AP_SSID_PREFIX, discriminator);

    ChipLogProgress(DeviceLayer, "wpa_supplicant: ConfigureWiFiAP, ssid: %s, channel: %d", ssid, channel);

    // Clean up current network if exists
    if (mWpaSupplicant.networkPath)
    {
        g_object_unref(mWpaSupplicant.networkPath);
        mWpaSupplicant.networkPath = nullptr;
    }

    g_variant_builder_init(&builder, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&builder, "{sv}", "ssid", g_variant_new_string(ssid));
    g_variant_builder_add(&builder, "{sv}", "key_mgmt", g_variant_new_string("NONE"));
    g_variant_builder_add(&builder, "{sv}", "mode", g_variant_new_int32(2));
    g_variant_builder_add(&builder, "{sv}", "frequency", g_variant_new_int32(channel));
    args = g_variant_builder_end(&builder);

    gboolean result = wpa_fi_w1_wpa_supplicant1_interface_call_add_network_sync(
        mWpaSupplicant.iface, args, &mWpaSupplicant.networkPath, nullptr, &MakeUniquePointerReceiver(err).Get());

    if (result)
    {
        GAutoPtr<GError> error;

        ChipLogProgress(DeviceLayer, "wpa_supplicant: added network: SSID: %s: %s", ssid, mWpaSupplicant.networkPath);

        result = wpa_fi_w1_wpa_supplicant1_interface_call_select_network_sync(mWpaSupplicant.iface, mWpaSupplicant.networkPath,
                                                                              nullptr, &MakeUniquePointerReceiver(error).Get());
        if (result)
        {
            ChipLogProgress(DeviceLayer, "wpa_supplicant: succeeded to start softAP: SSID: %s", ssid);
        }
        else
        {
            ChipLogProgress(DeviceLayer, "wpa_supplicant: failed to start softAP: SSID: %s: %s", ssid,
                            error ? error->message : "unknown error");

            ret = CHIP_ERROR_INTERNAL;
        }
    }
    else
    {
        ChipLogProgress(DeviceLayer, "wpa_supplicant: failed to add network: %s: %s", ssid, err ? err->message : "unknown error");

        if (mWpaSupplicant.networkPath)
        {
            g_object_unref(mWpaSupplicant.networkPath);
            mWpaSupplicant.networkPath = nullptr;
        }

        ret = CHIP_ERROR_INTERNAL;
    }

    return ret;
}

void ConnectivityManagerImpl::ChangeWiFiAPState(WiFiAPState newState)
{
    if (mWiFiAPState != newState)
    {
        ChipLogProgress(DeviceLayer, "WiFi AP state change: %s -> %s", WiFiAPStateToStr(mWiFiAPState), WiFiAPStateToStr(newState));
        mWiFiAPState = newState;
    }
}

void ConnectivityManagerImpl::DriveAPState(::chip::System::Layer * aLayer, void * aAppState)
{
    sInstance.DriveAPState();
}

CHIP_ERROR
ConnectivityManagerImpl::_ConnectWiFiNetworkAsync(GVariant * args,
                                                  NetworkCommissioning::Internal::WirelessDriver::ConnectCallback * apCallback)
{
    GAutoPtr<GVariant> argsDeleter(g_variant_ref_sink(args)); // args may be floating, ensure we don't leak it

    CHIP_ERROR ret = CHIP_NO_ERROR;
    GAutoPtr<GError> err;
    gboolean result;

    const gchar * networkPath = wpa_fi_w1_wpa_supplicant1_interface_get_current_network(mWpaSupplicant.iface);

    // wpa_supplicant DBus API: if network path of current network is not "/", means we have already selected some network.
    if (strcmp(networkPath, "/") != 0)
    {
        GAutoPtr<GError> error;

        result = wpa_fi_w1_wpa_supplicant1_interface_call_remove_network_sync(mWpaSupplicant.iface, networkPath, nullptr,
                                                                              &MakeUniquePointerReceiver(error).Get());

        if (result)
        {
            if (mWpaSupplicant.networkPath != nullptr)
            {
                ChipLogProgress(DeviceLayer, "wpa_supplicant: removed network: %s", mWpaSupplicant.networkPath);
                g_free(mWpaSupplicant.networkPath);
                mWpaSupplicant.networkPath = nullptr;
            }
        }
        else
        {
            ChipLogProgress(DeviceLayer, "wpa_supplicant: failed to stop AP mode with error: %s",
                            error ? error->message : "unknown error");
            ret = CHIP_ERROR_INTERNAL;
        }

        SuccessOrExit(ret);
    }

    result = wpa_fi_w1_wpa_supplicant1_interface_call_add_network_sync(mWpaSupplicant.iface, args, &mWpaSupplicant.networkPath,
                                                                       nullptr, &MakeUniquePointerReceiver(err).Get());

    if (result)
    {
        // Note: wpa_supplicant will return immediately if the network is already connected, but it will still try reconnect in the
        // background. The client still need to wait for a few seconds for this reconnect operation. So we always disconnect from
        // the network we are connected and ignore any errors.
        wpa_fi_w1_wpa_supplicant1_interface_call_disconnect_sync(mWpaSupplicant.iface, nullptr, nullptr);
        ChipLogProgress(DeviceLayer, "wpa_supplicant: added network: %s", mWpaSupplicant.networkPath);

        wpa_fi_w1_wpa_supplicant1_interface_call_select_network(mWpaSupplicant.iface, mWpaSupplicant.networkPath, nullptr,
                                                                _ConnectWiFiNetworkAsyncCallback, this);
        mpConnectCallback = apCallback;
    }
    else
    {
        ChipLogError(DeviceLayer, "wpa_supplicant: failed to add network: %s", err ? err->message : "unknown error");

        if (mWpaSupplicant.networkPath)
        {
            g_object_unref(mWpaSupplicant.networkPath);
            mWpaSupplicant.networkPath = nullptr;
        }

        ret = CHIP_ERROR_INTERNAL;
    }

exit:
    return ret;
}

CHIP_ERROR
ConnectivityManagerImpl::ConnectWiFiNetworkAsync(ByteSpan ssid, ByteSpan credentials,
                                                 NetworkCommissioning::Internal::WirelessDriver::ConnectCallback * connectCallback)
{
    char ssidStr[kMaxWiFiSSIDLength + 1] = { 0 };
    char keyStr[kMaxWiFiKeyLength + 1]   = { 0 };

    VerifyOrReturnError(ssid.size() <= kMaxWiFiSSIDLength, CHIP_ERROR_INVALID_ARGUMENT);
    VerifyOrReturnError(credentials.size() <= kMaxWiFiKeyLength, CHIP_ERROR_INVALID_ARGUMENT);

    std::lock_guard<std::mutex> lock(mWpaSupplicantMutex);
    VerifyOrReturnError(mWpaSupplicant.iface != nullptr, CHIP_ERROR_INCORRECT_STATE);

    // There is another ongoing connect request, reject the new one.
    VerifyOrReturnError(mpConnectCallback == nullptr, CHIP_ERROR_INCORRECT_STATE);

    GVariantBuilder builder;
    g_variant_builder_init(&builder, G_VARIANT_TYPE_VARDICT);
    memcpy(ssidStr, ssid.data(), ssid.size());
    memcpy(keyStr, credentials.data(), credentials.size());
    g_variant_builder_add(&builder, "{sv}", "ssid", g_variant_new_string(ssidStr));
    g_variant_builder_add(&builder, "{sv}", "psk", g_variant_new_string(keyStr));
    g_variant_builder_add(&builder, "{sv}", "key_mgmt", g_variant_new_string("SAE WPA-PSK"));
    GVariant * args = g_variant_builder_end(&builder);
    return _ConnectWiFiNetworkAsync(args, connectCallback);
}

#if CHIP_DEVICE_CONFIG_ENABLE_WIFI_PDC
static CHIP_ERROR AddOrReplaceBlob(WpaFiW1Wpa_supplicant1Interface * iface, const char * nameOrRef, ByteSpan data)
{
    // Strip the blob:// prefix off the name (if present), so we don't need as many string constants.
    constexpr auto refPrefix = "blob://"_span;
    const char * name = (strncmp(nameOrRef, refPrefix.data(), refPrefix.size()) == 0) ? nameOrRef + refPrefix.size() : nameOrRef;

    GAutoPtr<GError> err;
    if (!wpa_fi_w1_wpa_supplicant1_interface_call_remove_blob_sync(iface, name, nullptr, &MakeUniquePointerReceiver(err).Get()))
    {
        GAutoPtr<char> remoteError(g_dbus_error_get_remote_error(err.get()));
        if (!(remoteError && strcmp(remoteError.get(), kWpaSupplicantBlobUnknown) == 0))
        {
            ChipLogError(DeviceLayer, "wpa_supplicant: failed to remove blob: %s", err ? err->message : "unknown error");
            return CHIP_ERROR_INTERNAL;
        }
        err.reset();
    }
    if (!wpa_fi_w1_wpa_supplicant1_interface_call_add_blob_sync(
            iface, name, g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, data.data(), data.size(), 1), nullptr,
            &MakeUniquePointerReceiver(err).Get()))
    {
        ChipLogError(DeviceLayer, "wpa_supplicant: failed to add blob: %s", err ? err->message : "unknown error");
        return CHIP_ERROR_INTERNAL;
    }
    return CHIP_NO_ERROR;
}

// Note: Static blob names assume we're only supporting a single network configuration.
static constexpr char kNetworkIdentityBlobRef[]   = "blob://pdc-ni";
static constexpr char kClientIdentityBlobRef[]    = "blob://pdc-ci";
static constexpr char kClientIdentityKeyBlobRef[] = "blob://pdc-cik";

CHIP_ERROR ConnectivityManagerImpl::ConnectWiFiNetworkWithPDCAsync(
    ByteSpan ssid, ByteSpan networkIdentity, ByteSpan clientIdentity, const Crypto::P256Keypair & clientIdentityKeypair,
    NetworkCommissioning::Internal::WirelessDriver::ConnectCallback * connectCallback)
{
    VerifyOrReturnError(ssid.size() <= kMaxWiFiSSIDLength, CHIP_ERROR_INVALID_ARGUMENT);

    std::lock_guard<std::mutex> lock(mWpaSupplicantMutex);
    VerifyOrReturnError(mWpaSupplicant.iface != nullptr, CHIP_ERROR_INCORRECT_STATE);

    // There is another ongoing connect request, reject the new one.
    VerifyOrReturnError(mpConnectCallback == nullptr, CHIP_ERROR_INCORRECT_STATE);

    // Convert identities and our key pair to DER and add them to wpa_supplicant as blobs
    {
        constexpr size_t bufferSize = std::max(kMaxDERCertLength, kP256ECPrivateKeyDERLength);
        Platform::ScopedMemoryBuffer<uint8_t> buffer;
        VerifyOrReturnError(buffer.Alloc(bufferSize), CHIP_ERROR_NO_MEMORY);

        MutableByteSpan networkIdentityDER(buffer.Get(), bufferSize);
        ReturnErrorOnFailure(ConvertChipCertToX509Cert(networkIdentity, networkIdentityDER));
        ReturnErrorOnFailure(AddOrReplaceBlob(mWpaSupplicant.iface, kNetworkIdentityBlobRef, networkIdentityDER));

        MutableByteSpan clientIdentityDER(buffer.Get(), bufferSize);
        ReturnErrorOnFailure(ConvertChipCertToX509Cert(clientIdentity, clientIdentityDER));
        ReturnErrorOnFailure(AddOrReplaceBlob(mWpaSupplicant.iface, kClientIdentityBlobRef, clientIdentityDER));

        Crypto::P256SerializedKeypair serializedKeypair;
        MutableByteSpan clientIdentityKeypairDER(buffer.Get(), bufferSize);
        ReturnErrorOnFailure(clientIdentityKeypair.Serialize(serializedKeypair));
        ReturnErrorOnFailure(ConvertECDSAKeypairRawToDER(serializedKeypair, clientIdentityKeypairDER));
        ReturnErrorOnFailure(AddOrReplaceBlob(mWpaSupplicant.iface, kClientIdentityKeyBlobRef, clientIdentityKeypairDER));
    }

    // Build the network configuration
    GVariantBuilder builder;
    g_variant_builder_init(&builder, G_VARIANT_TYPE_VARDICT);

    {
        char ssidStr[kMaxWiFiSSIDLength + 1] = { 0 };
        memcpy(ssidStr, ssid.data(), ssid.size());
        g_variant_builder_add(&builder, "{sv}", "ssid", g_variant_new_string(ssidStr));
    }

    {
        CertificateKeyIdStorage keyId;
        ReturnErrorOnFailure(ExtractIdentifierFromChipNetworkIdentity(networkIdentity, keyId));

        static constexpr char kNAIDomain[] = ".pdc.csa-iot.org";
        static constexpr auto keyIdHexSize = keyId.size() * 2;
        char identityStr[1 + keyIdHexSize + sizeof(kNAIDomain)]; // sizeof(kNAIDomain) includes null terminator

        identityStr[0] = '@';
        ReturnErrorOnFailure(Encoding::BytesToUppercaseHexBuffer(keyId.data(), keyId.size(), &identityStr[1], keyIdHexSize));
        strcpy(&identityStr[1 + keyIdHexSize], kNAIDomain);
        g_variant_builder_add(&builder, "{sv}", "identity", g_variant_new_string(identityStr));
    }

    // The configuration will become simpler once we add explicit Matter support to wpa_supplicant
    g_variant_builder_add(&builder, "{sv}", "key_mgmt", g_variant_new_string("WPA-EAP-SHA256"));
    g_variant_builder_add(&builder, "{sv}", "fallback_key_mgmt", g_variant_new_string("WPA-EAP-SHA256"));
    g_variant_builder_add(&builder, "{sv}", "pairwise", g_variant_new_string("CCMP"));
    g_variant_builder_add(&builder, "{sv}", "group", g_variant_new_string("CCMP"));
    g_variant_builder_add(&builder, "{sv}", "ieee80211w", g_variant_new_int32(2));
    g_variant_builder_add(&builder, "{sv}", "eap", g_variant_new_string("TLS"));
    g_variant_builder_add(&builder, "{sv}", "eap_workaround", g_variant_new_int32(0));

    g_variant_builder_add(
        &builder, "{sv}", "phase1",
        g_variant_new_string("tls_disable_tlsv1_0=1,tls_disable_tlsv1_1=1,tls_disable_tlsv1_2=1,tls_disable_tlsv1_3=0"));
    g_variant_builder_add(&builder, "{sv}", "openssl_ciphers", g_variant_new_string("TLS_AES_128_CCM_SHA256"));
    g_variant_builder_add(&builder, "{sv}", "openssl_ecdh_curves", g_variant_new_string("P-256"));

    g_variant_builder_add(&builder, "{sv}", "ca_cert", g_variant_new_string(kNetworkIdentityBlobRef));
    g_variant_builder_add(&builder, "{sv}", "client_cert", g_variant_new_string(kClientIdentityBlobRef));
    g_variant_builder_add(&builder, "{sv}", "private_key", g_variant_new_string(kClientIdentityKeyBlobRef));
    GVariant * args = g_variant_builder_end(&builder);
    return _ConnectWiFiNetworkAsync(args, connectCallback);
}
#endif // CHIP_DEVICE_CONFIG_ENABLE_WIFI_PDC

void ConnectivityManagerImpl::_ConnectWiFiNetworkAsyncCallback(GObject * source_object, GAsyncResult * res, gpointer user_data)
{
    ConnectivityManagerImpl * this_ = reinterpret_cast<ConnectivityManagerImpl *>(user_data);
    GAutoPtr<GError> err;

    std::lock_guard<std::mutex> lock(mWpaSupplicantMutex);

    {
        gboolean result = wpa_fi_w1_wpa_supplicant1_interface_call_select_network_finish(mWpaSupplicant.iface, res,
                                                                                         &MakeUniquePointerReceiver(err).Get());
        if (!result)
        {
            ChipLogError(DeviceLayer, "Failed to perform connect network: %s", err == nullptr ? "unknown error" : err->message);
            DeviceLayer::SystemLayer().ScheduleLambda([this_]() {
                if (mpConnectCallback != nullptr)
                {
                    // TODO(#14175): Replace this with actual thread attach result.
                    this_->mpConnectCallback->OnResult(NetworkCommissioning::Status::kUnknownError, CharSpan(), 0);
                    this_->mpConnectCallback = nullptr;
                }
                mpConnectCallback = nullptr;
            });

            return;
        }

        result = wpa_fi_w1_wpa_supplicant1_interface_call_save_config_sync(mWpaSupplicant.iface, nullptr,
                                                                           &MakeUniquePointerReceiver(err).Get());
        if (result)
        {
            ChipLogProgress(DeviceLayer, "wpa_supplicant: save config succeeded!");
        }
        else
        {
            ChipLogProgress(DeviceLayer, "wpa_supplicant: failed to save config: %s", err ? err->message : "unknown error");
        }
    }
}

void ConnectivityManagerImpl::PostNetworkConnect()
{
    // Iterate on the network interface to see if we already have beed assigned addresses.
    // The temporary hack for getting IP address change on linux for network provisioning in the rendezvous session.
    // This should be removed or find a better place once we depercate the rendezvous session.
    for (chip::Inet::InterfaceAddressIterator it; it.HasCurrent(); it.Next())
    {
        char ifName[chip::Inet::InterfaceId::kMaxIfNameLength];
        if (it.IsUp() && CHIP_NO_ERROR == it.GetInterfaceName(ifName, sizeof(ifName)) &&
            strncmp(ifName, sWiFiIfName, sizeof(ifName)) == 0)
        {
            chip::Inet::IPAddress addr;
            if ((it.GetAddress(addr) == CHIP_NO_ERROR) && addr.IsIPv4())
            {
                ChipDeviceEvent event;
                event.Type                                 = DeviceEventType::kInternetConnectivityChange;
                event.InternetConnectivityChange.IPv4      = kConnectivity_Established;
                event.InternetConnectivityChange.IPv6      = kConnectivity_NoChange;
                event.InternetConnectivityChange.ipAddress = addr;

                char ipStrBuf[chip::Inet::IPAddress::kMaxStringLength] = { 0 };
                addr.ToString(ipStrBuf);

                ChipLogDetail(DeviceLayer, "Got IP address on interface: %s IP: %s", ifName, ipStrBuf);

                PlatformMgr().PostEventOrDie(&event);
            }
        }
    }

#if defined(CHIP_DEVICE_CONFIG_LINUX_DHCPC_CMD)
    // CHIP_DEVICE_CONFIG_LINUX_DHCPC_CMD can be defined to a command pattern
    // to run once the network has been connected, with a %s placeholder for the
    // interface name. E.g. "dhclient -nw %s"
    // Run dhclient for IP on WiFi.
    // TODO: The wifi can be managed by networkmanager on linux so we don't have to care about this.
    char cmdBuffer[128];
    sprintf(cmdBuffer, CHIP_DEVICE_CONFIG_LINUX_DHCPC_CMD, sWiFiIfName);
    int dhclientSystemRet = system(cmdBuffer);
    if (dhclientSystemRet != 0)
    {
        ChipLogError(DeviceLayer, "Failed to run dhclient, system() returns %d", dhclientSystemRet);
    }
    else
    {
        ChipLogProgress(DeviceLayer, "dhclient is running on the %s interface.", sWiFiIfName);
    }
#endif // defined(CHIP_DEVICE_CONFIG_LINUX_DHCPC_CMD)
}

CHIP_ERROR ConnectivityManagerImpl::CommitConfig()
{
    gboolean result;
    GAutoPtr<GError> err;

    std::lock_guard<std::mutex> lock(mWpaSupplicantMutex);

    if (mWpaSupplicant.state != GDBusWpaSupplicant::WPA_INTERFACE_CONNECTED)
    {
        ChipLogError(DeviceLayer, "wpa_supplicant: CommitConfig: interface proxy not connected");
        return CHIP_ERROR_INCORRECT_STATE;
    }

    ChipLogProgress(DeviceLayer, "wpa_supplicant: save config");

    result = wpa_fi_w1_wpa_supplicant1_interface_call_save_config_sync(mWpaSupplicant.iface, nullptr,
                                                                       &MakeUniquePointerReceiver(err).Get());

    if (!result)
    {
        ChipLogProgress(DeviceLayer, "wpa_supplicant: failed to save config: %s", err ? err->message : "unknown error");
        return CHIP_ERROR_INTERNAL;
    }

    ChipLogProgress(DeviceLayer, "wpa_supplicant: save config succeeded!");
    return CHIP_NO_ERROR;
}

CHIP_ERROR ConnectivityManagerImpl::GetWiFiBssId(MutableByteSpan & value)
{
    constexpr size_t bssIdSize = 6;
    static_assert(kMaxHardwareAddrSize >= bssIdSize, "We are assuming we can fit a BSSID in a buffer of size kMaxHardwareAddrSize");
    VerifyOrReturnError(value.size() >= bssIdSize, CHIP_ERROR_BUFFER_TOO_SMALL);

    CHIP_ERROR err          = CHIP_ERROR_READ_FAILED;
    struct ifaddrs * ifaddr = nullptr;

    // On Linux simulation, we don't have the DBus API to get the BSSID of connected AP. Use mac address
    // of local WiFi network card instead.
    if (getifaddrs(&ifaddr) == -1)
    {
        ChipLogError(DeviceLayer, "Failed to get network interfaces");
    }
    else
    {
        // Walk through linked list, maintaining head pointer so we can free list later.
        for (struct ifaddrs * ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next)
        {
            if (ConnectivityUtils::GetInterfaceConnectionType(ifa->ifa_name) == InterfaceTypeEnum::kWiFi)
            {
                if (ConnectivityUtils::GetInterfaceHardwareAddrs(ifa->ifa_name, value.data(), kMaxHardwareAddrSize) !=
                    CHIP_NO_ERROR)
                {
                    ChipLogError(DeviceLayer, "Failed to get WiFi network hardware address");
                }
                else
                {
                    // Set 48-bit IEEE MAC Address
                    value.reduce_size(bssIdSize);
                    err = CHIP_NO_ERROR;
                    break;
                }
            }
        }

        freeifaddrs(ifaddr);
    }

    return err;
}

CHIP_ERROR ConnectivityManagerImpl::GetWiFiSecurityType(SecurityTypeEnum & securityType)
{
    const gchar * mode = nullptr;

    std::lock_guard<std::mutex> lock(mWpaSupplicantMutex);

    if (mWpaSupplicant.state != GDBusWpaSupplicant::WPA_INTERFACE_CONNECTED)
    {
        ChipLogError(DeviceLayer, "wpa_supplicant: GetWiFiSecurityType: interface proxy not connected");
        return CHIP_ERROR_INCORRECT_STATE;
    }

    mode = wpa_fi_w1_wpa_supplicant1_interface_get_current_auth_mode(mWpaSupplicant.iface);
    ChipLogProgress(DeviceLayer, "wpa_supplicant: current Wi-Fi security type: %s", StringOrNullMarker(mode));

    if (strncmp(mode, "WPA-PSK", 7) == 0)
    {
        securityType = SecurityTypeEnum::kWpa;
    }
    else if (strncmp(mode, "WPA2-PSK", 8) == 0)
    {
        securityType = SecurityTypeEnum::kWpa2;
    }
    else if (strncmp(mode, "WPA2-EAP", 8) == 0)
    {
        securityType = SecurityTypeEnum::kWpa2;
    }
    else if (strncmp(mode, "WPA3-PSK", 8) == 0)
    {
        securityType = SecurityTypeEnum::kWpa3;
    }
    else if (strncmp(mode, "WEP", 3) == 0)
    {
        securityType = SecurityTypeEnum::kWep;
    }
    else if (strncmp(mode, "NONE", 4) == 0)
    {
        securityType = SecurityTypeEnum::kNone;
    }
    else if (strncmp(mode, "WPA-NONE", 8) == 0)
    {
        securityType = SecurityTypeEnum::kNone;
    }
    else
    {
        securityType = SecurityTypeEnum::kUnspecified;
    }

    return CHIP_NO_ERROR;
}

CHIP_ERROR ConnectivityManagerImpl::GetWiFiVersion(WiFiVersionEnum & wiFiVersion)
{
    // We don't have direct API to get the WiFi version yet, return 802.11n on Linux simulation.
    wiFiVersion = WiFiVersionEnum::kN;

    return CHIP_NO_ERROR;
}

int32_t ConnectivityManagerImpl::GetDisconnectReason()
{
    std::lock_guard<std::mutex> lock(mWpaSupplicantMutex);
    GAutoPtr<GError> err;

    gint errorValue = wpa_fi_w1_wpa_supplicant1_interface_get_disconnect_reason(mWpaSupplicant.iface);
    // wpa_supplicant DBus API: DisconnectReason: The most recent IEEE 802.11 reason code for disconnect. Negative value
    // indicates locally generated disconnection.
    return errorValue;
}

CHIP_ERROR ConnectivityManagerImpl::GetConfiguredNetwork(NetworkCommissioning::Network & network)
{
    // This function can be called without g_main_context_get_thread_default() being set.
    // The network proxy object is created in a synchronous manner, so the D-Bus call will
    // be completed before this function returns. Also, no external callbacks are registered
    // with the proxy object.

    std::lock_guard<std::mutex> lock(mWpaSupplicantMutex);
    GAutoPtr<GError> err;

    if (mWpaSupplicant.iface == nullptr)
    {
        ChipLogDetail(DeviceLayer, "Wifi network not currently connected");
        return CHIP_ERROR_INCORRECT_STATE;
    }

    const gchar * networkPath = wpa_fi_w1_wpa_supplicant1_interface_get_current_network(mWpaSupplicant.iface);

    // wpa_supplicant DBus API: if network path of current network is "/", means no networks is currently selected.
    if (strcmp(networkPath, "/") == 0)
    {
        return CHIP_ERROR_KEY_NOT_FOUND;
    }

    GAutoPtr<WpaFiW1Wpa_supplicant1Network> networkInfo(wpa_fi_w1_wpa_supplicant1_network_proxy_new_for_bus_sync(
        G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, kWpaSupplicantServiceName, networkPath, nullptr,
        &MakeUniquePointerReceiver(err).Get()));
    if (networkInfo == nullptr)
    {
        return CHIP_ERROR_INTERNAL;
    }

    network.connected     = wpa_fi_w1_wpa_supplicant1_network_get_enabled(networkInfo.get());
    GVariant * properties = wpa_fi_w1_wpa_supplicant1_network_get_properties(networkInfo.get());
    GAutoPtr<GVariant> ssid(g_variant_lookup_value(properties, "ssid", nullptr));
    gsize length;
    const gchar * ssidStr = g_variant_get_string(ssid.get(), &length);
    // TODO: wpa_supplicant will return ssid with quotes! We should have a better way to get the actual ssid in bytes.
    gsize length_actual = length - 2;
    VerifyOrReturnError(length_actual <= sizeof(network.networkID), CHIP_ERROR_INTERNAL);
    ChipLogDetail(DeviceLayer, "Current connected network: %s", StringOrNullMarker(ssidStr));
    memcpy(network.networkID, ssidStr + 1, length_actual);
    network.networkIDLen = length_actual;
    return CHIP_NO_ERROR;
}

CHIP_ERROR ConnectivityManagerImpl::StopAutoScan()
{
    std::lock_guard<std::mutex> lock(mWpaSupplicantMutex);
    VerifyOrReturnError(mWpaSupplicant.iface != nullptr, CHIP_ERROR_INCORRECT_STATE);

    GAutoPtr<GError> err;
    gboolean result;

    ChipLogDetail(DeviceLayer, "wpa_supplicant: disabling auto scan");

    result = wpa_fi_w1_wpa_supplicant1_interface_call_auto_scan_sync(
        mWpaSupplicant.iface, "" /* empty string means disabling auto scan */, nullptr, &MakeUniquePointerReceiver(err).Get());
    if (!result)
    {
        ChipLogError(DeviceLayer, "wpa_supplicant: Failed to stop auto network scan: %s", err ? err->message : "unknown");
        return CHIP_ERROR_INTERNAL;
    }
    return CHIP_NO_ERROR;
}

CHIP_ERROR ConnectivityManagerImpl::StartWiFiScan(ByteSpan ssid, WiFiDriver::ScanCallback * callback)
{
    std::lock_guard<std::mutex> lock(mWpaSupplicantMutex);
    VerifyOrReturnError(mWpaSupplicant.iface != nullptr, CHIP_ERROR_INCORRECT_STATE);
    // There is another ongoing scan request, reject the new one.
    VerifyOrReturnError(mpScanCallback == nullptr, CHIP_ERROR_INCORRECT_STATE);
    VerifyOrReturnError(ssid.size() <= sizeof(sInterestedSSID), CHIP_ERROR_INVALID_ARGUMENT);

    CHIP_ERROR ret = CHIP_NO_ERROR;
    GAutoPtr<GError> err;
    GVariant * args = nullptr;
    GVariantBuilder builder;
    gboolean result;

    memcpy(sInterestedSSID, ssid.data(), ssid.size());
    sInterestedSSIDLen = ssid.size();

    g_variant_builder_init(&builder, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&builder, "{sv}", "Type", g_variant_new_string("active"));
    args = g_variant_builder_end(&builder);

    result = wpa_fi_w1_wpa_supplicant1_interface_call_scan_sync(mWpaSupplicant.iface, args, nullptr,
                                                                &MakeUniquePointerReceiver(err).Get());

    if (result)
    {
        ChipLogProgress(DeviceLayer, "wpa_supplicant: initialized network scan.");
        mpScanCallback = callback;
    }
    else
    {
        ChipLogProgress(DeviceLayer, "wpa_supplicant: failed to start network scan: %s", err ? err->message : "unknown error");
        ret = CHIP_ERROR_INTERNAL;
    }

    return ret;
}

namespace {

// wpa_supplicant's scan results don't contains the channel infomation, so we need this lookup table for resolving the band and
// channel infomation.
std::pair<WiFiBand, uint16_t> GetBandAndChannelFromFrequency(uint32_t freq)
{
    std::pair<WiFiBand, uint16_t> ret = std::make_pair(WiFiBand::k2g4, 0);
    if (freq <= 931)
    {
        ret.first = WiFiBand::k1g;
        if (freq >= 916)
        {
            ret.second = ((freq - 916) * 2) - 1;
        }
        else if (freq >= 902)
        {
            ret.second = (freq - 902) * 2;
        }
        else if (freq >= 863)
        {
            ret.second = (freq - 863) * 2;
        }
        else
        {
            ret.second = 1;
        }
    }
    else if (freq <= 2472)
    {
        ret.second = static_cast<uint16_t>((freq - 2412) / 5 + 1);
    }
    else if (freq == 2484)
    {
        ret.second = 14;
    }
    else if (freq >= 3600 && freq <= 3700)
    {
        // Note: There are not many devices supports this band, and this band contains rational frequency in MHz, need to figure out
        // the behavior of wpa_supplicant in this case.
        ret.first = WiFiBand::k3g65;
    }
    else if (freq >= 5035 && freq <= 5945)
    {
        ret.first  = WiFiBand::k5g;
        ret.second = static_cast<uint16_t>((freq - 5000) / 5);
    }
    else if (freq == 5960 || freq == 5980)
    {
        ret.first  = WiFiBand::k5g;
        ret.second = static_cast<uint16_t>((freq - 5000) / 5);
    }
    else if (freq >= 5955)
    {
        ret.first  = WiFiBand::k6g;
        ret.second = static_cast<uint16_t>((freq - 5950) / 5);
    }
    else if (freq >= 58000)
    {
        ret.first = WiFiBand::k60g;
        // Note: Some channel has the same center frequency but different bandwidth. Should figure out wpa_supplicant's behavior in
        // this case. Also, wpa_supplicant's frequency property is uint16 infact.
        switch (freq)
        {
        case 58'320:
            ret.second = 1;
            break;
        case 60'480:
            ret.second = 2;
            break;
        case 62'640:
            ret.second = 3;
            break;
        case 64'800:
            ret.second = 4;
            break;
        case 66'960:
            ret.second = 5;
            break;
        case 69'120:
            ret.second = 6;
            break;
        case 59'400:
            ret.second = 9;
            break;
        case 61'560:
            ret.second = 10;
            break;
        case 63'720:
            ret.second = 11;
            break;
        case 65'880:
            ret.second = 12;
            break;
        case 68'040:
            ret.second = 13;
            break;
        }
    }
    return ret;
}

} // namespace

bool ConnectivityManagerImpl::_GetBssInfo(const gchar * bssPath, NetworkCommissioning::WiFiScanResponse & result)
{
    // This function can be called without g_main_context_get_thread_default() being set.
    // The BSS proxy object is created in a synchronous manner, so the D-Bus call will be
    // completed before this function returns. Also, no external callbacks are registered
    // with the proxy object.

    GAutoPtr<GError> err;
    GAutoPtr<WpaFiW1Wpa_supplicant1BSS> bss(
        wpa_fi_w1_wpa_supplicant1_bss_proxy_new_for_bus_sync(G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, kWpaSupplicantServiceName,
                                                             bssPath, nullptr, &MakeUniquePointerReceiver(err).Get()));

    if (bss == nullptr)
    {
        return false;
    }

    WpaFiW1Wpa_supplicant1BSSProxy * bssProxy = WPA_FI_W1_WPA_SUPPLICANT1_BSS_PROXY(bss.get());

    GAutoPtr<GVariant> ssid(g_dbus_proxy_get_cached_property(G_DBUS_PROXY(bssProxy), "SSID"));
    GAutoPtr<GVariant> bssid(g_dbus_proxy_get_cached_property(G_DBUS_PROXY(bssProxy), "BSSID"));

    // Network scan is performed in the background, so the BSS
    // may be gone when we try to get the properties.
    if (ssid == nullptr || bssid == nullptr)
    {
        ChipLogDetail(DeviceLayer, "wpa_supplicant: BSS not found: %s", StringOrNullMarker(bssPath));
        return false;
    }

    const guchar * ssidStr       = nullptr;
    const guchar * bssidBuf      = nullptr;
    char bssidStr[2 * 6 + 5 + 1] = { 0 };
    gsize ssidLen                = 0;
    gsize bssidLen               = 0;
    gint16 signal                = wpa_fi_w1_wpa_supplicant1_bss_get_signal(bss.get());
    guint16 frequency            = wpa_fi_w1_wpa_supplicant1_bss_get_frequency(bss.get());

    ssidStr  = reinterpret_cast<const guchar *>(g_variant_get_fixed_array(ssid.get(), &ssidLen, sizeof(guchar)));
    bssidBuf = reinterpret_cast<const guchar *>(g_variant_get_fixed_array(bssid.get(), &bssidLen, sizeof(guchar)));

    if (bssidLen == 6)
    {
        snprintf(bssidStr, sizeof(bssidStr), "%02x:%02x:%02x:%02x:%02x:%02x", bssidBuf[0], bssidBuf[1], bssidBuf[2], bssidBuf[3],
                 bssidBuf[4], bssidBuf[5]);
    }
    else
    {
        bssidLen = 0;
        ChipLogError(DeviceLayer, "Got a network with bssid not equals to 6");
    }
    ChipLogDetail(DeviceLayer, "Network Found: %.*s (%s) Signal:%d", int(ssidLen), StringOrNullMarker((const gchar *) ssidStr),
                  bssidStr, signal);

    // A flag for enterprise encryption option to avoid returning open for these networks by mistake
    // TODO: The following code will mistakenly recognize WEP encryption as OPEN network, this should be fixed by reading
    // IEs (information elements) field instead of reading cooked data.

    static constexpr uint8_t kEAP = (1 << 7);

    auto IsNetworkWPAPSK = [](GVariant * wpa) -> uint8_t {
        if (wpa == nullptr)
        {
            return 0;
        }

        GAutoPtr<GVariant> keyMgmt(g_variant_lookup_value(wpa, "KeyMgmt", nullptr));
        if (keyMgmt == nullptr)
        {
            return 0;
        }
        GAutoPtr<const char *> keyMgmts(g_variant_get_strv(keyMgmt.get(), nullptr));
        const gchar ** keyMgmtsHendle = keyMgmts.get();
        uint8_t res                   = 0;

        VerifyOrReturnError(keyMgmtsHendle != nullptr, res);

        for (auto keyMgmtVal = *keyMgmtsHendle; keyMgmtVal != nullptr; keyMgmtVal = *(++keyMgmtsHendle))
        {
            if (g_strcasecmp(keyMgmtVal, "wpa-psk") == 0 || g_strcasecmp(keyMgmtVal, "wpa-none") == 0)
            {
                res |= (1 << 2); // SecurityType::WPA_PERSONAL
            }
            else if (g_strcasecmp(keyMgmtVal, "wpa-eap"))
            {
                res |= (kEAP);
            }
        }

        return res;
    };
    auto IsNetworkWPA2PSK = [](GVariant * rsn) -> uint8_t {
        if (rsn == nullptr)
        {
            return 0;
        }
        GAutoPtr<GVariant> keyMgmt(g_variant_lookup_value(rsn, "KeyMgmt", nullptr));
        if (keyMgmt == nullptr)
        {
            return 0;
        }
        GAutoPtr<const char *> keyMgmts(g_variant_get_strv(keyMgmt.get(), nullptr));
        const gchar ** keyMgmtsHendle = keyMgmts.get();
        uint8_t res                   = 0;

        VerifyOrReturnError(keyMgmtsHendle != nullptr, res);

        for (auto keyMgmtVal = *keyMgmtsHendle; keyMgmtVal != nullptr; keyMgmtVal = *(++keyMgmtsHendle))
        {
            if (g_strcasecmp(keyMgmtVal, "wpa-psk") == 0 || g_strcasecmp(keyMgmtVal, "wpa-psk-sha256") == 0 ||
                g_strcasecmp(keyMgmtVal, "wpa-ft-psk") == 0)
            {
                res |= (1 << 3); // SecurityType::WPA2_PERSONAL
            }
            else if (g_strcasecmp(keyMgmtVal, "wpa-eap") == 0 || g_strcasecmp(keyMgmtVal, "wpa-eap-sha256") == 0 ||
                     g_strcasecmp(keyMgmtVal, "wpa-ft-eap") == 0)
            {
                res |= kEAP;
            }
            else if (g_strcasecmp(keyMgmtVal, "sae") == 0)
            {
                // wpa_supplicant will include "sae" in KeyMgmt field for WPA3 WiFi, this is not included in the wpa_supplicant
                // document.
                res |= (1 << 4); // SecurityType::WPA3_PERSONAL
            }
        }

        return res;
    };
    auto GetNetworkSecurityType = [IsNetworkWPAPSK, IsNetworkWPA2PSK](WpaFiW1Wpa_supplicant1BSSProxy * proxy) -> uint8_t {
        GAutoPtr<GVariant> wpa(g_dbus_proxy_get_cached_property(G_DBUS_PROXY(proxy), "WPA"));
        GAutoPtr<GVariant> rsn(g_dbus_proxy_get_cached_property(G_DBUS_PROXY(proxy), "RSN"));

        uint8_t res = IsNetworkWPAPSK(wpa.get()) | IsNetworkWPA2PSK(rsn.get());
        if (res == 0)
        {
            res = 1; // Open
        }
        return res & (0x7F);
    };

    // Drop the network if its SSID or BSSID is illegal.
    VerifyOrReturnError(ssidLen <= kMaxWiFiSSIDLength, false);
    VerifyOrReturnError(bssidLen == kWiFiBSSIDLength, false);
    memcpy(result.ssid, ssidStr, ssidLen);
    memcpy(result.bssid, bssidBuf, bssidLen);
    result.ssidLen = ssidLen;
    if (signal < INT8_MIN)
    {
        result.rssi = INT8_MIN;
    }
    else if (signal > INT8_MAX)
    {
        result.rssi = INT8_MAX;
    }
    else
    {
        result.rssi = static_cast<uint8_t>(signal);
    }

    auto bandInfo   = GetBandAndChannelFromFrequency(frequency);
    result.wiFiBand = bandInfo.first;
    result.channel  = bandInfo.second;
    result.security.SetRaw(GetNetworkSecurityType(bssProxy));

    return true;
}

void ConnectivityManagerImpl::_OnWpaInterfaceScanDone(GObject * source_object, GAsyncResult * res, gpointer user_data)
{
    std::lock_guard<std::mutex> lock(mWpaSupplicantMutex);

    ChipLogProgress(DeviceLayer, "wpa_supplicant: network scan done");
    gchar ** bsss    = wpa_fi_w1_wpa_supplicant1_interface_dup_bsss(mWpaSupplicant.iface);
    gchar ** oldBsss = bsss;
    if (bsss == nullptr)
    {
        ChipLogProgress(DeviceLayer, "wpa_supplicant: no network found");
        DeviceLayer::SystemLayer().ScheduleLambda([]() {
            if (mpScanCallback != nullptr)
            {
                mpScanCallback->OnFinished(Status::kSuccess, CharSpan(), nullptr);
                mpScanCallback = nullptr;
            }
        });
        return;
    }

    std::vector<WiFiScanResponse> * networkScanned = new std::vector<WiFiScanResponse>();
    for (const gchar * bssPath = (bsss != nullptr ? *bsss : nullptr); bssPath != nullptr; bssPath = *(++bsss))
    {
        WiFiScanResponse network;
        if (_GetBssInfo(bssPath, network))
        {
            if (sInterestedSSIDLen == 0)
            {
                networkScanned->push_back(network);
            }
            else if (network.ssidLen == sInterestedSSIDLen && memcmp(network.ssid, sInterestedSSID, sInterestedSSIDLen) == 0)
            {
                networkScanned->push_back(network);
            }
        }
    }

    DeviceLayer::SystemLayer().ScheduleLambda([networkScanned]() {
        // Note: We cannot post a event in ScheduleLambda since std::vector is not trivial copiable. This results in the use of
        // const_cast but should be fine for almost all cases, since we actually handled the ownership of this element to this
        // lambda.
        if (mpScanCallback != nullptr)
        {
            LinuxScanResponseIterator<WiFiScanResponse> iter(const_cast<std::vector<WiFiScanResponse> *>(networkScanned));
            mpScanCallback->OnFinished(Status::kSuccess, CharSpan(), &iter);
            mpScanCallback = nullptr;
        }

        delete const_cast<std::vector<WiFiScanResponse> *>(networkScanned);
    });

    g_strfreev(oldBsss);
}

CHIP_ERROR ConnectivityManagerImpl::_StartWiFiManagement(ConnectivityManagerImpl * self)
{
    // When creating D-Bus proxy object, the thread default context must be initialized. Otherwise,
    // all D-Bus signals will be delivered to the GLib global default main context.
    VerifyOrDie(g_main_context_get_thread_default() != nullptr);

    ChipLogProgress(DeviceLayer, "wpa_supplicant: Start WiFi management");
    wpa_fi_w1_wpa_supplicant1_proxy_new_for_bus(G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, kWpaSupplicantServiceName,
                                                kWpaSupplicantObjectPath, nullptr, self->_OnWpaProxyReady, nullptr);

    return CHIP_NO_ERROR;
}

#endif // CHIP_DEVICE_CONFIG_ENABLE_WPA

} // namespace DeviceLayer
} // namespace chip
