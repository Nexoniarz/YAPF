// YAPF thumbnails for KDE Dolphin and other KIO file dialogs.
// Copyright 2026 Nexoniarz — Apache License 2.0.

#include <KIO/ThumbnailCreator>
#include <KPluginFactory>
#include <QImage>

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
        const QByteArray path = request.url().toLocalFile().toLocal8Bit();
        yapf_image_t *img = yapf_load(path.constData());
        if (!img) {
            return KIO::ThumbnailResult::fail();
        }

        // Start from the smallest stored mip level that still covers the
        // requested size; Qt scales the rest.
        const QSize want = request.targetSize();
        int m = 0;
        while (m + 1 < img->mip_levels &&
               int(img->width >> (m + 1)) >= want.width() &&
               int(img->height >> (m + 1)) >= want.height()) {
            ++m;
        }
        const int w = qMax(1, int(img->width >> m));
        const int h = qMax(1, int(img->height >> m));
        const uchar *px = img->mips[m];

        QImage image;
        switch (img->channels) {
        case 1:
            image = QImage(px, w, h, w, QImage::Format_Grayscale8).copy();
            break;
        case 2: {                                   // gray + alpha → RGBA
            image = QImage(w, h, QImage::Format_RGBA8888);
            for (int y = 0; y < h; ++y) {
                const uchar *s = px + size_t(y) * w * 2;
                uchar *d = image.scanLine(y);
                for (int x = 0; x < w; ++x, s += 2, d += 4) {
                    d[0] = d[1] = d[2] = s[0];
                    d[3] = s[1];
                }
            }
            break;
        }
        case 3:
            image = QImage(px, w, h, w * 3, QImage::Format_RGB888).copy();
            break;
        default:
            image = QImage(px, w, h, w * 4,
                           (img->flags & YAPF_FLAG_PREMULT_ALPHA) ? QImage::Format_RGBA8888_Premultiplied
                                                                  : QImage::Format_RGBA8888).copy();
            break;
        }
        yapf_free(img);

        if (image.width() > want.width() || image.height() > want.height()) {
            image = image.scaled(want, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        }
        return KIO::ThumbnailResult::pass(image);
    }
};

K_PLUGIN_CLASS_WITH_JSON(YapfThumbnailCreator, "yapfthumbnail.json")

#include "yapfthumbnail.moc"
