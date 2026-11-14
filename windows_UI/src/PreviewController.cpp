#include "PreviewController.hpp"
#include "Histogram.hpp"
#include <QColorSpace>
#include <QMetaObject>
#include <QDir>
#include <QFileInfo>
#include <QSet>
#include <QStandardPaths>
#include <algorithm>
#include <cmath>

namespace {
namespace desktop=spk::desktop;
namespace output=desktop::output;
namespace editing=desktop::editing;
namespace l2=desktop::layer2;
namespace histogram=desktop::histogram;
QString text(const std::string& s) { return QString::fromUtf8(s.data(),qsizetype(s.size())); }
std::filesystem::path path(const QUrl& u) { return std::filesystem::path(u.toLocalFile().toStdWString()); }
QVariantList list(const std::vector<desktop::Stock>& stocks) {
    QVariantList result;
    for(const auto& s:stocks) result.push_back(QVariantMap{{"id",text(s.id)},{"name",text(s.label)},{"positive",s.is_positive}});
    return result;
}
bool sameSettings(desktop::RenderSettings a, desktop::RenderSettings b) {
    const bool exposure=std::abs(a.print_exposure-b.print_exposure)<1e-12;
    a.print_exposure=b.print_exposure;
    return exposure&&a==b;
}
bool sameState(const editing::State& a,const editing::State& b) {
    return sameSettings(a.settings,b.settings)&&a.mode==b.mode&&a.geometry==b.geometry&&a.layer2==b.layer2;
}
QVariantList pointList(const std::vector<QPointF>& points) {
    QVariantList result;
    for(const auto& point:points)result.push_back(QVariant::fromValue(point));
    return result;
}
QVariantMap zoneMap(const l2::ColorZone& zone) {
    return {{"hue",zone.hue},{"saturation",zone.saturation},{"luminance",zone.luminance}};
}
// The five curve channels, in the desktop's CURVE_CHANNELS order.
const l2::Curve* curveOf(const l2::Adjustments& adjustments,const QString& channel) {
    const auto& curves=adjustments.curves;
    if(channel==QLatin1String("rgb"))return &curves.rgb;
    if(channel==QLatin1String("luma"))return &curves.luma;
    if(channel==QLatin1String("red"))return &curves.red;
    if(channel==QLatin1String("green"))return &curves.green;
    if(channel==QLatin1String("blue"))return &curves.blue;
    return nullptr;
}
l2::Curve* curveOf(l2::Adjustments& adjustments,const QString& channel) {
    return const_cast<l2::Curve*>(curveOf(static_cast<const l2::Adjustments&>(adjustments),channel));
}
QImage visibleImage(const desktop::FramePtr& frame,const QImage& full,const output::Geometry& geometry,
                    const l2::Adjustments& layer2) {
    // `full` already carries the grade (gradedDisplayImage). Only the
    // straightening path re-derives pixels from the frame, and it re-grades
    // from the frame's 16-bit print through Task 3's three-argument form.
    if(geometry.quarterTurns==0&&!geometry.flipHorizontal&&!geometry.flipVertical&&geometry.straightenDegrees==0)
        return output::displayImage(full,geometry.crop);
    return output::displayFrame(frame,geometry,layer2);
}
output::ExportOptions exportOptions(int format,int quality,int maxEdge) {
    if(format<0||format>3||quality<1||quality>100||maxEdge<0||maxEdge>100000)
        throw std::runtime_error("Invalid export options");
    return {static_cast<output::Format>(format),quality,maxEdge};
}
}
QImage PreviewController::imageFromFrame(const desktop::FramePtr& frame) {
    // QImage is implicitly shared; the cleanup holder keeps the independently
    // owned engine result and its display bytes alive on the scene-graph side.
    auto held=std::make_unique<desktop::FramePtr>(frame);
    QImage image(frame->bgra8.data(),int(frame->width()),int(frame->height()),
                 qsizetype(frame->width())*4,QImage::Format_ARGB32,
                 [](void* p){delete static_cast<desktop::FramePtr*>(p);},held.get());
    if(image.isNull()) throw std::runtime_error("Cannot create the display image");
    held.release();
    image.setColorSpace(QColorSpace::SRgb);
    return image;
}

