/*********************************************************************************
 If not stated otherwise in this file or this component's Licenses.txt file the
 * following copyright and licenses apply:
 *
 * Copyright 2026 Deutsche Telekom AG.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 ********************************************************************************/
/**
 * @file wan_interface_rbus.h
 *
 * @description APIs to keep parodus WAN interface in sync with
 *              Device.X_RDK_WanManager.CurrentActiveInterface via RBUS.
 *
 */

#ifndef _WAN_INTERFACE_RBUS_H_
#define _WAN_INTERFACE_RBUS_H_

#if defined(FEATURE_RDKB_MULTIWAN)

#ifdef __cplusplus
extern "C" {
#endif

/*----------------------------------------------------------------------------*/
/*                             Function Prototypes                            */
/*----------------------------------------------------------------------------*/

/**
 * @brief Subscribe to WanManager CurrentActiveInterface and seed webpa_interface_used.
 *
 * Opens an RBUS handle (if needed), reads the current active WAN interface,
 * updates webpa_interface_used, then subscribes for value-change events.
 * On a later interface change, webpa_interface_used is updated and a cloud
 * reconnect is triggered so the new interface is used.
 *
 * @return RBUS_ERROR_SUCCESS (0) on successful subscribe; non-zero on failure.
 *         On failure, parodus continues with the existing/configured interface.
 */
int subscribeCurrentActiveInterfaceEvent(void);

#ifdef __cplusplus
}
#endif

#endif /* FEATURE_RDKB_MULTIWAN */

#endif /* _WAN_INTERFACE_RBUS_H_ */
