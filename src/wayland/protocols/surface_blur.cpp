// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "wayland/protocols/surface_blur.h"

#include <algorithm>
#include <cmath>
#include <cutefish-surface-blur-v1-server-protocol.h>
#include <limits>

#include "compositor/surface.h"
#include "wayland/server_util.h"

namespace cutefish::wayland {

namespace {

void destroy(wl_client *, wl_resource *resource)
{
    wl_resource_destroy(resource);
}

void set(wl_client *, wl_resource *resource, wl_resource *surfaceResource, int32_t x, int32_t y,
         int32_t width, int32_t height, int32_t radius)
{
    if (width <= 0 || height <= 0 || radius < 0
        || int64_t(x) + width > std::numeric_limits<int32_t>::max()
        || int64_t(y) + height > std::numeric_limits<int32_t>::max()) {
        wl_resource_post_error(resource, CUTEFISH_SURFACE_BLUR_V1_ERROR_INVALID_GEOMETRY,
                               "invalid blur rectangle or radius");
        return;
    }
    auto *surface = getUserDataAs<compositor::Surface>(surfaceResource);
    surface->setBackgroundBlur(QRect(x, y, width, height),
                               std::min(radius, std::min(width, height) / 2));
}

void unset(wl_client *, wl_resource *, wl_resource *surfaceResource)
{
    getUserDataAs<compositor::Surface>(surfaceResource)->setBackgroundBlur({}, 0);
}

void setGlass(wl_client *, wl_resource *, wl_resource *surfaceResource)
{
    auto *surface = getUserDataAs<compositor::Surface>(surfaceResource);
    render::GlassParams glass = surface->pendingGlass();
    glass.enabled = true;
    surface->setGlass(glass);
}

void unsetGlass(wl_client *, wl_resource *, wl_resource *surfaceResource)
{
    auto *surface = getUserDataAs<compositor::Surface>(surfaceResource);
    render::GlassParams glass = surface->pendingGlass();
    glass.enabled = false;
    surface->setGlass(glass);
}

// Rejects the values the shader cannot interpret; everything else is clamped
// where it is used, so a client cannot trip an error by asking for too much.
bool valid(wl_fixed_t value, double minimum)
{
    const double number = wl_fixed_to_double(value);
    return std::isfinite(number) && number >= minimum;
}

void setGlassMaterial(wl_client *, wl_resource *resource, wl_resource *surfaceResource,
                      wl_fixed_t blurSigma, wl_fixed_t bevel, wl_fixed_t depth, wl_fixed_t ior,
                      wl_fixed_t profile, wl_fixed_t displacement, wl_fixed_t chroma)
{
    if (!valid(blurSigma, 0) || !valid(bevel, 0) || !valid(depth, 0) || !valid(ior, 1)
        || !valid(profile, 0) || !valid(chroma, 0)
        || !std::isfinite(wl_fixed_to_double(displacement))) {
        wl_resource_post_error(resource, CUTEFISH_SURFACE_BLUR_V1_ERROR_INVALID_GEOMETRY,
                               "invalid glass material");
        return;
    }
    auto *surface = getUserDataAs<compositor::Surface>(surfaceResource);
    render::GlassParams glass = surface->pendingGlass();
    glass.blurSigma = wl_fixed_to_double(blurSigma);
    glass.bevel = wl_fixed_to_double(bevel);
    glass.depth = wl_fixed_to_double(depth);
    glass.ior = wl_fixed_to_double(ior);
    glass.profile = wl_fixed_to_double(profile);
    glass.displacement = wl_fixed_to_double(displacement);
    glass.chroma = wl_fixed_to_double(chroma);
    surface->setGlass(glass);
}

void setGlassLighting(wl_client *, wl_resource *resource, wl_resource *surfaceResource,
                      wl_fixed_t lightAngle, wl_fixed_t specularIntensity, wl_fixed_t shininess,
                      wl_fixed_t sheen, wl_fixed_t ambientOcclusion,
                      wl_fixed_t ambientOcclusionRadius)
{
    if (!std::isfinite(wl_fixed_to_double(lightAngle)) || !valid(specularIntensity, 0)
        || !valid(shininess, 0) || !valid(sheen, 0) || !valid(ambientOcclusion, 0)
        || !valid(ambientOcclusionRadius, 0)) {
        wl_resource_post_error(resource, CUTEFISH_SURFACE_BLUR_V1_ERROR_INVALID_GEOMETRY,
                               "invalid glass lighting");
        return;
    }
    auto *surface = getUserDataAs<compositor::Surface>(surfaceResource);
    render::GlassParams glass = surface->pendingGlass();
    glass.lightAngle = wl_fixed_to_double(lightAngle);
    glass.specularIntensity = wl_fixed_to_double(specularIntensity);
    glass.shininess = wl_fixed_to_double(shininess);
    glass.sheen = wl_fixed_to_double(sheen);
    glass.ambientOcclusion = wl_fixed_to_double(ambientOcclusion);
    glass.ambientOcclusionRadius = wl_fixed_to_double(ambientOcclusionRadius);
    surface->setGlass(glass);
}

void setGlassTint(wl_client *, wl_resource *, wl_resource *surfaceResource, wl_fixed_t red,
                  wl_fixed_t green, wl_fixed_t blue, wl_fixed_t alpha)
{
    const auto component = [](wl_fixed_t value) {
        const double number = wl_fixed_to_double(value);
        return float(std::isfinite(number) ? std::clamp(number, 0.0, 1.0) : 0.0);
    };
    auto *surface = getUserDataAs<compositor::Surface>(surfaceResource);
    render::GlassParams glass = surface->pendingGlass();
    glass.tintRed = component(red);
    glass.tintGreen = component(green);
    glass.tintBlue = component(blue);
    glass.tintAlpha = component(alpha);
    surface->setGlass(glass);
}

void setShape(wl_client *, wl_resource *resource, wl_resource *surfaceResource, wl_array *array)
{
    constexpr size_t kMaxValues = 4096;
    const size_t count = array->size / sizeof(int32_t);
    const auto *values = static_cast<const int32_t *>(array->data);
    const auto invalid = [resource] {
        wl_resource_post_error(resource, CUTEFISH_SURFACE_BLUR_V1_ERROR_INVALID_GEOMETRY,
                               "invalid blur shape");
    };
    if (array->size % sizeof(int32_t) || count > kMaxValues)
        return invalid();

    QPainterPath path;
    path.setFillRule(Qt::WindingFill);
    size_t i = 0;
    const auto point = [&] {
        const QPointF result(wl_fixed_to_double(values[i]), wl_fixed_to_double(values[i + 1]));
        i += 2;
        return result;
    };
    while (i < count) {
        const uint32_t command = values[i++];
        const size_t points = command == CUTEFISH_SURFACE_BLUR_V1_PATH_COMMAND_CUBIC_TO ? 3
                : command == CUTEFISH_SURFACE_BLUR_V1_PATH_COMMAND_CLOSE                ? 0
                                                                                        : 1;
        if (command > CUTEFISH_SURFACE_BLUR_V1_PATH_COMMAND_CLOSE || count - i < points * 2
            || (path.elementCount() == 0
                && command != CUTEFISH_SURFACE_BLUR_V1_PATH_COMMAND_MOVE_TO))
            return invalid();
        switch (command) {
        case CUTEFISH_SURFACE_BLUR_V1_PATH_COMMAND_MOVE_TO:
            path.moveTo(point());
            break;
        case CUTEFISH_SURFACE_BLUR_V1_PATH_COMMAND_LINE_TO:
            path.lineTo(point());
            break;
        case CUTEFISH_SURFACE_BLUR_V1_PATH_COMMAND_CUBIC_TO: {
            const QPointF first = point();
            const QPointF second = point();
            path.cubicTo(first, second, point());
            break;
        }
        case CUTEFISH_SURFACE_BLUR_V1_PATH_COMMAND_CLOSE:
            path.closeSubpath();
            break;
        }
    }
    getUserDataAs<compositor::Surface>(surfaceResource)->setBackgroundBlurShape(path);
}

void unsetShape(wl_client *, wl_resource *, wl_resource *surfaceResource)
{
    getUserDataAs<compositor::Surface>(surfaceResource)->setBackgroundBlurShape({});
}

void setGlassSaturation(wl_client *, wl_resource *resource, wl_resource *surfaceResource,
                        wl_fixed_t saturation)
{
    if (!valid(saturation, 0)) {
        wl_resource_post_error(resource, CUTEFISH_SURFACE_BLUR_V1_ERROR_INVALID_GEOMETRY,
                               "invalid glass saturation");
        return;
    }
    auto *surface = getUserDataAs<compositor::Surface>(surfaceResource);
    render::GlassParams glass = surface->pendingGlass();
    glass.saturation = wl_fixed_to_double(saturation);
    surface->setGlass(glass);
}

void setGlassRim(wl_client *, wl_resource *resource, wl_resource *surfaceResource,
                 wl_fixed_t intensity)
{
    if (!valid(intensity, 0)) {
        wl_resource_post_error(resource, CUTEFISH_SURFACE_BLUR_V1_ERROR_INVALID_GEOMETRY,
                               "invalid glass rim");
        return;
    }
    auto *surface = getUserDataAs<compositor::Surface>(surfaceResource);
    render::GlassParams glass = surface->pendingGlass();
    glass.rim = std::min(wl_fixed_to_double(intensity), 1.0);
    surface->setGlass(glass);
}

const struct cutefish_surface_blur_v1_interface implementation = {
        destroy,          set,          unset,    setGlass,   unsetGlass,         setGlassMaterial,
        setGlassLighting, setGlassTint, setShape, unsetShape, setGlassSaturation, setGlassRim};

void bind(wl_client *client, void *, uint32_t, uint32_t id)
{
    auto *resource = wl_resource_create(client, &cutefish_surface_blur_v1_interface, 1, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &implementation, nullptr, nullptr);
}

} // namespace

wl_global *createSurfaceBlurGlobal(wl_display *display)
{
    return wl_global_create(display, &cutefish_surface_blur_v1_interface, 1, nullptr, bind);
}

} // namespace cutefish::wayland
