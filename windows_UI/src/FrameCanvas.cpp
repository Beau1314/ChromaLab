#include "FrameCanvas.hpp"
#include <QColorSpace>
#include <QMouseEvent>
#include <QQuickWindow>
#include <QSGSimpleTextureNode>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace {class ImageNode final:public QSGSimpleTextureNode {public:quint64 revision=0;};}
FrameCanvas::FrameCanvas(QQuickItem* parent):QQuickItem(parent) {
    setFlag(ItemHasContents,true);setClip(true);setAcceptedMouseButtons(Qt::LeftButton);
}
void FrameCanvas::setImage(const QImage& image) {
    if(image.cacheKey()==after_image_.cacheKey())return;
    const bool changedSize=image.size()!=after_image_.size();after_image_=image;
    if(changedSize)pan_={};clampPan();emit imageChanged();emit viewChanged();
    recomposite();
}
void FrameCanvas::setBeforeImage(const QImage& image) {
    if(image.cacheKey()==before_image_.cacheKey())return;
    before_image_=image;emit beforeImageChanged();recomposite();
}
void FrameCanvas::setCompareEnabled(bool enabled) {
    if(enabled==compare_enabled_)return;
    compare_enabled_=enabled;emit compareEnabledChanged();recomposite();
}
void FrameCanvas::setComparePosition(double position) {
    if(!std::isfinite(position))return;
    position=std::clamp(position,0.0,1.0);
    if(position==compare_position_)return;
    compare_position_=position;emit comparePositionChanged();recomposite();
}
QImage FrameCanvas::composited(const QImage& after,const QImage& before,bool enabled,double position) {
    if(!enabled||after.isNull()||before.isNull()||before.size()!=after.size())return after;
    // The destination starts as a deep copy of the after print, so only the
    // before half is ever written. Rows are copied whole: any mismatching
    // format is converted first, and the split column rounds from the
    // fractional position.
    QImage result=after.copy();
    if(result.isNull())return after;
    result.setColorSpace(after.colorSpace());
    const QImage& source=before.format()==after.format()?before:before.convertToFormat(after.format());
    if(source.isNull())return result;
    const int split=std::clamp(int(std::lround(std::clamp(position,0.0,1.0)*after.width())),0,after.width());
    if(split<=0)return result;
    const int bpp=std::max(1,after.depth()/8);
    const std::size_t leftBytes=std::size_t(split)*bpp;
    for(int y=0;y<result.height();++y)
        std::memcpy(result.scanLine(y),source.constScanLine(y),leftBytes);
    return result;
}
void FrameCanvas::recomposite() {
    image_=composited(after_image_,before_image_,compare_enabled_,compare_position_);
    ++revision_;update();
}
void FrameCanvas::setFit(bool value) {if(value==fit_)return;fit_=value;pan_={};update();emit viewChanged();}
void FrameCanvas::resetView(bool value) {fit_=value;pan_={};update();emit viewChanged();}
double FrameCanvas::dpr() const {return window()?window()->effectiveDevicePixelRatio():1.0;}
double FrameCanvas::scale() const {
    if(after_image_.isNull()||width()<=0||height()<=0)return 1;
    return fit_?std::min(width()*dpr()/after_image_.width(),height()*dpr()/after_image_.height()):1.0;
}
double FrameCanvas::scalePercent() const {return after_image_.isNull()?0:scale()*100;}
QRectF FrameCanvas::imageRect() const {
    if(after_image_.isNull())return {};
    const QSizeF size(after_image_.width()*scale()/dpr(),after_image_.height()*scale()/dpr());
    auto position=[](double extent,double canvas,double pan){
        return extent<=canvas?(canvas-extent)*0.5:std::clamp((canvas-extent)*0.5+pan,canvas-extent,0.0);
    };
    double x=position(size.width(),width(),fit_?0:pan_.x());
    double y=position(size.height(),height(),fit_?0:pan_.y());
    // At 100%, align the image origin to the physical pixel grid as well as
    // preserving its physical size. Fractional centring must not shift grain.
    if(std::abs(scale()-1.0)<1e-12){x=std::round(x*dpr())/dpr();y=std::round(y*dpr())/dpr();}
    return {x,y,size.width(),size.height()};
}
void FrameCanvas::clampPan() {
    const auto rect=imageRect();
    pan_.setX(rect.width()>width()?rect.x()-(width()-rect.width())*0.5:0);
    pan_.setY(rect.height()>height()?rect.y()-(height()-rect.height())*0.5:0);
}
void FrameCanvas::panBy(double x,double y) {
    if(fit_||after_image_.isNull())return;
    pan_+=QPointF(x,y);clampPan();update();emit viewChanged();
}
QSGNode* FrameCanvas::updatePaintNode(QSGNode* old,UpdatePaintNodeData*) {
    auto* node=static_cast<ImageNode*>(old);
    if(image_.isNull()||!window()){delete node;return nullptr;}
    if(!node)node=new ImageNode;
    if(node->revision!=revision_) {
        auto* texture=window()->createTextureFromImage(image_);
        if(!texture){delete node;return nullptr;}
        node->setTexture(texture);node->setOwnsTexture(true);node->revision=revision_;
    }
    node->setFiltering(scale()>=1.0?QSGTexture::Nearest:QSGTexture::Linear);
    node->setRect(imageRect());return node;
}
void FrameCanvas::geometryChange(const QRectF& next,const QRectF& previous) {
    QQuickItem::geometryChange(next,previous);clampPan();update();emit viewChanged();
}
void FrameCanvas::itemChange(ItemChange change,const ItemChangeData& data) {
    QQuickItem::itemChange(change,data);
    if(change==ItemDevicePixelRatioHasChanged){clampPan();update();emit viewChanged();}
}
void FrameCanvas::mousePressEvent(QMouseEvent* e) {
    if(fit_||after_image_.isNull()){e->ignore();return;}dragging_=true;last_mouse_=e->position();e->accept();
}
void FrameCanvas::mouseMoveEvent(QMouseEvent* e) {if(dragging_){const auto delta=e->position()-last_mouse_;last_mouse_=e->position();panBy(delta.x(),delta.y());e->accept();}else e->ignore();}
void FrameCanvas::mouseReleaseEvent(QMouseEvent* e) {dragging_=false;e->accept();}
void FrameCanvas::mouseUngrabEvent(){dragging_=false;}
void FrameCanvas::mouseDoubleClickEvent(QMouseEvent* e){resetView(!fit_);e->accept();}