PreviewController::PreviewController(std::filesystem::path resources,QObject* parent,ImageFactory factory,QString editDirectory)
    :QObject(parent),resources_(resources),
     edit_store_(editDirectory.isEmpty()?QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)+"/Edits":editDirectory),
     image_factory_(factory?std::move(factory):ImageFactory(imageFromFrame)) {
    auto_apply_.setSingleShot(true);auto_apply_.setInterval(180);
    connect(&auto_apply_,&QTimer::timeout,this,[this]{if(dirty())apply();});
    // The property's no-frame state: four all-zero channels before any
    // publication (count() of a null image is exactly that).
    computeHistogram();
    save_timer_.setSingleShot(true);save_timer_.setInterval(350);
    connect(&save_timer_,&QTimer::timeout,this,&PreviewController::flushEdits);
    worker_=std::thread([this,resources=std::move(resources)]{workerLoop(resources);});
}
PreviewController::~PreviewController() {
    auto_apply_.stop();
    endEditGesture();flushEdits();
    if(cancel_batch_)cancel_batch_->store(true);
    ++generation_;
    {std::lock_guard lock(mutex_);stopping_=true;jobs_.clear();}
    wake_.notify_one();
    if(worker_.joinable()) worker_.join();
}
void PreviewController::workerLoop(std::filesystem::path resources) {
    std::unique_ptr<desktop::PreviewHost> host;
    Reply init;init.operation="init";
    try {host=std::make_unique<desktop::PreviewHost>(resources);init.catalog=host->catalog();}
    catch(const std::exception& e){init.error=text(e.what());}
    deliver(std::move(init));
    if(!host)return;
    for(;;) {
        Job job;
        {std::unique_lock lock(mutex_);wake_.wait(lock,[&]{return stopping_||!jobs_.empty();});
         if(stopping_)break;job=std::move(jobs_.front());jobs_.pop_front();}
        deliver(job(*host));
    }
}
void PreviewController::deliver(Reply reply) {
    QMetaObject::invokeMethod(this,[this,reply=std::move(reply)]() mutable {accept(std::move(reply));},Qt::QueuedConnection);
}
void PreviewController::submit(Job job,QString status,QString operation) {
    auto_apply_.stop();active_operation_=std::move(operation);
    busy_=true;failed_=false;status_=std::move(status);emit stateChanged();emit historyChanged();
    {std::lock_guard lock(mutex_);jobs_.push_back(std::move(job));}
    wake_.notify_one();
}
void PreviewController::accept(Reply reply) {
    if(reply.generation!=generation_)return; // a retired generation never replaces the canvas
    const bool superseded=(reply.operation=="render"&&reply.settings_revision!=settings_revision_)
        ||(reply.operation=="layer2"&&reply.settings_revision!=layer2_revision_);
    busy_=false;active_operation_.clear();failed_=!superseded&&!reply.error.isEmpty();
    if(superseded) {
        // The worker may finish an earlier choice after the next click. Keep
        // both the displayed frame and the newest desired controls; never flash
        // the retired result or restore its settings over the user's last edit.
        status_=dirty()?QStringLiteral("正在更新预览…"):QStringLiteral("预览已是当前选择");
    } else if(failed_) {
        status_=reply.error;
        if(frame_&&reply.operation!="export"&&reply.operation!="batch-export")restoreSettings();
    } else if(reply.operation=="init") {
        ready_=true;catalog_=std::move(reply.catalog);films_=list(catalog_.films);papers_=list(catalog_.papers);
        restoreSettings();emit catalogChanged();status_=QStringLiteral("就绪 · 打开 RAW 开始");
    } else if(reply.operation=="layer2") {
        // The fast path's job only re-sequences the publication behind the
        // worker queue; the pixels come from the retained frame below.
        publishFromFrame(QStringLiteral("预览已更新"));
    } else if(reply.frame) {
        // Allocate the presentation wrapper before publishing any new UI state.
        try {
            // The opened photo's grade only commits with its successful
            // publication; a failure leaves the previous photo intact.
            auto nextLayer2=reply.operation=="open"?std::move(reply.layer2):layer2_;
            auto next=gradedDisplayImage(reply.frame,nextLayer2);
            if(next.isNull())throw std::runtime_error("Cannot create the display image");
            const auto geometry=reply.operation=="open"?reply.geometry:geometry_;
            auto visible=visibleImage(reply.frame,next,geometry,nextLayer2);
            layer2_=std::move(nextLayer2);
            frame_=std::move(reply.frame);full_image_=std::move(next);image_=std::move(visible);
            geometry_=geometry;
            refreshBeforeImage();
            computeHistogram();
            if(reply.operation=="open")active_index_=reply.library_index;
        }
        catch(const std::exception& e){failed_=true;status_=text(e.what());if(frame_)restoreSettings();}
        if(!failed_) {
            restoreSettings();
            updateThumbnail();
            status_=QStringLiteral("渲染完成 %1 秒%2")
                .arg(frame_->timings.render_ms/1000,0,'f',2)
                .arg(frame_->reprint()?QStringLiteral(" · 已复用胶片"):QString());
            emit frameChanged();emit geometryChanged();if(reply.operation=="open")emit sourceOpened();
        }
    } else if(reply.operation=="export") status_=QStringLiteral("已导出当前画面");
    else if(reply.operation=="batch-export")status_=reply.message;
    if(failed_&&reply.operation=="open"&&reply.library_index>=0&&reply.library_index<int(library_.size()))
        library_[reply.library_index].error=status_;
    if(failed_&&reply.operation!="export"&&reply.operation!="batch-export"&&active_index_>=0){
        auto& history=library_[active_index_].undo;
        while(!history.empty()&&sameState(history.back(),currentState()))history.pop_back();
    }
    if(reply.operation=="batch-export")cancel_batch_.reset();
    emit libraryChanged();
    emit historyChanged();
    emit stateChanged();
    if(reply.operation=="init")emit initialized(!failed_);
    else emit operationFinished(reply.operation,!failed_);
    // Export holds an immutable displayed frame. An edit made just before it
    // began still needs applying, including after an overwrite refusal. The
    // layer 2 publication runs the same courtesy: its job cancelled any
    // pending parameter debounce inside submit().
    if((superseded||reply.operation=="export"||reply.operation=="batch-export"
        ||reply.operation=="layer2")&&!busy_)schedulePreview();
    // A layer 2 edit during a busy job only recorded its intent; publish it now.
    if(layer2_pending_&&!busy_){layer2_pending_=false;scheduleLayer2();}
}
desktop::RenderSettings PreviewController::settings() const {
    desktop::RenderSettings s=extra_settings_;
    if(film_index_>=0&&std::size_t(film_index_)<catalog_.films.size())s.film_stock=catalog_.films[film_index_].id;
    if(paper_index_>=0&&std::size_t(paper_index_)<catalog_.papers.size())s.print_stock=catalog_.papers[paper_index_].id;
    s.print_exposure=std::exp2(-exposure_ev_);return s;
}
void PreviewController::restoreSettings() {
    const auto s=frame_?frame_->settings:desktop::RenderSettings{};
    loadSettings(s,frame_?frame_->decode_mode:desktop::DecodeMode::compatible16);
    saveActiveSettings();
}
void PreviewController::loadSettings(const desktop::RenderSettings& s,desktop::DecodeMode mode) {
    extra_settings_=s;
    for(std::size_t i=0;i<catalog_.films.size();++i)if(catalog_.films[i].id==s.film_stock)film_index_=int(i);
    for(std::size_t i=0;i<catalog_.papers.size();++i)if(catalog_.papers[i].id==s.print_stock)paper_index_=int(i);
    exposure_ev_=-std::log2(s.print_exposure);
    decode_mode_=mode==desktop::DecodeMode::headroom?1:0;
    emit settingsChanged();
}
bool PreviewController::dirty() const {
    if(!frame_)return false;
    return !sameSettings(settings(),frame_->settings)||
        decode_mode_!=(frame_->decode_mode==desktop::DecodeMode::headroom?1:0);
}
bool PreviewController::filmIsPositive() const {
    return film_index_>=0&&std::size_t(film_index_)<catalog_.films.size()&&catalog_.films[film_index_].is_positive;
}
QString PreviewController::fileName() const {return frame_?QString::fromStdWString(frame_->source.filename().wstring()):QString();}
QUrl PreviewController::sourceFolder() const {return frame_?QUrl::fromLocalFile(QString::fromStdWString(frame_->source.parent_path().wstring())):QUrl();}
QVariantMap PreviewController::metadata() const {
    if(!frame_)return {};
    return {{"width",image_.width()},{"height",image_.height()},
        {"sourceWidth",frame_->width()},{"sourceHeight",frame_->height()},
        {"camera",text(frame_->metadata.make+" "+frame_->metadata.model)},
        {"decodeMs",frame_->timings.decode_ms},{"renderMs",frame_->timings.render_ms},
        {"totalMs",frame_->timings.total_ms},{"cached",frame_->reprint()},{"scanFilm",frame_->scan_film},
        {"mode",frame_->decode_mode==desktop::DecodeMode::headroom?"headroom":"compatible16"}};
}
void PreviewController::schedulePreview(){if(!dialog_open_&&gesture_key_!="straightenDegrees"&&frame_&&dirty()&&!auto_apply_.isActive())auto_apply_.start();}
void PreviewController::setDialogOpen(bool open){
    if(dialog_open_==open)return;
    dialog_open_=open;
    if(open){endEditGesture();auto_apply_.stop();}else schedulePreview();
    emit stateChanged();emit historyChanged();
}
void PreviewController::changed(const QString& editKey){
    if(gesture_index_>=0&&gesture_key_!=editKey)endEditGesture();
    saveActiveSettings(true);
    ++settings_revision_;auto_apply_.stop();schedulePreview();
    if(frame_)status_=QStringLiteral("正在更新预览…");
    failed_=false;emit settingsChanged();emit stateChanged();
}
void PreviewController::setFilmIndex(int v){if(!settingsEditable()||v<0||v>=films_.size()||v==film_index_)return;film_index_=v;changed("filmIndex");}
void PreviewController::setPaperIndex(int v){if(!settingsEditable()||filmIsPositive()||v<0||v>=papers_.size()||v==paper_index_)return;paper_index_=v;changed("paperIndex");}
void PreviewController::setDecodeMode(int v){if(!settingsEditable()||v<0||v>1||v==decode_mode_)return;decode_mode_=v;changed("decodeMode");}
void PreviewController::setExposureEv(double v){if(!settingsEditable()||filmIsPositive()||!std::isfinite(v)||v< -2||v>2||v==exposure_ev_)return;exposure_ev_=v;changed("exposureEv");}
void PreviewController::resetSettings(){if(!settingsEditable())return;loadSettings({},desktop::DecodeMode::compatible16);changed();}
void PreviewController::openRaw(const QUrl& url) {
    // Changing sources and exporting remain single flight. Parameter edits
    // can be coalesced during a render, but cannot retarget an in-flight open.
    if(!ready_||busy_||!url.isLocalFile())return;
    saveActiveSettings();
    const int index=addFile(url);if(index<0)return;
    // A known photo restores its own saved settings. A genuinely new photo
    // still starts with the current preset; opening never overwrites a sidecar.
    openItem(index);
}
void PreviewController::openItem(int index) {
    endEditGesture();
    const auto item=library_.at(index);const auto p=path(item.url);
    const auto gen=++generation_;
    submit([p,item,gen,index](auto& host){Reply r;r.generation=gen;r.operation="open";r.library_index=index;r.geometry=item.geometry;r.layer2=item.layer2;
        try{r.frame=host.open_raw(p,item.mode,item.settings);}catch(const std::exception& e){r.error=text(e.what());}return r;},QStringLiteral("正在解码并渲染 RAW…"),"open");
}
void PreviewController::apply() {
    if(!frame_||busy_||dialog_open_)return;
    const auto s=settings();const auto shown=frame_;const auto gen=++generation_;const auto revision=settings_revision_;
    const auto mode=decode_mode_?desktop::DecodeMode::headroom:desktop::DecodeMode::compatible16;
    submit([s,shown,gen,revision,mode](auto& host){Reply r;r.generation=gen;r.settings_revision=revision;r.operation="render";
        try {
            // Publication can fail after a successful worker open (for example,
            // a display allocation failure). Reconcile against the image the
            // user is actually editing before reusing the worker's session.
            const auto active=host.current();
            r.frame=active&&active->source==shown->source&&active->decode_mode==mode
                ?host.render(s):host.open_raw(shown->source,mode,s);
        }catch(const std::exception& e){r.error=text(e.what());}return r;},QStringLiteral("正在更新胶片效果…"),"render");
}
void PreviewController::exportTiff(const QUrl& url) {
    exportImage(url,0,95,0);
}
void PreviewController::exportImage(const QUrl& url,int format,int quality,int maxLongEdge) {
    if(!frame_||busy_||!url.isLocalFile())return;
    const auto p=path(url);const auto held=frame_;const auto gen=++generation_;const auto geometry=geometry_;const auto resources=resources_;
    const auto layer2=layer2_; // the grade as the user saw it at submit time
    submit([p,held,gen,geometry,resources,layer2,format,quality,maxLongEdge](auto&){Reply r;r.generation=gen;r.operation="export";
        try{output::exportFrame(held,p,geometry,exportOptions(format,quality,maxLongEdge),resources,layer2);}catch(const std::exception& e){r.error=text(e.what());}return r;},QStringLiteral("正在导出当前画面…"),"export");
}

