#include "audio/DeviceReserve.h"

#include <dbus/dbus.h>

#include <cstring>

#include "audio/Format.h"
#include "common/I18n.h"

namespace gs {
namespace {

const char* kIface = "org.freedesktop.ReserveDevice1";
const dbus_int32_t kOurPriority = 100;  // PulseAudio uses 0; a higher value makes it yield to us

void appendVariant(DBusMessage* reply, int type, const void* value) {
    DBusMessageIter it, sub;
    dbus_message_iter_init_append(reply, &it);
    const char sig[2] = {(char)type, 0};
    dbus_message_iter_open_container(&it, DBUS_TYPE_VARIANT, sig, &sub);
    dbus_message_iter_append_basic(&sub, type, value);
    dbus_message_iter_close_container(&it, &sub);
}

struct HandlerCtx {
    std::string deviceName;
};

DBusHandlerResult objectHandler(DBusConnection* c, DBusMessage* m, void* userdata) {
    auto* ctx = static_cast<HandlerCtx*>(userdata);
    if (dbus_message_is_method_call(m, kIface, "RequestRelease")) {
        dbus_int32_t prio = 0;
        dbus_message_get_args(m, nullptr, DBUS_TYPE_INT32, &prio, DBUS_TYPE_INVALID);
        dbus_bool_t ret = FALSE;  // we're playing: don't give it up
        DBusMessage* reply = dbus_message_new_method_return(m);
        if (reply) {
            dbus_message_append_args(reply, DBUS_TYPE_BOOLEAN, &ret, DBUS_TYPE_INVALID);
            dbus_connection_send(c, reply, nullptr);
            dbus_message_unref(reply);
        }
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (dbus_message_is_method_call(m, "org.freedesktop.DBus.Properties", "Get")) {
        const char* iface = nullptr;
        const char* prop = nullptr;
        if (!dbus_message_get_args(m, nullptr, DBUS_TYPE_STRING, &iface, DBUS_TYPE_STRING, &prop, DBUS_TYPE_INVALID))
            return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
        DBusMessage* reply = nullptr;
        if (std::strcmp(iface, kIface) == 0) {
            if (std::strcmp(prop, "ApplicationName") == 0) {
                reply = dbus_message_new_method_return(m);
                const char* v = "Guitar Studio";
                appendVariant(reply, DBUS_TYPE_STRING, &v);
            } else if (std::strcmp(prop, "ApplicationDeviceName") == 0) {
                reply = dbus_message_new_method_return(m);
                const char* v = ctx->deviceName.c_str();
                appendVariant(reply, DBUS_TYPE_STRING, &v);
            } else if (std::strcmp(prop, "Priority") == 0) {
                reply = dbus_message_new_method_return(m);
                dbus_int32_t v = kOurPriority;
                appendVariant(reply, DBUS_TYPE_INT32, &v);
            }
        }
        if (!reply) reply = dbus_message_new_error(m, DBUS_ERROR_UNKNOWN_METHOD, "Unknown property");
        dbus_connection_send(c, reply, nullptr);
        dbus_message_unref(reply);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (dbus_message_is_method_call(m, "org.freedesktop.DBus.Introspectable", "Introspect")) {
        static const char* xml =
            "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\" "
            "\"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">"
            "<node><interface name=\"org.freedesktop.ReserveDevice1\">"
            "<method name=\"RequestRelease\"><arg name=\"priority\" type=\"i\" direction=\"in\"/>"
            "<arg name=\"result\" type=\"b\" direction=\"out\"/></method>"
            "<property name=\"Priority\" type=\"i\" access=\"read\"/>"
            "<property name=\"ApplicationName\" type=\"s\" access=\"read\"/>"
            "<property name=\"ApplicationDeviceName\" type=\"s\" access=\"read\"/>"
            "</interface></node>";
        DBusMessage* reply = dbus_message_new_method_return(m);
        dbus_message_append_args(reply, DBUS_TYPE_STRING, &xml, DBUS_TYPE_INVALID);
        dbus_connection_send(c, reply, nullptr);
        dbus_message_unref(reply);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

void unregisterCtx(DBusConnection*, void* userdata) { delete static_cast<HandlerCtx*>(userdata); }

}  // namespace

bool DeviceReservation::acquire(int cardIndex, const std::string& deviceName, std::string& msg) {
    release();
    deviceName_ = deviceName;
    service_ = "org.freedesktop.ReserveDevice1.Audio" + std::to_string(cardIndex);
    path_ = "/org/freedesktop/ReserveDevice1/Audio" + std::to_string(cardIndex);

    DBusError err;
    dbus_error_init(&err);
    DBusConnection* c = dbus_bus_get_private(DBUS_BUS_SESSION, &err);
    if (!c) {
        msg = strf(tr("Cannot connect to the session D-Bus (%s); opening the card directly"), err.message ? err.message : "");
        dbus_error_free(&err);
        return false;
    }
    dbus_connection_set_exit_on_disconnect(c, FALSE);

    static const DBusObjectPathVTable vtable = {unregisterCtx, objectHandler, nullptr, nullptr, nullptr, nullptr};
    auto* ctx = new HandlerCtx{deviceName};
    if (!dbus_connection_register_object_path(c, path_.c_str(), &vtable, ctx)) {
        delete ctx;
        dbus_connection_close(c);
        dbus_connection_unref(c);
        msg = tr("Failed to register the D-Bus object");
        return false;
    }

    int k = dbus_bus_request_name(c, service_.c_str(), DBUS_NAME_FLAG_DO_NOT_QUEUE, &err);
    if (k == DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER || k == DBUS_REQUEST_NAME_REPLY_ALREADY_OWNER) {
        conn_ = c;
        msg = tr("Card reserved (no other program was using it)");
        return true;
    }
    if (dbus_error_is_set(&err)) dbus_error_free(&err);

    // Already reserved by someone else (usually PulseAudio): politely ask it to release
    bool granted = false;
    DBusMessage* m = dbus_message_new_method_call(service_.c_str(), path_.c_str(), kIface, "RequestRelease");
    if (m) {
        dbus_int32_t prio = kOurPriority;
        dbus_message_append_args(m, DBUS_TYPE_INT32, &prio, DBUS_TYPE_INVALID);
        DBusMessage* r = dbus_connection_send_with_reply_and_block(c, m, 5000, &err);
        dbus_message_unref(m);
        if (r) {
            dbus_bool_t ok = FALSE;
            if (dbus_message_get_args(r, nullptr, DBUS_TYPE_BOOLEAN, &ok, DBUS_TYPE_INVALID)) granted = ok;
            dbus_message_unref(r);
        } else if (dbus_error_is_set(&err)) {
            dbus_error_free(&err);
        }
    }
    if (!granted) {
        dbus_connection_close(c);
        dbus_connection_unref(c);
        msg = tr("Another program refused to release the card");
        return false;
    }
    k = dbus_bus_request_name(c, service_.c_str(), DBUS_NAME_FLAG_DO_NOT_QUEUE | DBUS_NAME_FLAG_REPLACE_EXISTING, &err);
    if (k != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
        if (dbus_error_is_set(&err)) dbus_error_free(&err);
        dbus_connection_close(c);
        dbus_connection_unref(c);
        msg = tr("Failed to take over the card reservation");
        return false;
    }
    conn_ = c;
    msg = tr("PulseAudio released the card");
    return true;
}

void DeviceReservation::poll() {
    if (!conn_) return;
    dbus_connection_read_write(conn_, 0);
    while (dbus_connection_dispatch(conn_) == DBUS_DISPATCH_DATA_REMAINS) {
    }
}

void DeviceReservation::release() {
    if (!conn_) return;
    DBusError err;
    dbus_error_init(&err);
    dbus_bus_release_name(conn_, service_.c_str(), &err);
    if (dbus_error_is_set(&err)) dbus_error_free(&err);
    dbus_connection_unregister_object_path(conn_, path_.c_str());
    dbus_connection_flush(conn_);
    dbus_connection_close(conn_);
    dbus_connection_unref(conn_);
    conn_ = nullptr;
}

}  // namespace gs
