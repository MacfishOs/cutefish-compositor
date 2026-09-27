// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <cmath>
#include <cstdint>
#include <optional>

#include <QPointF>
#include <QRectF>
#include <QRegularExpression>
#include <QSize>
#include <QString>

namespace cutefish::compositor {

class OutputScale
{
public:
    static constexpr uint32_t denominator = 120;

    static std::optional<OutputScale> fromNumerator(uint32_t value)
    {
        if (value < 120 || value > 480)
            return std::nullopt;
        return OutputScale(value);
    }

    static std::optional<OutputScale> parse(const QString& text)
    {
        static const QRegularExpression decimal(QStringLiteral("^[0-9]+(?:\\.[0-9]+)?$"));
        if (!decimal.match(text).hasMatch())
            return std::nullopt;
        bool valid = false;
        const double value = text.toDouble(&valid);
        if (!valid || !std::isfinite(value) || value < 1 || value > 4)
            return std::nullopt;
        return fromNumerator(static_cast<uint32_t>(std::lround(value * denominator)));
    }

    OutputScale() = default;

    uint32_t numerator() const { return m_numerator; }

    double value() const { return double(m_numerator) / denominator; }

    int integerScale() const { return (m_numerator + denominator - 1) / denominator; }

    QSize logicalSize(const QSize& native) const
    {
        // Cover the native target; the fractional last logical pixel is clipped.
        const auto extent = [this](int pixels) {
            return int((int64_t(pixels) * denominator + m_numerator - 1) / m_numerator);
        };
        return QSize(extent(native.width()), extent(native.height()));
    }

    QPointF toDevice(const QPointF& logical) const { return logical * value(); }

    QPointF toLogical(const QPointF& device) const { return device / value(); }

    QRect deviceCoverage(const QRectF& logical) const
    {
        return QRectF(toDevice(logical.topLeft()), logical.size() * value()).toAlignedRect();
    }

    bool operator==(const OutputScale& other) const { return m_numerator == other.m_numerator; }

private:
    explicit OutputScale(uint32_t value)
        : m_numerator(value)
    {
    }

    uint32_t m_numerator = denominator;
};

} // namespace cutefish::compositor