QVariantMap PreviewController::adjustments() const {
    const auto& s=extra_settings_;
    return {{"filmExposureEv",s.film_exposure_ev},{"filmFormatMm",s.film_format_mm},
        {"grainActive",s.grain_active},{"grainAmount",s.grain_amount},
        {"halationActive",s.halation_active},{"halationAmount",s.halation_amount},
        {"glareActive",s.glare_active},{"glareAmount",s.glare_amount},
        {"yFilterShift",s.y_filter_shift},{"mFilterShift",s.m_filter_shift}};
}
void PreviewController::setAdjustment(const QString& name,const QVariant& value) {
    if(!settingsEditable())return;
    auto next=extra_settings_;
    if(name=="grainActive")next.grain_active=value.toBool();
    else if(name=="halationActive")next.halation_active=value.toBool();
    else if(name=="glareActive")next.glare_active=value.toBool();
    else {
        bool ok=false;const double v=value.toDouble(&ok);if(!ok||!std::isfinite(v))return;
        double* field=nullptr;double low=0,high=0;
        if(name=="filmExposureEv"){field=&next.film_exposure_ev;low=-8;high=8;}
        else if(name=="filmFormatMm"){field=&next.film_format_mm;low=4;high=200;}
        else if(name=="grainAmount"){field=&next.grain_amount;high=2;}
        else if(name=="halationAmount"){field=&next.halation_amount;high=4;}
        else if(name=="glareAmount"){field=&next.glare_amount;high=30;}
        else if(name=="yFilterShift"){field=&next.y_filter_shift;low=-1;high=1;}
        else if(name=="mFilterShift"){field=&next.m_filter_shift;low=-1;high=1;}
        else return;
        if(v<low||v>high)return;*field=v;
    }
    if(next==extra_settings_)return;
    extra_settings_=next;changed(name);
}

