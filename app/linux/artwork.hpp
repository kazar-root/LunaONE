// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "core/http_client.hpp"
#include "ui/image_url_policy.hpp"

#include <gtk/gtk.h>
#include <memory>

namespace lunaone::linux_desktop
{
using Pic = std::shared_ptr<GdkPixbuf>;

inline Pic Artwork(const std::string& url, int width, int height)
{
    if (!url.starts_with("https://"))
        return {};
    const auto host = url.substr(8, url.find('/', 8) - 8);
    if (!host.ends_with(".media-amazon.com") && !host.ends_with(".images-amazon.com"))
        return {};
    const auto response = lunanx::HttpClient{}.Get(lunanx::ResizeAmazonImageUrl(url, height ? 320 : width), "LunaONE", {}, 3, 5);
    if (response.status_code != 200 || response.body.size() > 6 * 1024 * 1024)
        return {};
    std::unique_ptr<GdkPixbufLoader, decltype(&g_object_unref)> loader(gdk_pixbuf_loader_new(), &g_object_unref);
    if (height) gdk_pixbuf_loader_set_size(loader.get(), width, height);
    if (!gdk_pixbuf_loader_write(loader.get(), reinterpret_cast<const guchar*>(response.body.data()), response.body.size(), nullptr) ||
        !gdk_pixbuf_loader_close(loader.get(), nullptr))
        return {};
    auto bitmap = gdk_pixbuf_loader_get_pixbuf(loader.get());
    if (!bitmap)
        return {};
    return Pic(GDK_PIXBUF(g_object_ref(bitmap)), [](auto value) { g_object_unref(value); });
}
}
