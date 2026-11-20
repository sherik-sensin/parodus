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
 * @file wan_interface_rbus.c
 *
 * @description Subscribe to WanManager active WAN interface and keep
 *              parodus webpa_interface_used in sync at runtime.
 *
 */

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <rbus.h>
#include "wan_interface_rbus.h"
#include "config.h"
#include "ParodusInternal.h"
#include "close_retry.h"
#include "connection.h"
#include "heartBeat.h"

/*----------------------------------------------------------------------------*/
/*                                   Macros                                   */
/*----------------------------------------------------------------------------*/

#define WAN_ACTIVE_INTERFACE_PARAM "Device.X_RDK_WanManager.CurrentActiveInterface"
#define SUBSCRIBE_RETRY_MAX        3
#define WAN_IFNAME_BUF_SIZE        16  /* matches ParodusCfg.webpa_interface_used */

/*----------------------------------------------------------------------------*/
/*                            File Scoped Variables                           */
/*----------------------------------------------------------------------------*/

static rbusHandle_t g_rbus_handle = NULL;

/*----------------------------------------------------------------------------*/
/*                             Function Prototypes                            */
/*----------------------------------------------------------------------------*/

static void subscribeAsyncHandler(rbusHandle_t handle,
                                  rbusEventSubscription_t *subscription,
                                  rbusError_t error);
static void eventReceiveHandler(rbusHandle_t handle,
                                rbusEvent_t const *event,
                                rbusEventSubscription_t *subscription);
static void applyActiveWanInterface(const char *interface, int trigger_reconnect);
static void fetchAndApplyCurrentActiveInterface(void);

/*----------------------------------------------------------------------------*/
/*                             Internal functions                             */
/*----------------------------------------------------------------------------*/

/**
 * @brief Apply active WAN interface name into parodus config.
 *
 * On runtime change, trigger cloud reconnect so the new IF is used.
 *
 * Concurrency: previous IF is copied under config_mut; setWebpaInterface()
 * is also mutex-protected. Reconnect uses existing close-retry helpers.
 * Input validation: reject NULL/empty/oversized interface names.
 *
 * @param[in] interface          Active WAN interface name from WanManager.
 * @param[in] trigger_reconnect  Non-zero to force cloud reconnect after update.
 */
static void applyActiveWanInterface(const char *interface, int trigger_reconnect)
{
    char previous[WAN_IFNAME_BUF_SIZE];

    if (interface == NULL || interface[0] == '\0') {
        ParodusError("Invalid WAN interface, ignoring update\n");
        return;
    }

    if (strlen(interface) >= WAN_IFNAME_BUF_SIZE) {
        ParodusError("WAN interface name too long: %s\n", interface);
        return;
    }

    previous[0] = '\0';
    pthread_mutex_lock(&config_mut);
    parStrncpy(previous, get_parodus_cfg()->webpa_interface_used, sizeof(previous));
    pthread_mutex_unlock(&config_mut);

    if ((previous[0] != '\0') && (strcmp(previous, interface) == 0)) {
        ParodusInfo("WAN interface unchanged: %s\n", interface);
        return;
    }

    setWebpaInterface(interface);
    ParodusInfo("WAN interface updated: '%s' -> '%s'\n",
                previous[0] ? previous : "(none)", interface);

    if (!trigger_reconnect) {
        return;
    }

    if (get_interface_down_event()) {
        reset_interface_down_event();
        ParodusInfo("Interface_down_event is reset after WAN change\n");
        resume_heartBeatTimer();
    }

    set_global_reconnect_reason("WAN_FAILOVER");
    set_global_reconnect_status(true);
    set_close_retry();
}

/**
 * @brief Read CurrentActiveInterface once and seed webpa_interface_used.
 *
 * Used at startup before the first cloud connection. Does not reconnect.
 */
