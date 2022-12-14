/*
 *   Copyright (c) 2022 Martijn van Welie
 *
 *   Permission is hereby granted, free of charge, to any person obtaining a copy
 *   of this software and associated documentation files (the "Software"), to deal
 *   in the Software without restriction, including without limitation the rights
 *   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *   copies of the Software, and to permit persons to whom the Software is
 *   furnished to do so, subject to the following conditions:
 *
 *   The above copyright notice and this permission notice shall be included in all
 *   copies or substantial portions of the Software.
 *
 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *   SOFTWARE.
 *
 */

#include <glib.h>
#include <stdio.h>
#include <signal.h>
#include "adapter.h"
#include "device.h"
#include "logger.h"
#include "agent.h"
#include "application.h"
#include "advertisement.h"
#include "utility.h"
#include "parser.h"

#define TAG "Main"


#define DIS_SERVICE_UUID "0000180a-0000-1000-8000-00805f9b34fb"
#define DIS_MANUFACTURER_UUID "00002a29-0000-1000-8000-00805f9b34fb"
#define DIS_MODEL_NUMBER_UUID "00002a24-0000-1000-8000-00805f9b34fb"

#define GHS_SERVICE_UUID "00007f44-0000-1000-8000-00805f9b34fb"
#define OBSERVATION_CHARACTERISTIC_UUID "00007f43-0000-1000-8000-00805f9b34fb"
#define STORED_OBSERVATIONS_CHARACTERISTIC_UUID "00007f42-0000-1000-8000-00805f9b34fb"
#define GHS_FEATURES_CHARACTERISTIC_UUID "00007f41-0000-1000-8000-00805f9b34fb"
#define GHS_CONTROL_POINT_CHARACTERISTIC_UUID "00007f40-0000-1000-8000-00805f9b34fb"
#define RACP_CHARACTERISTIC_UUID "00002a52-0000-1000-8000-00805f9b34fb"
#define GHS_SCHEDULE_CHANGED_CHAR_UUID "00007f3f-0000-1000-8000-00805f9b34fb"
#define GHS_SCHEDULE_DESCRIPTOR_UUID "00007f35-0000-1000-8000-00805f9b34fb"
#define VALID_RANGE_AND_ACCURACY_DESCRIPTOR_UUID "00007f34-0000-1000-8000-00805f9b34fb"
#define MDC_PULS_OXIM_SAT_O2 150456
#define MDC_PULS_OXIM_PULS_RATE 149530
#define MDC_DIM_PER_CENT 0x0220

#define NUMERIC_OBSERVATION 0


GMainLoop *loop = NULL;
Adapter *default_adapter = NULL;
Advertisement *advertisement = NULL;
Application *app = NULL;
guint observation_timer_ref = 0;

void on_powered_state_changed(Adapter *adapter, gboolean state) {
    log_debug(TAG, "powered '%s' (%s)", state ? "on" : "off", binc_adapter_get_path(adapter));
}

void on_central_state_changed(Adapter *adapter, Device *device) {
    log_debug(TAG, "remote central %s is %s", binc_device_get_address(device), binc_device_get_connection_state_name(device));
    ConnectionState state = binc_device_get_connection_state(device);
    if (state == CONNECTED) {
        binc_adapter_stop_advertising(adapter, advertisement);
    } else if (state == DISCONNECTED){
        binc_adapter_start_advertising(adapter, advertisement);
    }
}

GByteArray *createObservation(float spo2_value) {
    const guint16 length = 25;
    float measurement_duration = 1.0f;
    Parser *parser = parser_create_empty(length, LITTLE_ENDIAN);
    parser_set_uint8(parser, 0x03);
    parser_set_uint8(parser, NUMERIC_OBSERVATION);
    parser_set_uint16(parser, length);
    parser_set_uint16(parser, 0x07); // Flags
    parser_set_uint32(parser, MDC_PULS_OXIM_SAT_O2);
    parser_set_elapsed_time(parser);
    parser_set_float(parser, measurement_duration, 1);
    parser_set_uint16(parser, MDC_DIM_PER_CENT);
    parser_set_float(parser, spo2_value,1);

    GByteArray *result = parser_get_byte_array(parser);
    parser_free(parser);
    return result;
}

