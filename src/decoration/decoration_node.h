// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Paints only the opaque SSD frame regions, caption and controls.
// WindowVisualNode owns the shared body clip and final exterior outline.

#ifndef CUTEFISH_DECORATION_DECORATION_NODE_H_
#define CUTEFISH_DECORATION_DECORATION_NODE_H_

#include <memory>
#include <vector>

#include <QString>

#include "scene/node.h"

namespace cutefish::decoration {

class Decoration;
class Theme;

class DecorationNode : public scene::Node
{
public:
    DecorationNode(const Decoration* decoration, Theme* theme);
    ~DecorationNode() override;

    // The client area inside this geometry is deliberately left unpainted.
    Content content() const override { return Content::Translucent; }

protected:
    void paint(render::Renderer& renderer, const QRect& absoluteGeometry) const override;

private:
    void paintFrame(render::Renderer& renderer, const QRect& bounds) const;
    void paintCaption(render::Renderer& renderer, const QRect& bounds) const;
    void paintButtons(render::Renderer& renderer, const QRect& bounds) const;

    const Decoration* m_decoration = nullptr;
    Theme* m_theme = nullptr;

    // The caption is rasterized from the window's title; it is not a second
    // copy of it. Rebuilt whenever the title or its colour changes, and kept
    // once per output scale, so outputs at different scales do not rebuild it
    // for each other every frame.
    struct Caption {
        double scale = 0;
        QString key;
        std::unique_ptr<render::Texture> texture;
    };
    mutable std::vector<Caption> m_captions;
};

} // namespace cutefish::decoration

#endif // CUTEFISH_DECORATION_DECORATION_NODE_H_