static void fetchAndApplyCurrentActiveInterface(void)
{
    rbusValue_t value = NULL;
    rbusError_t rc;
    const char *interface = NULL;

    if (g_rbus_handle == NULL) {
        return;
    }

    rc = rbus_get(g_rbus_handle, WAN_ACTIVE_INTERFACE_PARAM, &value);
    if (rc != RBUS_ERROR_SUCCESS || value == NULL) {
        ParodusError("rbus_get %s failed: %d - %s\n",
                     WAN_ACTIVE_INTERFACE_PARAM, rc, rbusError_ToString(rc));
        return;
    }

    interface = rbusValue_GetString(value, NULL);
    if (interface != NULL && interface[0] != '\0') {
        applyActiveWanInterface(interface, 0);
    } else {
        ParodusError("CurrentActiveInterface value is empty\n");
    }

    rbusValue_Release(value);
}

/**
 * @brief RBUS async subscribe completion callback.
 */
static void subscribeAsyncHandler(rbusHandle_t handle,
                                  rbusEventSubscription_t *subscription,
                                  rbusError_t error)
{
    (void)handle;
    if (subscription == NULL) {
        ParodusError("subscribeAsyncHandler: subscription is NULL\n");
        return;
    }

    ParodusInfo("subscribeAsyncHandler event %s, error %d - %s\n",
                subscription->eventName, error, rbusError_ToString(error));
}

/**
 * @brief RBUS value-change handler for CurrentActiveInterface.
 *
 * Updates webpa_interface_used and triggers cloud reconnect.
 */
static void eventReceiveHandler(rbusHandle_t handle,
                                rbusEvent_t const *event,
                                rbusEventSubscription_t *subscription)
{
    rbusValue_t newValue = NULL;
    const char *interface = NULL;

    (void)handle;
    (void)subscription;

    if (event == NULL || event->data == NULL) {
        ParodusError("eventReceiveHandler: invalid event\n");
        return;
    }

    ParodusInfo("ValueChange for %s\n", event->name);
    newValue = rbusObject_GetValue(event->data, "value");
    if (newValue == NULL) {
        ParodusError("eventReceiveHandler: newValue is NULL\n");
        return;
    }

    interface = rbusValue_GetString(newValue, NULL);
    applyActiveWanInterface(interface, 1);
}

/*----------------------------------------------------------------------------*/
/*                             External functions                             */
/*----------------------------------------------------------------------------*/

/**
 * @brief Subscribe to WanManager CurrentActiveInterface and seed webpa_interface_used.
 *
 * Called once during socket connection setup, before createNopollConnection().
 *
 * @return RBUS_ERROR_SUCCESS (0) on success; non-zero rbus error otherwise.
 */
int subscribeCurrentActiveInterfaceEvent(void)
{
    int retryCount = 0;
    int rc = RBUS_ERROR_SUCCESS;

    ParodusInfo("Subscribing to %s\n", WAN_ACTIVE_INTERFACE_PARAM);

    if (g_rbus_handle == NULL) {
        rc = rbus_open(&g_rbus_handle, "parodus");
        if (rc != RBUS_ERROR_SUCCESS) {
            ParodusError("rbus_open failed: %s\n", rbusError_ToString(rc));
            g_rbus_handle = NULL;
            return rc;
        }
    }

    fetchAndApplyCurrentActiveInterface();

    while (retryCount <= SUBSCRIBE_RETRY_MAX) {
        rc = rbusEvent_SubscribeAsync(g_rbus_handle,
                                      WAN_ACTIVE_INTERFACE_PARAM,
                                      eventReceiveHandler,
                                      subscribeAsyncHandler,
                                      "parodusInterface",
                                      10 * 20);
        if (rc == RBUS_ERROR_SUCCESS) {
            ParodusInfo("%s subscribe success\n", WAN_ACTIVE_INTERFACE_PARAM);
            return rc;
        }

        ParodusError("%s subscribe failed: %d - %s\n",
                     WAN_ACTIVE_INTERFACE_PARAM, rc, rbusError_ToString(rc));
        retryCount++;
        sleep(1);
    }

    return rc;
}