// --- Layer 2: the scan-side grade on the controller's fast path ---------------

QImage PreviewController::gradedDisplayImage(const desktop::FramePtr& frame,const l2::Adjustments& adjustments) const {
    // A neutral grade stays byte-identical: applyImage would still run its
    // per-pixel chain, so it is skipped outright and the factory's
    // external-buffer wrapper is returned zero-copy.
    auto image=image_factory_(frame);
    if(!image.isNull()&&!l2::isNeutral(adjustments)) {
        // The copy is required before the first write: the factory wraps the
        // frame's display bytes in a non-const external-buffer QImage whose
        // refcount is 1 and which carries no ro_data, so Qt's detach() would
        // NOT deep-copy on bits() and applyImage would grade the retained
        // frame's bgra8 in place. The engine owns those bytes — every
        // re-grade must start from pristine pixels (repeated fast-path
        // edits, undo back to a neutral grade, export/canvas parity) — so a
        // visible grade always writes into an explicit deep copy.
        image=image.copy();
        l2::applyImage(image,adjustments);
    }
    return image;
}
void PreviewController::changedLayer2(const QString& editKey) {
    // The engine's parameters did not change: settings_revision_ and the
    // generation stay put (in-flight engine replies remain valid). Undo and
    // gestures ride the same mechanics as every other edit family.
    if(gesture_index_>=0&&gesture_key_!=editKey)endEditGesture();
    saveActiveSettings(true);
    ++layer2_revision_;
    scheduleLayer2();
    if(frame_)status_=QStringLiteral("正在更新预览…");
    failed_=false;emit settingsChanged();emit stateChanged();
}
void PreviewController::scheduleLayer2() {
    // Nothing is on screen yet: there is no retained print to republish, and
    // a reply would leave a stale "updating" status behind.
    if(!frame_)return;
    if(busy_){layer2_pending_=true;return;} // accept() re-schedules after the reply
    const auto gen=generation_;const auto revision=layer2_revision_;
    submit([gen,revision](desktop::PreviewHost&){Reply r;r.generation=gen;
        r.settings_revision=revision;r.operation="layer2";return r;},
        QStringLiteral("正在更新预览…"),"layer2");
}
void PreviewController::refreshBeforeImage() {
    // The before half is the current film/paper print WITHOUT Layer 2 —
    // "冲洗后、编辑前". It is only worth computing while the comparison is
    // on and the grade actually differs from that print; otherwise the two
    // halves would be identical and the canvas shows the graded image alone.
    if(!frame_||!compare_enabled_||l2::isNeutral(layer2_)){before_image_={};return;}
    try {
        // The ungraded full skips the grade entirely (the raw wrapper) and the
        // visible before image walks the same geometry path as the graded one,
        // with the grade held at neutral — only the grade differs between the
        // halves. A failure here (for example an allocation failure) leaves
        // the before half empty, which the canvas displays as after-only.
        const auto ungraded=image_factory_(frame_);
        before_image_=visibleImage(frame_,ungraded,geometry_,l2::Adjustments{});
    } catch(const std::exception&){before_image_={};}
}
void PreviewController::setCompareEnabled(bool enabled) {
    if(compare_enabled_==enabled)return;
    compare_enabled_=enabled;
    // Toggling the comparison republishes the retained print: the before view
    // only exists while the comparison is on and the grade is non-neutral.
    if(frame_)publishFromFrame(QStringLiteral("预览已更新"));
    emit compareEnabledChanged();
}
void PreviewController::publishFromFrame(const QString& status) {
    if(!frame_)return;
    try {
        auto next=gradedDisplayImage(frame_,layer2_);
        auto visible=visibleImage(frame_,next,geometry_,layer2_);
        full_image_=std::move(next);image_=std::move(visible);
        refreshBeforeImage();
        computeHistogram();
    } catch(const std::exception& e){failed_=true;status_=text(e.what());emit stateChanged();return;}
    updateThumbnail();
    failed_=false;status_=status;
    emit frameChanged();emit stateChanged();
}
void PreviewController::computeHistogram() {
    // The bins describe exactly the pixels the canvas shows: the published
    // preview is the geometry+grade output, and this runs after image_ is
    // final at every publication site (accept's render/open branch,
    // publishFromFrame, setGeometry, restoreHistory). A null image counts to
    // the all-zero channels the no-frame contract promises. The 4×256 int
    // QVariantList conversion is 1024 values, cheap next to the counting.
    const auto bins=histogram::count(image_);
    QVariantList channels;
    channels.reserve(4);
    for(const auto& channel:bins) {
        QVariantList row;
        row.reserve(histogram::kBins);
        for(const int bin:channel)row.push_back(bin);
        channels.push_back(row);
    }
    histogram_=std::move(channels);
    emit histogramChanged();
}
QVariantMap PreviewController::layer2() const {
    QVariantMap curves;
    curves.insert("rgb",pointList(layer2_.curves.rgb.points));
    curves.insert("luma",pointList(layer2_.curves.luma.points));
    curves.insert("red",pointList(layer2_.curves.red.points));
    curves.insert("green",pointList(layer2_.curves.green.points));
    curves.insert("blue",pointList(layer2_.curves.blue.points));
    QVariantMap colorBalance;
    colorBalance.insert("master",zoneMap(layer2_.colorBalance.master));
    colorBalance.insert("shadows",zoneMap(layer2_.colorBalance.shadows));
    colorBalance.insert("midtones",zoneMap(layer2_.colorBalance.midtones));
    colorBalance.insert("highlights",zoneMap(layer2_.colorBalance.highlights));
    return {{"enabled",layer2_.enabled},{"temperature",layer2_.temperature},{"tint",layer2_.tint},
        {"exposure",layer2_.exposure},{"contrast",layer2_.contrast},{"brightness",layer2_.brightness},
        {"saturation",layer2_.saturation},{"highlights",layer2_.highlights},{"shadows",layer2_.shadows},
        {"blackPoint",layer2_.blackPoint},{"whitePoint",layer2_.whitePoint},
        {"curves",curves},{"colorBalance",colorBalance},
        {"vignette",QVariantMap{{"amount",layer2_.vignette.amount},{"midpoint",layer2_.vignette.midpoint}}}};
}
void PreviewController::setLayer2(const QString& name,double value) {
    if(!settingsEditable()||!std::isfinite(value))return;
    auto next=layer2_;
    if(name=="enabled") {
        if((value!=0)==next.enabled)return;
        next.enabled=value!=0;
    } else {
        double* field=nullptr;double low=0,high=0;
        if(name=="temperature"){field=&next.temperature;low=-100;high=100;}
        else if(name=="tint"){field=&next.tint;low=-100;high=100;}
        else if(name=="exposure"){field=&next.exposure;low=-5;high=5;}
        else if(name=="contrast"){field=&next.contrast;low=-100;high=100;}
        else if(name=="brightness"){field=&next.brightness;low=-100;high=100;}
        else if(name=="saturation"){field=&next.saturation;low=-100;high=100;}
        else if(name=="highlights"){field=&next.highlights;low=-100;high=100;}
        else if(name=="shadows"){field=&next.shadows;low=-100;high=100;}
        else if(name=="blackPoint"){field=&next.blackPoint;low=0;high=100;}
        else if(name=="whitePoint"){field=&next.whitePoint;low=0;high=100;}
        else if(name=="vignetteAmount"){field=&next.vignette.amount;low=-100;high=100;}
        else if(name=="vignetteMidpoint"){field=&next.vignette.midpoint;low=0;high=100;}
        else return;
        if(value<low||value>high)return;
        *field=value;
    }
    if(next==layer2_)return;
    layer2_=next;changedLayer2(name);
}
void PreviewController::curveInsertPoint(const QString& channel,double x,double y) {
    if(!settingsEditable())return;
    auto* curve=curveOf(layer2_,channel);
    if(!curve||!std::isfinite(x)||!std::isfinite(y))return;
    l2::insertPoint(*curve,x,y);
    changedLayer2(QStringLiteral("curve:")+channel);
}
void PreviewController::curveMovePoint(const QString& channel,int index,double x,double y) {
    if(!settingsEditable())return;
    auto* curve=curveOf(layer2_,channel);
    if(!curve||index<0||index>=int(curve->points.size())||!std::isfinite(x)||!std::isfinite(y))return;
    const l2::Curve before=*curve;
    l2::movePoint(*curve,index,x,y);
    if(*curve==before)return; // an endpoint or out-of-range move is a no-op
    changedLayer2(QStringLiteral("curve:")+channel);
}
void PreviewController::curveRemovePoint(const QString& channel,int index) {
    if(!settingsEditable())return;
    auto* curve=curveOf(layer2_,channel);
    if(!curve||index<0||index>=int(curve->points.size()))return;
    const l2::Curve before=*curve;
    l2::removePoint(*curve,index);
    if(*curve==before)return; // endpoints and short curves refuse removal
    changedLayer2(QStringLiteral("curve:")+channel);
}
void PreviewController::curveReset(const QString& channel) {
    if(!settingsEditable())return;
    auto* curve=curveOf(layer2_,channel);
    if(!curve)return;
    if(*curve==l2::identityCurve())return;
    *curve=l2::identityCurve();
    changedLayer2(QStringLiteral("curve:")+channel);
}
QVariantList PreviewController::curvePoints(const QString& channel) const {
    const auto* curve=curveOf(layer2_,channel);
    return curve?pointList(curve->points):QVariantList{};
}
double PreviewController::curveEval(const QString& channel,double x) const {
    if(!std::isfinite(x))return 0;
    x=std::clamp(x,0.0,1.0);
    const auto* curve=curveOf(layer2_,channel);
    return curve?l2::evaluateCurve(*curve,x):x;
}
void PreviewController::setZone(int zone,int axis,double value) {
    if(!settingsEditable()||!std::isfinite(value)||zone<0||zone>3||axis<0||axis>2)return;
    static const char* zoneNames[4]={"master","shadows","midtones","highlights"};
    auto next=layer2_;
    auto& target=zone==0?next.colorBalance.master:zone==1?next.colorBalance.shadows
        :zone==2?next.colorBalance.midtones:next.colorBalance.highlights;
    // The desktop's own ranges: hue in degrees, saturation and luminance as
    // fractions of the zone's strength.
    if(axis==0){if(value<0||value>360)return;target.hue=value;}
    else if(axis==1){if(value<0||value>1)return;target.saturation=value;}
    else {if(value<-1||value>1)return;target.luminance=value;}
    if(next==layer2_)return;
    layer2_=next;
    changedLayer2(QStringLiteral("colorBalance:")+zoneNames[zone]);
}
QVariantList PreviewController::libraryItems() const {
    QVariantList result;
    for(int i=0;i<int(library_.size());++i) {
        const auto& item=library_[i];
        result.push_back(QVariantMap{{"name",QFileInfo(item.url.toLocalFile()).fileName()},
            {"url",item.url},{"thumbnail",item.thumbnail},{"active",i==active_index_},
            {"selected",item.selected},{"error",item.error}});
    }
    return result;
}
int PreviewController::selectedCount() const {
    int count=0;for(const auto& item:library_)count+=item.selected;return count;
}
void PreviewController::saveActiveSettings(bool recordHistory) {
    if(active_index_<0||active_index_>=int(library_.size()))return;
    auto& item=library_[active_index_];const auto desired=settings();
    const auto mode=decode_mode_?desktop::DecodeMode::headroom:desktop::DecodeMode::compatible16;
    // A saved pending edit has no rendered thumbnail yet. Do not let the
    // strip present an older look as if it represented the new settings.
    const bool changed=!sameSettings(item.settings,desired)||item.mode!=mode
        ||item.geometry!=geometry_||item.layer2!=layer2_;
    if(changed){
        if(recordHistory)recordEdit(item,itemState(item));
        item.thumbnail={};item.pending_save=true;
    }
    item.settings=desired;item.mode=mode;
    item.geometry=geometry_;
    item.layer2=layer2_;
    if(changed){scheduleSave();emit libraryChanged();emit historyChanged();}
}
void PreviewController::updateThumbnail() {
    if(active_index_<0||active_index_>=int(library_.size()))return;
    auto& item=library_[active_index_];
    item.thumbnail=dirty()?QImage{}:image_.scaled(180,120,Qt::KeepAspectRatio,Qt::SmoothTransformation);
    item.error.clear();emit libraryChanged();
}
int PreviewController::addFile(const QUrl& url) {
    if(!url.isLocalFile())return -1;
    const QFileInfo info(url.toLocalFile());
    if(!info.isFile())return -1;
    const auto local=QUrl::fromLocalFile(info.canonicalFilePath());
    for(int i=0;i<int(library_.size());++i)
        if(library_[i].url.toLocalFile().compare(local.toLocalFile(),Qt::CaseInsensitive)==0)return i;
    LibraryItem item;item.url=local;item.settings=settings();
    item.mode=decode_mode_?desktop::DecodeMode::headroom:desktop::DecodeMode::compatible16;
    item.layer2=layer2_;
    const auto stored=edit_store_.load(local,catalog_);
    item.save_blocked=stored.blocked;item.save_warning=stored.warning;
    if(stored.state){item.settings=stored.state->settings;item.mode=stored.state->mode;item.geometry=stored.state->geometry;
        item.layer2=stored.state->layer2;item.has_stored=true;}
    library_.push_back(std::move(item));emit libraryChanged();return int(library_.size())-1;
}
void PreviewController::openFiles(const QVariantList& urls) {
    if(!ready_||busy_)return;
    saveActiveSettings();int first=-1;
    for(const auto& url:urls){const int index=addFile(url.toUrl());if(first<0&&index>=0)first=index;}
    if(first>=0)openItem(first);
}
void PreviewController::selectImage(int index) {
    if(!ready_||busy_||dialog_open_||index<0||index>=int(library_.size())||index==active_index_)return;
    saveActiveSettings();openItem(index);
}
void PreviewController::setItemSelected(int index,bool selected) {
    if(busy_||dialog_open_||index<0||index>=int(library_.size()))return;
    library_[index].selected=selected;emit libraryChanged();
}
void PreviewController::selectAllPhotos(bool selected) {
    if(busy_||dialog_open_)return;
    for(auto& item:library_)item.selected=selected;emit libraryChanged();
}
void PreviewController::syncSelectedSettings(bool includeCrop) {
    if(!frame_||busy_||dialog_open_)return;
    saveActiveSettings();int count=0;
    for(int i=0;i<int(library_.size());++i)if(i!=active_index_&&library_[i].selected){
        auto& item=library_[i];auto next=currentState();
        if(!includeCrop)next.geometry=item.geometry;
        if(sameState(itemState(item),next))continue;
        // Each recipient owns its own undo history, just like its settings.
        item.undo.push_back(itemState(item));if(item.undo.size()>64)item.undo.erase(item.undo.begin());item.redo.clear();
        item.settings=next.settings;item.mode=next.mode;item.geometry=next.geometry;item.layer2=next.layer2;item.pending_save=true;
        item.thumbnail={};item.error.clear();++count;
    }
    status_=QStringLiteral("已将当前参数同步至 %1 张照片%2").arg(count)
        .arg(includeCrop?QStringLiteral("（含裁切）"):QStringLiteral("（各自保留裁切）"));
    scheduleSave();emit libraryChanged();emit historyChanged();emit stateChanged();
}
void PreviewController::setCropRect(const QRectF& crop) {
    auto next=geometry_;next.crop=crop;setGeometry(next,"cropRect");
}
void PreviewController::setGeometry(const output::Geometry& geometry,const QString& editKey) {
    if(!frame_||busy_||dialog_open_||geometry==geometry_)return;
    try {
        auto next=visibleImage(frame_,full_image_,geometry,layer2_);
        if(gesture_index_>=0&&gesture_key_!=editKey)endEditGesture();
        image_=std::move(next);geometry_=geometry;refreshBeforeImage();computeHistogram();
        saveActiveSettings(true);updateThumbnail();
        failed_=false;emit geometryChanged();emit frameChanged();emit stateChanged();
    } catch(const std::exception& e){failed_=true;status_=text(e.what());emit stateChanged();}
}
void PreviewController::resetCrop(){setCropRect(QRectF(0,0,1,1));}
void PreviewController::setCropAspect(double ratio) {
    if(!frame_||!std::isfinite(ratio)||ratio<0)return;
    if(ratio==0){resetCrop();return;}
    // Ratios describe the displayed result, even after a quarter turn.
    if(geometry_.quarterTurns%2)ratio=1/ratio;
    const double original=double(frame_->width())/frame_->height();
    double w=1,h=1;
    if(original>ratio)w=ratio/original;else h=original/ratio;
    setCropRect(QRectF((1-w)/2,(1-h)/2,w,h));
}
void PreviewController::cancelExport(){if(cancel_batch_)cancel_batch_->store(true);}

