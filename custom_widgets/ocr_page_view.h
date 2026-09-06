#ifndef OCR_PAGE_VIEW_H
#define OCR_PAGE_VIEW_H

#include <QLabel>
#include <QMouseEvent>
#include <QRubberBand>

// Selection coordinates always refer to the decoded page, not the scaled
// preview. This widget never changes or saves the source image.
class OcrPageView : public QLabel
{
public:
    explicit OcrPageView(QWidget *parent = nullptr) : QLabel(parent), band(QRubberBand::Rectangle, this)
    {
        setAlignment(Qt::AlignTop | Qt::AlignLeft);
        setCursor(Qt::CrossCursor);
    }
    void setPage(const QImage &image)
    {
        band.hide();
        selection = { };
        sourceSize = image.size();
        const auto preview = QPixmap::fromImage(image).scaled(650, 950, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        setPixmap(preview);
        setFixedSize(preview.isNull() ? QSize(1, 1) : preview.size());
    }
    QRect selectedRegion() const
    {
        if (selection.width() < 4 || selection.height() < 4 || sourceSize.isEmpty())
            return { };
        const QRectF mapped(selection.x() * double(sourceSize.width()) / width(),
                            selection.y() * double(sourceSize.height()) / height(),
                            selection.width() * double(sourceSize.width()) / width(),
                            selection.height() * double(sourceSize.height()) / height());
        return mapped.toAlignedRect().intersected(QRect(QPoint(), sourceSize));
    }

protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton)
            return;
        origin = event->position().toPoint();
        selection = { };
        band.setGeometry(QRect(origin, QSize()));
        band.show();
    }
    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (event->buttons() & Qt::LeftButton) {
            selection = QRect(origin, event->position().toPoint()).normalized().intersected(rect());
            band.setGeometry(selection);
        }
    }
    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton) {
            selection = QRect(origin, event->position().toPoint()).normalized().intersected(rect());
            band.setGeometry(selection);
            if (selectedRegion().isEmpty())
                band.hide();
        }
    }

private:
    QRubberBand band;
    QPoint origin;
    QRect selection;
    QSize sourceSize;
};
#endif