void sendObservation(float spo2_value) {
    if (app == NULL) return;

    if (binc_application_char_is_notifying(app, GHS_SERVICE_UUID, OBSERVATION_CHARACTERISTIC_UUID)) {

        GByteArray *observation = createObservation(spo2_value);

        binc_application_notify(
                app,
                GHS_SERVICE_UUID,
                OBSERVATION_CHARACTERISTIC_UUID,
                observation
        );
        g_byte_array_unref(observation);
    }
}

void sendNextObservation() {
    sendObservation(96.1f);
}

void on_local_desc_write_success(const Application *application, const char *address,
                          const char *service_uuid, const char *char_uuid,
                          const char *desc_uuid, const GByteArray *byteArray) {

    if (binc_application_char_is_notifying(application, GHS_SERVICE_UUID, GHS_SCHEDULE_CHANGED_CHAR_UUID)) {

        binc_application_notify(
                application,
                GHS_SERVICE_UUID,
                GHS_SCHEDULE_CHANGED_CHAR_UUID,
                byteArray
        );
    }
}

char *on_local_desc_write(const Application *application, const char *address,
                          const char *service_uuid, const char *char_uuid,
                          const char *desc_uuid, const GByteArray *byteArray) {

    if (byteArray->len != 12) {
        return BLUEZ_ERROR_REJECTED;
    }

    Parser *parser = parser_create((GByteArray*)byteArray, LITTLE_ENDIAN);
    const guint32 mdc = parser_get_uint32(parser);
    const double schedule_measurement_period = parser_get_float(parser);
    const double schedule_update_interval = parser_get_float(parser);
    parser_free(parser);

    if (!(mdc == MDC_PULS_OXIM_SAT_O2 || mdc == MDC_PULS_OXIM_PULS_RATE)) {
        return BLUEZ_ERROR_REJECTED;
    }

    if (schedule_measurement_period > 5 || schedule_measurement_period < 1) {
        return BLUEZ_ERROR_REJECTED;
    }

    if (schedule_update_interval < schedule_measurement_period || schedule_update_interval > 10) {
        return BLUEZ_ERROR_REJECTED;
    }

    return NULL;
}

gboolean observation_timer_expired(gpointer data) {
    log_debug(TAG, "observation timer expired");
    if (app == NULL) return FALSE;

    sendNextObservation();
    return TRUE;
}

void on_local_char_start_notify(const Application *application, const char *service_uuid, const char *char_uuid) {
    log_debug(TAG, "on start notify");
    if (g_str_equal(char_uuid, OBSERVATION_CHARACTERISTIC_UUID)) {
        observation_timer_ref = g_timeout_add_seconds(2, observation_timer_expired, NULL);
    }
}

void on_local_char_stop_notify(const Application *application, const char *service_uuid, const char *char_uuid) {
    log_debug(TAG, "on stop notify");
    if (g_str_equal(char_uuid, OBSERVATION_CHARACTERISTIC_UUID)) {
        g_source_remove(observation_timer_ref);
    }
}

gboolean callback(gpointer data) {
    if (app != NULL) {
        binc_adapter_unregister_application(default_adapter, app);
        binc_application_free(app);
        app = NULL;
    }

    if (advertisement != NULL) {
        binc_adapter_stop_advertising(default_adapter, advertisement);
        binc_advertisement_free(advertisement);
    }

    if (default_adapter != NULL) {
        binc_adapter_free(default_adapter);
        default_adapter = NULL;
    }

    g_main_loop_quit((GMainLoop *) data);
    return FALSE;
}

static void cleanup_handler(int signo) {
    if (signo == SIGINT) {
        log_error(TAG, "received SIGINT");
        callback(loop);
    }
}

