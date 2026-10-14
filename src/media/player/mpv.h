#pragma once
#include <mpv/client.h>
#include <QByteArray>
#include <QMap>
#include <QString>
#include <QUrl>
#include <cstring>
#include <string>
#include <stdexcept>
#include <thread>
#include <assert.h>

namespace Mpv
{
class Node : mpv_node
{
public:
    typedef Node *iterator;

    inline Node() noexcept
    {
        format = MPV_FORMAT_NONE;
    }

    inline Node(Node &&other) noexcept
    {
        format = other.format;
        u = other.u;
        other.format = MPV_FORMAT_NONE;
    }

    inline Node(bool v) noexcept
    {
        format = MPV_FORMAT_FLAG;
        u.flag = v;
    }

    inline Node(int64_t v) noexcept
    {
        format = MPV_FORMAT_INT64;
        u.int64 = v;
    }

    inline Node(double v) noexcept
    {
        format = MPV_FORMAT_DOUBLE;
        u.double_ = v;
    }

    inline Node(const char *v) noexcept
    {
        format = MPV_FORMAT_STRING;
        u.string = const_cast<char*>(v);
    }

    inline mpv_format type() const noexcept
    {
        return format;
    }

    inline operator bool() const noexcept
    {
        assert(format == MPV_FORMAT_FLAG);
        return u.flag;
    }

    inline operator int64_t() const noexcept
    {
        assert(format == MPV_FORMAT_INT64);
        return u.int64;
    }

    inline operator double() const noexcept
    {
        assert(format == MPV_FORMAT_DOUBLE);
        return u.double_;
    }

    inline operator const char*() const noexcept
    {
        assert(format == MPV_FORMAT_STRING);
        return u.string;
    }

    inline int size() const noexcept
    {
        assert(format == MPV_FORMAT_NODE_ARRAY);
        return u.list->num;
    }

    inline const Node &operator[](int i) const noexcept
    {
        assert(format == MPV_FORMAT_NODE_ARRAY);
        return static_cast<Node *>(u.list->values)[i];
    }

    inline iterator begin() const noexcept
    {
        assert(format == MPV_FORMAT_NODE_ARRAY);
        return static_cast<Node *>(&u.list->values[0]);
    }

    inline iterator end() const noexcept
    {
        assert(format == MPV_FORMAT_NODE_ARRAY);
        return static_cast<Node *>(&u.list->values[u.list->num]);
    }

    inline const Node &operator[](const char *key) const
    {
        assert(format == MPV_FORMAT_NODE_MAP);
        for (int i = 0; i < u.list->num; i++)
        {
            if (std::strcmp(key, u.list->keys[i]) == 0)
            {
                return static_cast<Node *>(u.list->values)[i];
            }
        }
        throw std::runtime_error("Mpv::NodeMap::operator[]: key does not exist!");
    }

    inline const mpv_node_list *list() const noexcept
    {
        assert(format == MPV_FORMAT_NODE_ARRAY || format == MPV_FORMAT_NODE_MAP);
        return u.list;
    }
};

// wasapi calls CoUninitialize() on the destroying thread without COM there and crashes Qt at exit; a joined thread takes it.
inline void terminate(mpv_handle *handle) noexcept
{
    if (handle) std::thread([handle] { mpv_terminate_destroy(handle); }).join();
}

inline QByteArray target(const QUrl &url)
{
    return (url.isLocalFile() ? url.toLocalFile() : url.toString()).toUtf8();
}

// The user-agent and referrer options miss HLS segments, so each header is set alone: a joined list splits at "KHTML, like Gecko".
inline void applyHttpHeaders(mpv_handle *mpv, const QMap<QString, QString> &headers)
{
    for (auto it = headers.constBegin(); it != headers.constEnd(); ++it) {
        const QString key = it.key().toLower();
        const QByteArray value = it.value().toUtf8();
        if (key == QLatin1String("user-agent"))
            mpv_set_property_string(mpv, "user-agent", value.constData());
        else if (key == QLatin1String("referrer") || key == QLatin1String("referer"))
            mpv_set_property_string(mpv, "referrer", value.constData());
        const QByteArray line = it.key().toUtf8() + ": " + value;
        const char *append[] = {"change-list", "http-header-fields", "append", line.constData(), nullptr};
        mpv_command(mpv, append);
    }
}

class Handle
{
private:
    mpv_handle *m_handle = nullptr;

public:
    inline mpv_handle *raw() const noexcept
    {
        return m_handle;
    }

    inline Handle() noexcept
    {
        m_handle = mpv_create();
    }

    inline ~Handle() noexcept
    {
        mpv_set_wakeup_callback(m_handle, nullptr, nullptr);
        terminate(m_handle);
    }

    inline int initialize() const noexcept
    {
        return mpv_initialize(m_handle);
    }

    inline int set_option(const char *name, const Node& data) const noexcept
    {
        return mpv_set_option(m_handle, name, MPV_FORMAT_NODE, const_cast<Node*>(&data));
    }

    inline int command_async(const char **args) const noexcept
    {
        return mpv_command_async(m_handle, 0, args);
    }

    inline int command(const char **args) const noexcept
    {
        return mpv_command(m_handle, args);
    }

    inline int set_property_async(const char *name, const Node& data) const noexcept
    {
        return mpv_set_property_async(m_handle, 0, name, MPV_FORMAT_NODE, const_cast<Node*>(&data));
    }

    inline int set_property(const char *name, const Node& data) const noexcept
    {
        return mpv_set_property(m_handle, name, MPV_FORMAT_NODE, const_cast<Node*>(&data));
    }

    inline bool get_int64(const char *name, int64_t &out) const noexcept
    {
        return mpv_get_property(m_handle, name, MPV_FORMAT_INT64, &out) >= 0;
    }

    inline bool get_double(const char *name, double &out) const noexcept
    {
        return mpv_get_property(m_handle, name, MPV_FORMAT_DOUBLE, &out) >= 0;
    }

    inline std::string get_string(const char *name) const
    {
        char *value = mpv_get_property_string(m_handle, name);
        std::string out = value ? value : "";
        mpv_free(value);
        return out;
    }

    inline int command_ret(const char **args, mpv_node *result) const noexcept
    {
        return mpv_command_ret(m_handle, args, result);
    }

    inline int observe_property(uint64_t id, const char *name) const noexcept
    {
        return mpv_observe_property(m_handle, id, name, MPV_FORMAT_NODE);
    }

    inline const mpv_event *wait_event(double timeout = 0) const noexcept
    {
        return mpv_wait_event(m_handle, timeout);
    }

    inline void set_wakeup_callback(void (*callback)(void *userdata), void *userdata) const noexcept
    {
        mpv_set_wakeup_callback(m_handle, callback, userdata);
    }

    inline int request_log_messages(const char *min_level) const noexcept
    {
        return mpv_request_log_messages(m_handle, min_level);
    }
};
}