editing::State PreviewController::itemState(const LibraryItem& item) {
    return {item.settings,item.mode,item.geometry,item.layer2};
}
editing::State PreviewController::currentState() const {
    return {settings(),decode_mode_?desktop::DecodeMode::headroom:desktop::DecodeMode::compatible16,geometry_,layer2_};
}
bool PreviewController::canUndo() const {
    return settingsEditable()&&active_index_>=0&&!library_[active_index_].undo.empty();
}
bool PreviewController::canRedo() const {
    return settingsEditable()&&active_index_>=0&&!library_[active_index_].redo.empty();
}
void PreviewController::recordEdit(LibraryItem& item,const editing::State& before) {
    if(gesture_index_!=active_index_||!gesture_recorded_) {
        item.undo.push_back(before);
        if(item.undo.size()>64)item.undo.erase(item.undo.begin());
        item.redo.clear();
        if(gesture_index_==active_index_)gesture_recorded_=true;
    }
}
void PreviewController::beginEditGesture(const QString& key) {
    if(!settingsEditable()||active_index_<0||key.isEmpty())return;
    endEditGesture();gesture_index_=active_index_;gesture_key_=key;gesture_recorded_=false;gesture_redo_=library_[active_index_].redo;
    if(key=="straightenDegrees")auto_apply_.stop();
}
void PreviewController::endEditGesture() {
    const bool heldGeometry=gesture_key_=="straightenDegrees";
    if(gesture_index_>=0&&gesture_index_<int(library_.size())&&gesture_recorded_) {
        auto& item=library_[gesture_index_];
        // A drag returning to its starting value is not an undo step.
        if(!item.undo.empty()&&sameState(item.undo.back(),itemState(item))){item.undo.pop_back();item.redo=std::move(gesture_redo_);}
    }
    gesture_index_=-1;gesture_key_.clear();gesture_recorded_=false;gesture_redo_.clear();emit historyChanged();
    if(heldGeometry)schedulePreview();
}
void PreviewController::undo(){restoreHistory(false);}
void PreviewController::redo(){restoreHistory(true);}
void PreviewController::restoreHistory(bool redo) {
    if(!frame_||!settingsEditable()||active_index_<0)return;
    endEditGesture();
    auto& item=library_[active_index_];auto& from=redo?item.redo:item.undo;
    auto& to=redo?item.undo:item.redo;
    if(from.empty())return;
    const auto target=from.back();
    try {
        // Prepare pixels before moving the history cursor. Allocation or
        // validation failure leaves the frame and both stacks intact. A grade
        // change re-grades the retained print on this fast path — the engine
        // frame never moves.
        const bool gradeChangedNow=target.layer2!=layer2_;
        const bool geometryChangedNow=target.geometry!=geometry_;
        auto nextFull=gradeChangedNow?gradedDisplayImage(frame_,target.layer2):full_image_;
        auto next=(gradeChangedNow||geometryChangedNow)
            ?visibleImage(frame_,nextFull,target.geometry,target.layer2):image_;
        if(gradeChangedNow)++layer2_revision_;
        to.push_back(currentState());if(to.size()>64)to.erase(to.begin());from.pop_back();
        geometry_=target.geometry;layer2_=target.layer2;
        full_image_=std::move(nextFull);image_=std::move(next);
        if(gradeChangedNow||geometryChangedNow)refreshBeforeImage();
        // The pixels moved on this fast path, so the histogram moves with
        // them even before the debounced engine render republishes.
        if(gradeChangedNow||geometryChangedNow)computeHistogram();
        loadSettings(target.settings,target.mode);saveActiveSettings();
        ++settings_revision_;auto_apply_.stop();schedulePreview();updateThumbnail();
        failed_=false;status_=redo?QStringLiteral("已重做当前照片调整"):QStringLiteral("已撤销当前照片调整");
        if(geometryChangedNow)emit geometryChanged();
        if(geometryChangedNow||gradeChangedNow)emit frameChanged();
        emit settingsChanged();emit stateChanged();emit historyChanged();
    }catch(const std::exception& e){failed_=true;status_=text(e.what());emit stateChanged();}
}
void PreviewController::scheduleSave() {
    save_timer_.start();emit stateChanged();
}
void PreviewController::flushEdits() {
    save_timer_.stop();
    for(auto& item:library_)if(item.pending_save&&!item.save_blocked) {
        try {edit_store_.save(item.url,itemState(item));item.pending_save=false;item.has_stored=true;item.save_warning.clear();}
        catch(const std::exception& e){item.save_warning=text(e.what());}
    }
    emit stateChanged();
}
QString PreviewController::persistenceWarning() const {
    QStringList warnings;
    for(const auto& item:library_)if(!item.save_warning.isEmpty())
        warnings.append(QFileInfo(item.url.toLocalFile()).fileName()+": "+item.save_warning);
    return warnings.join("\n");
}
QString PreviewController::savedStatus() const {
    if(!persistenceWarning().isEmpty())return QStringLiteral("部分设置未保存，请查看下方提示");
    for(const auto& item:library_)if(item.pending_save)return QStringLiteral("正在自动保存设置…");
    if(active_index_>=0&&library_[active_index_].has_stored)return QStringLiteral("当前照片设置已自动保存");
    return QStringLiteral("调整将自动保存，重新打开照片时恢复");
}
// Flips are stored in output axes. Rotating the displayed image also swaps
// those axes; without this a mirrored photo would turn the opposite way.
void PreviewController::rotateClockwise(){auto next=geometry_;next.quarterTurns=(next.quarterTurns+1)%4;std::swap(next.flipHorizontal,next.flipVertical);setGeometry(next,"quarterTurns");}
void PreviewController::rotateCounterClockwise(){auto next=geometry_;next.quarterTurns=(next.quarterTurns+3)%4;std::swap(next.flipHorizontal,next.flipVertical);setGeometry(next,"quarterTurns");}
void PreviewController::setFlipHorizontal(bool value){auto next=geometry_;next.flipHorizontal=value;setGeometry(next,"flipHorizontal");}
void PreviewController::setFlipVertical(bool value){auto next=geometry_;next.flipVertical=value;setGeometry(next,"flipVertical");}
void PreviewController::setStraightenDegrees(double value){auto next=geometry_;next.straightenDegrees=value;setGeometry(next,"straightenDegrees");}
void PreviewController::resetGeometry(){setGeometry(output::Geometry{});}