int main(void) {
    // Get a DBus connection
    GDBusConnection *dbusConnection = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, NULL);

    // Setup handler for CTRL+C
    if (signal(SIGINT, cleanup_handler) == SIG_ERR)
        log_error(TAG, "can't catch SIGINT");

    // Setup mainloop
    loop = g_main_loop_new(NULL, FALSE);

    // Get the default default_adapter
    default_adapter = binc_adapter_get_default(dbusConnection);

    if (default_adapter != NULL) {
        log_debug(TAG, "using default_adapter '%s'", binc_adapter_get_path(default_adapter));

        // Make sure the adapter is on
        binc_adapter_set_powered_state_cb(default_adapter, &on_powered_state_changed);
        if (!binc_adapter_get_powered_state(default_adapter)) {
            binc_adapter_power_on(default_adapter);
        }

        // Setup remote central connection state callback
        binc_adapter_set_remote_central_cb(default_adapter, &on_central_state_changed);

        // Setup advertisement
        GPtrArray *adv_service_uuids = g_ptr_array_new();
        g_ptr_array_add(adv_service_uuids, GHS_SERVICE_UUID);

        advertisement = binc_advertisement_create();
        binc_advertisement_set_local_name(advertisement, "BINC");
        binc_advertisement_set_services(advertisement, adv_service_uuids);
        g_ptr_array_free(adv_service_uuids, TRUE);
        binc_adapter_start_advertising(default_adapter, advertisement);

        // Setup all services, characteristics and descriptors
        app = binc_create_application(default_adapter);
        binc_application_add_service(app, DIS_SERVICE_UUID);
        binc_application_add_characteristic(
                app,
                DIS_SERVICE_UUID,
                DIS_MANUFACTURER_UUID,
                GATT_CHR_PROP_READ);
        binc_application_add_characteristic(
                app,
                DIS_SERVICE_UUID,
                DIS_MODEL_NUMBER_UUID,
                GATT_CHR_PROP_READ);

        binc_application_add_service(app, GHS_SERVICE_UUID);
        binc_application_add_characteristic(
                app,
                GHS_SERVICE_UUID,
                OBSERVATION_CHARACTERISTIC_UUID,
                GATT_CHR_PROP_INDICATE);

        binc_application_add_characteristic(
                app,
                GHS_SERVICE_UUID,
                GHS_FEATURES_CHARACTERISTIC_UUID,
                GATT_CHR_PROP_READ);

        binc_application_add_characteristic(
                app,
                GHS_SERVICE_UUID,
                GHS_SCHEDULE_CHANGED_CHAR_UUID,
                GATT_CHR_PROP_INDICATE);

        binc_application_add_descriptor(
                app,
                GHS_SERVICE_UUID,
                GHS_FEATURES_CHARACTERISTIC_UUID,
                GHS_SCHEDULE_DESCRIPTOR_UUID,
                GATT_CHR_PROP_READ | GATT_CHR_PROP_WRITE);

        // Set initial value for Observation Schedule Descriptor
        Parser *parser = parser_create_empty(12, LITTLE_ENDIAN);
        parser_set_uint32(parser, MDC_PULS_OXIM_SAT_O2);
        parser_set_float(parser, 1.0f, 1);
        parser_set_float(parser, 1.0f, 1);
        GByteArray *scheduleByteArray = parser_get_byte_array(parser);
        binc_application_set_desc_value(app,
                                        GHS_SERVICE_UUID,
                                        GHS_FEATURES_CHARACTERISTIC_UUID,
                                        GHS_SCHEDULE_DESCRIPTOR_UUID,
                                        scheduleByteArray);
        parser_free(parser);

        GByteArray *manufacturer = g_byte_array_new_take((guint8*) g_strdup("Philips"), 7);
        binc_application_set_char_value(app, DIS_SERVICE_UUID, DIS_MANUFACTURER_UUID, manufacturer);

        GByteArray *model = g_byte_array_new_take((guint8*) g_strdup("POX22"), 5);
        binc_application_set_char_value(app, DIS_SERVICE_UUID, DIS_MODEL_NUMBER_UUID, model);

        // Setup callbacks
        binc_application_set_desc_write_cb(app, &on_local_desc_write);
        binc_application_set_desc_write_success_cb(app, &on_local_desc_write_success);
        binc_application_set_char_start_notify_cb(app, &on_local_char_start_notify);
        binc_application_set_char_stop_notify_cb(app, &on_local_char_stop_notify);
        binc_adapter_register_application(default_adapter, app);
    } else {
        log_debug("MAIN", "No default_adapter found");
    }

    // Bail out after some time
    g_timeout_add_seconds(60, callback, loop);

    // Start the mainloop
    g_main_loop_run(loop);

    // Clean up mainloop
    g_main_loop_unref(loop);

    // Disconnect from DBus
    g_dbus_connection_close_sync(dbusConnection, NULL, NULL);
    g_object_unref(dbusConnection);
    return 0;
}