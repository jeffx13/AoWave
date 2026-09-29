#pragma once
#include <QByteArray>
#include <QImage>

// Byte mode, error correction M or better. Null for empty data or data too long to encode.
namespace QrCode {

// One pixel per module. Scanners need the quiet zone.
QImage render(const QByteArray &data, int quietZone = 4);

}
