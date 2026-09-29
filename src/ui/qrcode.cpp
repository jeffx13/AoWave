#include "ui/qrcode.h"

#include <cstdint>
#include <optional>
#include <qrcodegen.hpp>

QImage QrCode::render(const QByteArray &data, int quietZone) {
    if (data.isEmpty()) return {};

    const auto *bytes = reinterpret_cast<const std::uint8_t *>(data.constData());
    std::optional<qrcodegen::QrCode> code;
    try {
        code = qrcodegen::QrCode::encodeBinary({bytes, bytes + data.size()},
                                               qrcodegen::QrCode::Ecc::MEDIUM);
    } catch (const qrcodegen::data_too_long &) {
        return {};
    }

    const int size = code->getSize();
    const int margin = qMax(0, quietZone);
    QImage image(size + margin * 2, size + margin * 2, QImage::Format_RGB32);
    image.fill(Qt::white);
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x)
            if (code->getModule(x, y)) image.setPixel(x + margin, y + margin, qRgb(0, 0, 0));
    return image;
}