void PreviewController::exportBatch(const QUrl& folder,int format,int quality,int maxLongEdge) {
    if(!ready_||busy_||!folder.isLocalFile()||!selectedCount())return;
    saveActiveSettings();
    std::vector<LibraryItem> items;
    for(const auto& item:library_)if(item.selected)items.push_back(item);
    // Strip thumbnails from the job snapshot. Only one developed frame is
    // retained by the worker, regardless of the number of imported photos.
    for(auto& item:items)item.thumbnail={};
    const auto held=frame_;const auto resources=resources_;const auto destination=path(folder);
    const auto gen=++generation_;cancel_batch_=std::make_shared<std::atomic_bool>(false);
    const auto cancel=cancel_batch_;batch_progress_=0;batch_total_=int(items.size());
    submit([this,items=std::move(items),held,resources,destination,gen,cancel,format,quality,maxLongEdge](auto& host){
        Reply r;r.generation=gen;r.operation="batch-export";
        int done=0,skipped=0,errors=0,processed=0;QStringList details;
        try {
            const auto options=exportOptions(format,quality,maxLongEdge);
            if(!std::filesystem::is_directory(destination))throw std::runtime_error("Export destination is not a folder");
            const QString extension=format==0?".tif":format==3?".jpg":".png";
            QSet<QString> names;
            for(const auto& item:items) {
                if(cancel->load())break;
                const auto source=path(item.url);
                const QString stem=QFileInfo(item.url.toLocalFile()).completeBaseName();
                QString name=stem+extension;int suffix=2;
                while(names.contains(name.toCaseFolded()))name=stem+"-"+QString::number(suffix++)+extension;
                names.insert(name.toCaseFolded());
                const auto target=destination/std::filesystem::path(name.toStdWString());
                try {
                    if(std::filesystem::exists(target))++skipped;
                    else {
                        const bool useShown=held&&held->source==source&&held->decode_mode==item.mode&&sameSettings(held->settings,item.settings);
                        const auto developed=useShown?held:host.open_raw(source,item.mode,item.settings);
                        // Each photo exports its own stored grade, not the
                        // active one: the snapshot was taken at submit time.
                        output::exportFrame(developed,target,item.geometry,options,resources,item.layer2);++done;
                    }
                }catch(const std::exception& e){++errors;details.append(name+": "+text(e.what()));}
                ++processed;
                QMetaObject::invokeMethod(this,[this,gen,processed]{if(generation_==gen){batch_progress_=processed;emit stateChanged();}},Qt::QueuedConnection);
            }
            r.message=QStringLiteral("%1：导出 %2 张，跳过已有 %3 张，失败 %4 张")
                .arg(cancel->load()?QStringLiteral("批量导出已取消"):QStringLiteral("批量导出完成")).arg(done).arg(skipped).arg(errors);
            if(errors)r.error=r.message+"\n"+details.join("\n");
        }catch(const std::exception& e){r.error=text(e.what());}
        return r;
    },QStringLiteral("正在批量导出…"),"batch-export");
}
