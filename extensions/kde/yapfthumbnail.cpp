// YAPF thumbnails for KDE Dolphin and other KIO file dialogs.
// Copyright 2026 Nexoniarz — Apache License 2.0.

#include <KIO/ThumbnailCreator>
#include <KPluginFactory>
#include <QFile>
#include <QImage>

#include <cstring>

extern "C" {
#include "yapf.h"
}

class YapfThumbnailCreator : public KIO::ThumbnailCreator
{
    Q_OBJECT
public:
    YapfThumbnailCreator(QObject *parent, const QVariantList &args)
        : KIO::ThumbnailCreator(parent, args)
    {
    }

    KIO::ThumbnailResult create(const KIO::ThumbnailRequest &request) override
    {
        QFile file(request.url().toLocalFile());
        if (!file.open(QIODevice::ReadOnly)) {
            return KIO::ThumbnailResult::fail();
        }
        const QByteArray data = file.readAll();

        // Version 1 and 2: read the tables, then decode only the mip we
        // need (frame 0, layer 0, face 0, slice 0).
        int err = 0;
        yapf_texture_t *tex = yapf_tex_open(data.constData(), size_t(data.size()), 0, &err);
        if (!tex) {
            return KIO::ThumbnailResult::fail();
        }

        // The smallest stored mip that still covers the requested size;
        // Qt scales the rest.
        const QSize want = request.targetSize();
        uint32_t m = 0, w = 0, h = 0;
        while (m + 1 < tex->mip_levels) {
            yapf_tex_mip_size(tex, m + 1, &w, &h, nullptr);
            if (int(w) < want.width() || int(h) < want.height()) {
                break;
            }
            ++m;
        }
        yapf_tex_mip_size(tex, m, &w, &h, nullptr);
        const uint32_t s = yapf_tex_surface(tex, 0, 0, 0, m);

        QImage image(int(w), int(h), (tex->flags & YAPF_FLAG_PREMULT_ALPHA) ? QImage::Format_RGBA8888_Premultiplied
                                                                            : QImage::Format_RGBA8888);
        bool ok = !image.isNull() &&
                  yapf_tex_decode_surface(tex, data.constData(), size_t(data.size()), s, 0) == YAPF_OK;
        if (ok) {
            // RGBA 8-bit rows (blocks decoded, 16-bit / float converted)
            QByteArray rgba(int(size_t(w) * h * 4), Qt::Uninitialized);
            ok = yapf_tex_to_rgba8(tex, s, 0, reinterpret_cast<uint8_t *>(rgba.data())) == YAPF_OK;
            for (int y = 0; ok && y < int(h); ++y) {
                memcpy(image.scanLine(y), rgba.constData() + size_t(y) * w * 4, size_t(w) * 4);
            }
        }
        yapf_tex_free(tex);
        if (!ok) {
            return KIO::ThumbnailResult::fail();
        }

        if (image.width() > want.width() || image.height() > want.height()) {
            image = image.scaled(want, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        }
        return KIO::ThumbnailResult::pass(image);
    }
};

K_PLUGIN_CLASS_WITH_JSON(YapfThumbnailCreator, "yapfthumbnail.json")

#include "yapfthumbnail.moc"
