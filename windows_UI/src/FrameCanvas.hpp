#pragma once
#include <QImage>
#include <QQuickItem>

class FrameCanvas : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(QImage image READ image WRITE setImage NOTIFY imageChanged)
    // Split compare: the left of the split line shows the ungraded print, the
    // right of it the graded one. The controller publishes beforeImage only
    // while a comparison is meaningful; comparePosition is canvas-local.
    Q_PROPERTY(QImage beforeImage READ beforeImage WRITE setBeforeImage NOTIFY beforeImageChanged)
    Q_PROPERTY(bool compareEnabled READ compareEnabled WRITE setCompareEnabled NOTIFY compareEnabledChanged)
    Q_PROPERTY(double comparePosition READ comparePosition WRITE setComparePosition NOTIFY comparePositionChanged)
    Q_PROPERTY(bool fit READ fit WRITE setFit NOTIFY viewChanged)
    Q_PROPERTY(double scalePercent READ scalePercent NOTIFY viewChanged)
    Q_PROPERTY(QRectF imageRect READ imageRect NOTIFY viewChanged)
public:
    explicit FrameCanvas(QQuickItem* parent=nullptr);
    QImage image() const { return after_image_; }
    void setImage(const QImage& image);
    bool fit() const { return fit_; }
    void setFit(bool fit);
    double scalePercent() const;
    QRectF imageRect() const;
    QImage beforeImage() const { return before_image_; }
    void setBeforeImage(const QImage& image);
    bool compareEnabled() const { return compare_enabled_; }
    void setCompareEnabled(bool enabled);
    double comparePosition() const { return compare_position_; }
    void setComparePosition(double position);
    // The composite rule, shared by the paint node and the workflow gate: only
    // an enabled comparison of equal-size prints composites, everything else
    // degrades to the after print. The result is a CPU row copy —
    // [0,split) from the before print, [split,width) from the after print,
    // with split=round(position×width) — never a GPU material rewrite.
    static QImage composited(const QImage& after,const QImage& before,bool enabled,double position);
    Q_INVOKABLE void resetView(bool fit=true);
    Q_INVOKABLE void panBy(double x,double y);
signals:
    void imageChanged();
    void beforeImageChanged();
    void compareEnabledChanged();
    void comparePositionChanged();
    void viewChanged();
protected:
    QSGNode* updatePaintNode(QSGNode* old,UpdatePaintNodeData*) override;
    void geometryChange(const QRectF& next,const QRectF& previous) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseUngrabEvent() override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void itemChange(ItemChange,const ItemChangeData&) override;
private:
    double dpr() const;
    double scale() const;
    void clampPan();
    // image_ is the texture source: the composite of the two prints, or the
    // after print itself whenever no comparison is in effect.
    void recomposite();
    QImage after_image_,before_image_,image_;
    quint64 revision_=0;
    bool fit_=true,dragging_=false,compare_enabled_=false;
    double compare_position_=0.5;
    QPointF pan_,last_mouse_;
};
