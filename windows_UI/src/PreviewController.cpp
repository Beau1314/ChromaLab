#include "PreviewController.hpp"
#include "ColourTransform.hpp"
#include "Histogram.hpp"
#include "PairPiece.hpp"
#include "core/json.hpp"
#include <QColorSpace>
#include <QJsonDocument>
#include <QMetaObject>
#include <QDir>
#include <QFileInfo>
#include <QSet>
#include <QStandardPaths>
#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace {
namespace desktop=spk::desktop;
namespace output=desktop::output;
namespace editing=desktop::editing;
namespace l2=desktop::layer2;
namespace colour=desktop::colour;
namespace histogram=desktop::histogram;
namespace pairc=desktop::pair;
namespace io=spk::io;
// The localization seam (task 6.1): user-facing status strings resolve
// through the core at set time; the literals are the English keys.
QString L(const QString& key) { return desktop::Localization::instance().L(key); }
QString L(const QString& key,std::initializer_list<QVariant> args) { return desktop::Localization::instance().L(key,QVariantList(args)); }
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
bool samePairState(const editing::PairState& a,const editing::PairState& b) {
    return a==b&&sameState(a.look,b.look);
}

// The pair's wire (PAIR-WIRING §3): the look with the pair's own rows
// forced. The meter is off and the exposure 0 -- each picture already
// carries its exposure in its pixels; the piece's long edge is the format;
// each hole's date prints only when that hole's switch is on (a switched-off
// hole is handed over with no text, as an empty hole is); overscan_pair
// rides a film edge -- or a date: the dated bare pair's rows ride the
// date's (DateBackSettings.pairRows -- without a film edge the engine is
// told here that the frame is a pair), and on a film edge the strip forces
// the engine's pairing rows and the camera's 1 mm. A bare pair (no film
// edge, no date) sends neither.
desktop::RenderSettings pairWireOf(const editing::PairState& pair) {
    auto s=pair.look.settings;
    const bool onStrip=s.overscan_active;
    const double spacing=onStrip?pairc::kStripAdvanceMM-pairc::kHoleAlongMM:pair.spacing_mm;
    s.auto_exposure=false;
    s.film_exposure_ev=0.0;
    s.film_format_mm=pairc::kHoleAlongMM*2.0+spacing;
    s.date_imprint_text=pair.holes[0].dated?pair.holes[0].date_text.toStdString():std::string();
    s.date_imprint_text_b=pair.holes[1].dated?pair.holes[1].date_text.toStdString():std::string();
    s.date_imprint_active=!s.date_imprint_text.empty()||!s.date_imprint_text_b.empty();
    // The pair rows ride the wire EXPLICITLY both ways. A reprint's delta
    // only carries changed keys, so a bare pair that omits them would leave
    // a previously-dated session's overscan_pair=true standing and render
    // the strip canvas on a piece the model calls bare (found by the first
    // real-RAW gate run: undoing the dated switch left the frame's wire
    // carrying pair=true while the model said dated=false). False is the
    // default on a clean open, so stating it costs nothing.
    s.overscan_pair=onStrip||s.date_imprint_active;
    // The format row only rides when the pair does (§3: a bare pair sends
    // neither); the boolean itself rides both ways so a reprint's delta
    // clears a previously-dated session.
    if(s.overscan_pair)s.overscan_format="135_half";
    return s;
}

// --- The pair's composition (PAIR-WIRING §2's order, inverted for the one
// window+turn form HoleInput composes with) -------------------------------

// The rect in the pre-turn space whose clockwise k-turn image is `r` (the
// inverse of a quarter turn of a normalized rectangle; rotations of the
// square commute, so placement and frame turns invert one after the other).
QRectF invertedTurnRect(QRectF r,int k) {
    k=((k%4)+4)%4;
    switch(k) {
        case 1: return QRectF(r.y(),1.0-(r.x()+r.width()),r.height(),r.width());
        case 2: return QRectF(1.0-(r.x()+r.width()),1.0-(r.y()+r.height()),r.width(),r.height());
        case 3: return QRectF(1.0-(r.y()+r.height()),r.x(),r.height(),r.width());
        default: return r;
    }
}
// A hole's placement window in the decoded picture's normalized space (y
// down, applied before the turn). The placement lives in the as-seen space
// (post frame turn, post placement turn): the largest hole-aspect rectangle
// of the as-framed picture, zoomed about its centre and translated. The
// window rides back through the placement turn, the frame's own turn and
// the frame's own crop; the compose turn is the sum of both turns.
QRectF pairPlacementWindow(const QRectF& crop,int frameTurns,
                           const editing::PairPlacement& placement,bool turned) {
    const double aspect=turned?pairc::kHoleAcrossMM/pairc::kHoleAlongMM
                              :pairc::kHoleAlongMM/pairc::kHoleAcrossMM;
    QRectF p(0,0,1,1);
    if(aspect<1.0)p.setWidth(aspect);else p.setHeight(1.0/aspect);
    p=QRectF((1.0-p.width())/2.0,(1.0-p.height())/2.0,p.width(),p.height());
    const double w=p.width()*placement.scale,h=p.height()*placement.scale;
    p=QRectF(p.x()+(p.width()-w)/2.0,p.y()+(p.height()-h)/2.0,w,h);
    p.translate(placement.x,placement.y);
    const QRectF rel=invertedTurnRect(invertedTurnRect(p,placement.quarterTurns),frameTurns);
    return QRectF(crop.x()+rel.x()*crop.width(),crop.y()+rel.y()*crop.height(),
                  rel.width()*crop.width(),rel.height()*crop.height());
}

// The meter's wire for one hole (PAIR-WIRING §4.1): the pair's own stocks,
// the meter on, exposure 0, a plain 24 mm frame, film edge and date off.
std::string meterWire(const desktop::RenderSettings& look,bool scanFilm) {
    spk::Json params=spk::Json::object();
    params.set("input_color_space",spk::Json(std::string("ProPhoto RGB")));
    params.set("input_cctf_decoding",spk::Json(false));
    params.set("film_stock",spk::Json(look.film_stock));
    params.set("print_stock",spk::Json(look.print_stock));
    params.set("scan_film",spk::Json(scanFilm));
    params.set("auto_exposure",spk::Json(true));
    params.set("auto_exposure_method",spk::Json(std::string("center_weighted")));
    params.set("exposure_compensation_ev",spk::Json(0.0));
    params.set("film_format_mm",spk::Json(24.0));
    params.set("grain_active",spk::Json(false));
    params.set("halation_active",spk::Json(false));
    params.set("glare_active",spk::Json(false));
    params.set("overscan_active",spk::Json(false));
    params.set("date_imprint_active",spk::Json(false));
    return params.dump();
}

// One hole's snapshot of its library frame: the decode ref (PAIR-WIRING §5
// -- the shot stays the frame's; the pair reads it again at every open).
struct PairSourceSnapshot {
    QUrl url;
    desktop::RenderSettings settings;
    output::Geometry geometry;
    desktop::DecodeMode mode=desktop::DecodeMode::compatible16;
};
using PairSources=std::array<PairSourceSnapshot,2>;

// One hole's meter reading, solved on a throwaway session over the hole's
// own placed picture alone (no gain -- the gain is what the solve answers).
double meterHole(desktop::PreviewHost& host,const io::DecodedRaw& decoded,
                 const QRectF& window,int turns,const pairc::PieceSpec& spec,
                 const std::string& wire) {
    QSizeF placed(window.width()*decoded.width,window.height()*decoded.height);
    if(turns%2)placed.transpose();
    // The meter is a <=1024 px session; laying the meter buffer out at the
    // capped size directly keeps the compose cheap and the reading within a
    // fraction of a stop of the full-size content (solveExposure's own cap
    // would box-downscale it anyway). The frame is a SINGLE hole -- this
    // hole's picture alone, no gap, no second hole (PAIR-WIRING §4.1): the
    // exposure solver reads every pixel it is handed (the engine's zero-light
    // exclusion is the scene latitude's, not the solver's), so a pair piece's
    // unexposed film would skew the EV off the 1.x single-hole basis.
    const double cap=std::min(1.0,1024.0/std::max(1.0,std::max(placed.width(),placed.height())));
    const auto layout=pairc::holeLayout(placed*cap,spec);
    auto buffer=pairc::compose(layout,{&decoded,window,turns,0.0,true},pairc::HoleInput{});
    const spk_image input{buffer.data(),std::uint32_t(layout.pieceSize.width()),
                          std::uint32_t(layout.pieceSize.height()),3};
    return host.solveExposure(input,wire);
}

// The worker's heart: decode both present holes from their library frames,
// meter any hole whose placement-excluding key is not cached, compose the
// piece and hand it to the engine as one frame. Throws on failure (the job
// reports the error; the previous frame stays).
desktop::FramePtr pairOpenFrame(desktop::PreviewHost& host,const PairSources& sources,
                                const editing::PairState& pair,
                                const QHash<QString,double>& meterCache,
                                bool filmIsPositive,
                                std::vector<std::pair<QString,double>>* meterUpdates) {
    const bool onStrip=pair.look.settings.overscan_active;
    const double spacing=onStrip?pairc::kStripAdvanceMM-pairc::kHoleAlongMM:pair.spacing_mm;
    const pairc::PieceSpec spec{spacing,pair.turned};
    const auto wire=pairWireOf(pair);
    const auto meter=meterWire(pair.look.settings,filmIsPositive);
    desktop::FramePtr frame;
    io::DecodedRaw decoded[2];
    pairc::HoleInput holes[2];
    QSizeF asFramed[2];
    const int totalTurns[2]={(sources[0].geometry.quarterTurns+pair.holes[0].placement.quarterTurns)%4,
                             (sources[1].geometry.quarterTurns+pair.holes[1].placement.quarterTurns)%4};
    for(int side=0;side<2;++side) {
        holes[side].present=false;
        const auto& src=sources[side];
        if(!src.url.isLocalFile())continue; // a missing frame: its hole is Missing (bare film)
        const auto file=path(src.url);
        if(!std::filesystem::is_regular_file(file))continue;
        std::string error;
        const bool ok=src.mode==desktop::DecodeMode::headroom
            ?io::decode_raw_headroom(file,decoded[side],error)
            :src.settings.wb_temperature_k!=0.0
                ?io::decode_raw_compatible(file,{src.settings.wb_temperature_k,src.settings.wb_tint},decoded[side],error)
                :io::decode_raw_compatible(file,decoded[side],error);
        if(!ok)throw std::runtime_error("Pair hole decode: "+error);
        const auto& g=src.geometry;
        const QRectF window=pairPlacementWindow(g.crop,g.quarterTurns,pair.holes[side].placement,pair.turned);
        const int turns=totalTurns[side];
        const QString key=pairc::meterKey(src.url.toLocalFile(),
                                          src.settings.wb_temperature_k==0.0
                                              ?QStringLiteral("as_shot"):QStringLiteral("override"),
                                          src.settings.wb_temperature_k,src.settings.wb_tint,false,
                                          QStringLiteral("center_weighted"),g.crop,g.quarterTurns);
        double metered=0.0;
        const auto cached=meterCache.constFind(key);
        if(cached!=meterCache.cend())metered=cached.value();
        else {
            metered=meterHole(host,decoded[side],window,turns,spec,meter);
            if(meterUpdates) {
                meterUpdates->emplace_back(key,metered);
                while(meterUpdates->size()>4)meterUpdates->erase(meterUpdates->begin());
            }
        }
        holes[side]={&decoded[side],window,turns,pairc::stops(metered,pair.holes[side].exposure_ev),true};
        // As-framed size (the frame's own crop and turns, pre-transposed by
        // the placement turn -- layout fits what the hole will see).
        QSizeF size(g.crop.width()*decoded[side].width,g.crop.height()*decoded[side].height);
        if(turns%2)size.transpose();
        asFramed[side]=size;
    }
    const auto layout=pairc::layout(asFramed[0],asFramed[1],spec);
    auto piece=pairc::compose(layout,holes[0],holes[1]);
    const int metadataSide=holes[0].present?0:1;
    frame=host.open_pair(piece.data(),std::uint32_t(layout.pieceSize.width()),
                         std::uint32_t(layout.pieceSize.height()),
                         path(sources[metadataSide].url),decoded[metadataSide].metadata,wire);
    return frame;
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
// The export dialog's four destinations, in its order: the engine's own
// names (`spk_output_transform`'s dst_cs; the Mac's ExportRecipe.Colour).
// The session's working space is the source, read from the frame (the open
// reply's resolved io), never assumed.
constexpr const char* kExportColourNames[] = {"sRGB", "Display P3", "Adobe RGB (1998)", "ProPhoto RGB"};

output::ExportOptions exportOptions(int format,int quality,int maxEdge,int colourSpace=0) {
    if(format<0||format>3||quality<1||quality>100||maxEdge<0||maxEdge>100000||
       colourSpace<0||colourSpace>3)
        throw std::runtime_error("Invalid export options");
    return {static_cast<output::Format>(format),quality,maxEdge,
            static_cast<output::ColourSpace>(colourSpace)};
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
    full_layer2_timer_.setSingleShot(true);full_layer2_timer_.setInterval(450);
    connect(&full_layer2_timer_,&QTimer::timeout,this,[this]{
        if(!frame_||busy_||dialog_open_)return;
        const auto gen=generation_;const auto revision=layer2_revision_;
        submit([gen,revision](desktop::PreviewHost&){Reply r;r.generation=gen;
            r.settings_revision=revision;r.operation="layer2";r.full=true;return r;},
            L("Updating the preview"),"layer2");
    });
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
        status_=dirty()?L("Updating the preview"):L("The preview already matches the current selection");
    } else if(failed_) {
        status_=reply.error;
        if(frame_&&reply.operation!="export"&&reply.operation!="batch-export")restoreSettings();
    } else if(reply.operation=="init") {
        ready_=true;catalog_=std::move(reply.catalog);films_=list(catalog_.films);papers_=list(catalog_.papers);
        restoreSettings();emit catalogChanged();status_=L("Ready · open a RAW to begin");
    } else if(reply.operation=="layer2") {
        // The fast path's job only re-sequences the publication behind the
        // worker queue; the pixels come from the retained frame below.
        publishFromFrame(L("Preview updated"),reply.full);
    } else if(reply.operation=="latitude") {
        // The analysis never renders, so nothing on screen moves: only the
        // parsed reply is published. A malformed reply is a failure like any
        // other (the engine guarantees "medium"/"scene"; the parse guards
        // everything else).
        const auto document=QJsonDocument::fromJson(reply.latitude_json.toUtf8());
        if(!reply.latitude_json.isEmpty()&&document.isObject()) {
            latitude_=document.object().toVariantMap();
            emit latitudeChanged();
            status_=L("Latitude analysis finished");
        } else {failed_=true;status_=L("The latitude analysis returned an invalid result");}
    } else if(reply.frame) {
        // Allocate the presentation wrapper before publishing any new UI state.
        try {
            // The opened photo's grade only commits with its successful
            // publication; a failure leaves the previous photo intact.
            auto nextLayer2=reply.operation=="open"?std::move(reply.layer2):layer2_;
            // The compare's plain-decode half rode the open reply: store it
            // before the publication, which cuts it to the geometry.
            if(!reply.before_plain.isNull())before_plain_=std::move(reply.before_plain);
            auto next=gradedDisplayImage(reply.frame,nextLayer2);
            if(next.isNull())throw std::runtime_error("Cannot create the display image");
            const auto geometry=reply.operation=="open"?reply.geometry:geometry_;
            auto visible=visibleImage(reply.frame,next,geometry,nextLayer2);
            layer2_=std::move(nextLayer2);
            frame_=std::move(reply.frame);full_image_=std::move(next);image_=std::move(visible);
            geometry_=geometry;
            refreshBeforeImage();
            computeHistogram();
            // A new print is a new session state: the analysis described the
            // previous one, so the card shows nothing until it is re-run.
            if(!latitude_.isEmpty()){latitude_.clear();emit latitudeChanged();}
            if(reply.operation=="open")active_index_=reply.library_index;
            // The pair item remembers its last published frame (the reprint
            // path recognises a live pair session by pointer identity) and
            // takes on the meter readings solved on the worker for keys the
            // cache did not hold.
            if(reply.library_index>=0&&reply.library_index<int(library_.size())&&
                library_[reply.library_index].is_pair) {
                library_[reply.library_index].pair_frame=frame_;
                if(!reply.meter_updates.empty()) {
                    auto& cache=library_[reply.library_index].meter_cache;
                    auto& order=library_[reply.library_index].meter_order;
                    for(const auto& update:reply.meter_updates) {
                        if(!cache.contains(update.first))order.append(update.first);
                        cache.insert(update.first,update.second);
                    }
                    while(order.size()>8)cache.remove(order.takeFirst());
                    emit pairChanged();
                }
            }
        }
        catch(const std::exception& e){failed_=true;status_=text(e.what());if(frame_)restoreSettings();}
        if(!failed_) {
            restoreSettings();
            updateThumbnail();
            status_=L("Render finished in %@ s%@",{QString::number(frame_->timings.render_ms/1000,'f',2),
                frame_->reprint()?L(" · negative reused"):QString()});
            emit frameChanged();emit geometryChanged();if(reply.operation=="open")emit sourceOpened();
        }
    } else if(reply.operation=="export") status_=L("Exported the current frame");
    // A failed batch must name what failed: the summary line alone ("…:
    // exported 2, skipped 0, failed 1") never says which file, and the
    // per-file reasons ride in the error's details. The workflow gate pins
    // that the status carries the rejected file's name beside the failure
    // word — the gate could never run before a real RAW fixture arrived.
    else if(reply.operation=="batch-export")status_=(failed_&&!reply.error.isEmpty())?reply.error:reply.message;
    if(failed_&&reply.operation=="open"&&reply.library_index>=0&&reply.library_index<int(library_.size()))
        library_[reply.library_index].error=status_;
    // A failed pair job (open or wire) loses the host's live pair session:
    // open_pair keeps no decode, so preview_host drops impl_->session while
    // impl_->frame -- the pointer the wire job compares against -- still
    // matches. The next wire edit would then take host.render()'s
    // rebuild-from-decode path on the emptied decode and fail with an
    // unrelated "Open image" error. Drop the item's held frame so that edit
    // misses the pointer check and takes the full compose+open fallback the
    // job already carries (photos self-heal in the host itself: their decode
    // is retained, so the same failure rebuilds from it).
    if(failed_&&reply.library_index>=0&&reply.library_index<int(library_.size())&&
        library_[reply.library_index].is_pair)
        library_[reply.library_index].pair_frame={};
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
    if(frame_)status_=L("Updating the preview");
    failed_=false;emit settingsChanged();emit stateChanged();
}
void PreviewController::setFilmIndex(int v){if(!settingsEditable()||v<0||v>=films_.size()||v==film_index_)return;film_index_=v;changed("filmIndex");}
void PreviewController::setPaperIndex(int v){if(!settingsEditable()||filmIsPositive()||v<0||v>=papers_.size()||v==paper_index_)return;paper_index_=v;changed("paperIndex");}
void PreviewController::setDecodeMode(int v){
    if(!settingsEditable()||v<0||v>1||v==decode_mode_)return;
    decode_mode_=v;
    // The headroom decode keeps the file's own as-shot multipliers, so a WB
    // override has nothing to attach to there (the engine host refuses the
    // pair outright). The switch therefore clears the override — parking it
    // would leave a slider showing a number the decode ignores and would
    // make reopen refuse the photo outright (restore of wb+headroom was the
    // first real-RAW gate run's failure: the persisted record held
    // headroom+5500K and the engine's refusal turned into an unopenable
    // photo). One undo step covers both rows because changed() snapshots the
    // whole state after the reset.
    if(v==1&&(extra_settings_.wb_temperature_k!=0.0||extra_settings_.wb_tint!=0.0)){
        extra_settings_.wb_temperature_k=0.0;extra_settings_.wb_tint=0.0;
    }
    changed("decodeMode");
}
void PreviewController::setExposureEv(double v){if(!settingsEditable()||filmIsPositive()||!std::isfinite(v)||v< -2||v>2||v==exposure_ev_)return;exposure_ev_=v;changed("exposureEv");}
void PreviewController::resetSettings(){if(!settingsEditable())return;loadSettings({},desktop::DecodeMode::compatible16);changed();}
void PreviewController::openRaw(const QUrl& url) {
    // Changing sources and exporting remain single flight. Parameter edits
    // can be coalesced during a render, but cannot retarget an in-flight open.
    if(!ready_||busy_||!url.isLocalFile())return;
    saveActiveSettings();
    const int index=addFile(url);
    if(index<0)return;
    // A known photo restores its own saved settings. A genuinely new photo
    // still starts with the current preset; opening never overwrites a sidecar.
    openItem(index);
}
// The compare's before half: the plain decode (linear ProPhoto) through the
// canonical output kernel — identity decode (the samples are linear), the
// CAT02 matrix, the CAM16 roll-off and the sRGB encode — as an ARGB32
// image the canvas composite can cut with the photo's geometry. Returns a
// null image when the decode is empty; failures leave the compare after-only.
QImage plainBeforeImage(desktop::PreviewHost& host) {
    std::uint32_t w = 0, h = 0;
    const auto lin = host.plain_decode_linear16(0, &w, &h);
    QImage linear(int(w), int(h), QImage::Format_RGBA64);
    if (linear.isNull()) return {};
    for (std::uint32_t y = 0; y < h; ++y) {
        auto* row = reinterpret_cast<std::uint16_t*>(linear.scanLine(int(y)));
        const auto* src = lin.data() + size_t(y) * w * 3;
        for (std::uint32_t x = 0; x < w; ++x) {
            row[x * 4 + 0] = src[x * 3 + 0];
            row[x * 4 + 1] = src[x * 3 + 1];
            row[x * 4 + 2] = src[x * 3 + 2];
            row[x * 4 + 3] = 65535;
        }
    }
    auto setup = host.outputTransform("ProPhoto RGB", "sRGB");
    setup.source_cctf_mode = 4;  // the samples are linear: decode is identity
    return colour::apply_rgba64(linear, setup).convertToFormat(QImage::Format_ARGB32);
}

void PreviewController::openItem(int index) {
    endEditGesture();
    auto& mutableItem=library_.at(index);
    if(mutableItem.is_pair) {
        // The pair's record restores on open, but never over unsaved
        // in-memory edits (the debounced save may not have flushed yet).
        const auto stored=edit_store_.loadPair(mutableItem.pair_sources[0],mutableItem.pair_sources[1],catalog_);
        mutableItem.save_blocked=stored.blocked;mutableItem.save_warning=stored.warning;
        if(stored.state&&!mutableItem.pending_save&&!samePairState(mutableItem.pair,*stored.state))
            mutableItem.pair=*stored.state;
        submitPairOpen(index);
        return;
    }
    const auto item=library_.at(index);const auto p=path(item.url);
    const auto gen=++generation_;
    submit([p,item,gen,index](auto& host){Reply r;r.generation=gen;r.operation="open";r.library_index=index;r.geometry=item.geometry;r.layer2=item.layer2;
        try{
            r.frame=host.open_raw(p,item.mode,item.settings);
            // The compare's plain-decode half rides the open, at the frame's
            // OWN resolution: the canvas composite requires the before to
            // match the after's dimensions exactly or it silently shows the
            // after on both sides of the split.
            try{r.before_plain=plainBeforeImage(host);}catch(const std::exception&){}
        }catch(const std::exception& e){r.error=text(e.what());}return r;},L("Decoding and rendering the RAW…"),"open");
}
void PreviewController::apply() {
    if(!frame_||busy_||dialog_open_)return;
    // A pair's look edits land on the wire of its piece: the reprint path of
    // the live pair session (never an engine render of a RAW session).
    if(activePairItem()){submitPairWire(active_index_);return;}
    const auto s=settings();const auto shown=frame_;const auto gen=++generation_;const auto revision=settings_revision_;
    const auto mode=decode_mode_?desktop::DecodeMode::headroom:desktop::DecodeMode::compatible16;
    // White balance is a decode-side parameter: when it moved, the update is
    // a transactional re-open — decode with the new multipliers plus render,
    // one job, the current library index, geometry and grade retained (the
    // "render" reply keeps all of them). While a job is in flight the edit
    // stays pending: the debounced apply() returns without submitting, and
    // the reply's superseded branch reschedules — the same hold-and-reschedule
    // shape the layer2 fast path keeps in layer2_pending_.
    const bool wbChanged=s.wb_temperature_k!=shown->settings.wb_temperature_k||
                          s.wb_tint!=shown->settings.wb_tint;
    submit([s,shown,gen,revision,mode,wbChanged](auto& host){Reply r;r.generation=gen;r.settings_revision=revision;r.operation="render";
        try {
            // Publication can fail after a successful worker open (for example,
            // a display allocation failure). Reconcile against the image the
            // user is actually editing before reusing the worker's session.
            const auto active=host.current();
            const bool via_open=!(active&&active->source==shown->source&&active->decode_mode==mode&&!wbChanged);
            if(via_open) {
                r.frame=host.open_raw(shown->source,mode,s);
                // A re-decode (white balance moved) changes the plain decode
                // too — refresh the compare's before half or it keeps the
                // old balance.
                try{r.before_plain=plainBeforeImage(host);}catch(const std::exception&){}
            } else r.frame=host.render(s);
        }catch(const std::exception& e){r.error=text(e.what());}return r;},
        wbChanged?L("Re-decoding with the white balance…"):L("Updating the film effects…"),"render");
}
void PreviewController::exportTiff(const QUrl& url) {
    exportImage(url,0,95,0);
}
void PreviewController::exportImage(const QUrl& url,int format,int quality,int maxLongEdge,int colourSpace) {
    if(!frame_||busy_||!url.isLocalFile())return;
    // PAIR-WIRING §6.4: a pair whose source has disappeared is refused, not
    // exported from the shown piece alone -- the piece would be the only
    // place the missing frame still exists. The batch path refuses pairs
    // outright; this is the single-export face of the same refusal.
    if(const auto* item=activePairItem()) {
        if(!QFileInfo(item->pair_sources[0].toLocalFile()).isFile()||
            !QFileInfo(item->pair_sources[1].toLocalFile()).isFile()) {
            failed_=true;status_=L("A source photo is missing; the half-frame pair cannot be exported");
            emit stateChanged();return;
        }
    }
    const auto p=path(url);const auto held=frame_;const auto gen=++generation_;const auto geometry=geometry_;const auto resources=resources_;
    const auto layer2=layer2_; // the grade as the user saw it at submit time
    submit([p,held,gen,geometry,resources,layer2,format,quality,maxLongEdge,colourSpace](auto& host){Reply r;r.generation=gen;r.operation="export";
        try{
            auto options=exportOptions(format,quality,maxLongEdge,colourSpace);
            // The conversion out of the working space, fetched once for this
            // destination (a fetch, not a render; the host holds the engine,
            // so no open session is needed). The session's own space needs
            // no fetch: its numbers would be an identity, and skipping the
            // pass keeps the default export the picture itself.
            desktop::OutputTransformData transform;
            const std::string working=held->output_color_space;
            const std::string target=kExportColourNames[std::size_t(colourSpace)];
            if(target!=working) {
                transform=host.outputTransform(working,target);
                options.transform=&transform;
            }
            output::exportFrame(held,p,geometry,options,resources,layer2);
        }catch(const std::exception& e){r.error=text(e.what());}return r;},L("Exporting the current frame…"),"export");
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

// --- White balance: the RAW decode's Temperature/Tint -------------------------

bool PreviewController::wbAsShotAvailable() const {
    // The decoder reports the as-shot illuminant only when the file carries
    // a three-colour camera matrix; the tint may legitimately be zero, so
    // availability rides on the kelvin alone.
    return bool(frame_)&&frame_->metadata.as_shot_temperature_k>0;
}
void PreviewController::setWbEdit(const double kelvin,const double tint,const QString& editKey) {
    // The headroom decode keeps the file's own as-shot multipliers, so a WB
    // override has nothing to attach to there (matching the engine host's
    // refusal). The sliders are disabled in that mode too.
    if(!settingsEditable()||decode_mode_==1)return;
    auto next=extra_settings_;
    next.wb_temperature_k=kelvin;next.wb_tint=tint;
    if(next.wb_temperature_k==extra_settings_.wb_temperature_k&&
        next.wb_tint==extra_settings_.wb_tint)return; // 复位不记录撤销步（无变化）
    extra_settings_=next;changed(editKey);
}
void PreviewController::setWb(const double kelvin,const double tint) {
    // Decode protocol ranges (the engine host's own): 0 kelvin is reserved
    // for the As Shot reset, never a partial state, so it is refused here.
    if(!std::isfinite(kelvin)||!std::isfinite(tint)||kelvin<2000||kelvin>50000||
        tint<-150||tint>150)return;
    setWbEdit(kelvin,tint,QStringLiteral("wb"));
}
void PreviewController::applyWbPreset(const QString& name) {
    if(!settingsEditable())return;
    double kelvin=0,tint=0;
    // The camera's named illuminants (Adobe/Lightroom's own numbers).
    if(name==QLatin1String("daylight"))kelvin=5500;
    else if(name==QLatin1String("cloudy"))kelvin=6500;
    else if(name==QLatin1String("shade"))kelvin=8000;
    else if(name==QLatin1String("tungsten"))kelvin=3200;
    else if(name==QLatin1String("fluorescent")){kelvin=4000;tint=20;}
    else if(name==QLatin1String("flash")){kelvin=5500;tint=10;}
    else return;
    setWbEdit(kelvin,tint,QStringLiteral("wbPreset:")+name);
}
void PreviewController::applyWbAsShot() {
    if(!settingsEditable()||!frame_)return;
    // Backfill the decoder-reported as-shot numbers as the edited value:
    // decoding with them reproduces the as-shot multipliers (white_balance.hpp's
    // no-op property), so this makes the implicit numbers explicit and
    // editable without moving the picture.
    const double kelvin=frame_->metadata.as_shot_temperature_k;
    const double tint=frame_->metadata.as_shot_tint;
    if(!(kelvin>0))return;
    setWbEdit(kelvin,tint,QStringLiteral("wbAsShot"));
}
void PreviewController::resetWb() {
    if(!settingsEditable())return;
    setWbEdit(0,0,QStringLiteral("wbReset"));
}
void PreviewController::setAutoExposure(const bool value) {
    if(!settingsEditable()||value==extra_settings_.auto_exposure)return;
    extra_settings_.auto_exposure=value;changed(QStringLiteral("autoExposure"));
}

// --- Digital Intermediate: the Cineon engine parameter -----------------------

QVariantMap PreviewController::di() const {
    const auto& s=extra_settings_;
    return {{"active",s.di_active},{"blueCompensation",s.di_blue_compensation}};
}
void PreviewController::setDi(const QString& name,const bool value) {
    if(!settingsEditable()||filmIsPositive())return;
    auto next=extra_settings_;
    if(name==QLatin1String("active"))next.di_active=value;
    else if(name==QLatin1String("blueCompensation"))next.di_blue_compensation=value;
    else return;
    if(next==extra_settings_)return;
    extra_settings_=next;changed(QStringLiteral("di:")+name);
}

// --- Contrast mask: RFC-024's print-side engine parameters -------------------

QVariantMap PreviewController::mask() const {
    const auto& s=extra_settings_;
    return {{"active",s.contrast_mask_active},{"highlights",s.contrast_mask_highlights},
        {"shadows",s.contrast_mask_shadows},{"core",s.contrast_mask_core},
        {"scale",s.contrast_mask_scale}};
}
void PreviewController::setMask(const QString& name,const QVariant& value) {
    // Slide film never reaches the print (RFC-024 §7.5), so the group is dead
    // there like the DI's; the positive-paper and LUT cases cannot arise from
    // the desktop's own settings.
    if(!settingsEditable()||filmIsPositive())return;
    auto next=extra_settings_;
    if(name==QLatin1String("active"))next.contrast_mask_active=value.toBool();
    else {
        bool ok=false;const double v=value.toDouble(&ok);if(!ok||!std::isfinite(v))return;
        // The engine schema's own rows (params.cpp): the amounts and the core
        // are stops in [0, 3], the scale is the sigma as a long-edge fraction.
        if(name==QLatin1String("highlights")){if(v<0||v>3)return;next.contrast_mask_highlights=v;}
        else if(name==QLatin1String("shadows")){if(v<0||v>3)return;next.contrast_mask_shadows=v;}
        else if(name==QLatin1String("core")){if(v<0||v>3)return;next.contrast_mask_core=v;}
        else if(name==QLatin1String("scale")){if(v<0.002||v>0.12)return;next.contrast_mask_scale=v;}
        else return;
    }
    if(next==extra_settings_)return;
    extra_settings_=next;changed(QStringLiteral("mask:")+name);
}

// --- The slide's HDR scan: the positive film's engine parameters -------------

QVariantMap PreviewController::scanHdr() const {
    const auto& s=extra_settings_;
    return {{"on",s.scan_hdr_stretch||s.scan_hdr_headroom>1.0},
        {"headroom",s.scan_hdr_headroom},{"stretch",s.scan_hdr_stretch}};
}
void PreviewController::setScanHdr(const QString& name,const QVariant& value) {
    // Only a positive film scanned has the HDR scan (the engine's own rule,
    // core/scan_hdr.hpp): the mirror image of the DI and the mask gates.
    if(!settingsEditable()||!filmIsPositive())return;
    auto next=extra_settings_;
    if(name==QLatin1String("on")) {
        // On backfills the study's working headroom (scan_hdr.hpp's measured
        // example: blue 8 % short at H = 4) unless a real one is already set;
        // off returns to 0, which is today's scan byte for byte.
        next.scan_hdr_headroom=value.toBool()
            ?(next.scan_hdr_headroom>1.0?next.scan_hdr_headroom:4.0):0.0;
    } else if(name==QLatin1String("headroom")) {
        bool ok=false;const double v=value.toDouble(&ok);
        if(!ok||!std::isfinite(v))return;
        // The engine schema's own row (params.cpp): the headroom is in [0, 16].
        if(v<0||v>16)return;
        next.scan_hdr_headroom=v;
    } else return;
    if(next==extra_settings_)return;
    extra_settings_=next;changed(QStringLiteral("scanHdr:")+name);
}

// --- Film edge: RFC-031/032's overscan, the phase 2 port ---------------------

QVariantMap PreviewController::overscan() const {
    const auto& s=extra_settings_;
    return {{"on",s.overscan_active},{"mode",text(s.overscan_mode)},{"format",text(s.overscan_format)},
        {"gate",text(s.overscan_gate)},{"holes",text(s.overscan_holes)},{"carrier",text(s.overscan_carrier)},
        {"edgeText",text(s.overscan_edge_text)},{"frameNumber",s.overscan_frame_number},
        {"frameSeed",s.overscan_frame_seed},{"cameraSeed",s.overscan_camera_seed},
        {"fNumber",s.overscan_f_number},{"fog",s.overscan_fog},{"leaks",s.overscan_leaks},
        {"pair",s.overscan_pair}};
}
void PreviewController::setOverscan(const QString& name,const QVariant& value) {
    if(!settingsEditable())return;
    auto next=extra_settings_;
    // The engine's own enumerations (params.cpp) and schema ranges.
    static const char* kModes[]={"strip","filed"};
    static const char* kFormats[]={"135","135_half","120_645","120_6x6","120_6x7","120_6x8",
                                    "120_6x9","135_xpan","120_6x12","120_6x17"};
    static const char* kGates[]={"auto","square","rounded","eared","shouldered","kicked"};
    static const char* kHoles[]={"white","black"};
    static const char* kCarriers[]={"none","black","open"};
    const auto choice=[&](const QVariant& v,const char** choices,int n){
        const QString s=v.toString();
        for(int i=0;i<n;++i)if(s==QLatin1String(choices[i]))return std::string(choices[i]);
        return std::string();
    };
    if(name==QLatin1String("on"))next.overscan_active=value.toBool();
    else if(name==QLatin1String("mode")){auto v=choice(value,kModes,2);if(v.empty())return;next.overscan_mode=v;}
    else if(name==QLatin1String("format")){auto v=choice(value,kFormats,10);if(v.empty())return;next.overscan_format=v;}
    else if(name==QLatin1String("gate")){auto v=choice(value,kGates,6);if(v.empty())return;next.overscan_gate=v;}
    else if(name==QLatin1String("holes")){auto v=choice(value,kHoles,2);if(v.empty())return;next.overscan_holes=v;}
    else if(name==QLatin1String("carrier")){auto v=choice(value,kCarriers,3);if(v.empty())return;next.overscan_carrier=v;}
    else if(name==QLatin1String("edgeText"))next.overscan_edge_text=value.toString().toStdString();
    else if(name==QLatin1String("frameNumber")||name==QLatin1String("frameSeed")||name==QLatin1String("cameraSeed")) {
        bool ok=false;const int v=value.toInt(&ok);
        if(!ok)return;
        if(name==QLatin1String("frameNumber")){if(v<0||v>99)return;next.overscan_frame_number=v;}
        else if(name==QLatin1String("frameSeed")){if(v<0)return;next.overscan_frame_seed=v;}
        else {if(v<0)return;next.overscan_camera_seed=v;}
    } else if(name==QLatin1String("fNumber")||name==QLatin1String("fog")||name==QLatin1String("leaks")) {
        bool ok=false;const double v=value.toDouble(&ok);
        if(!ok||!std::isfinite(v))return;
        if(name==QLatin1String("fNumber")){if(v<0||v>64)return;next.overscan_f_number=v;}
        else if(name==QLatin1String("fog")){if(v<0||v>4)return;next.overscan_fog=v;}
        else {if(v<0||v>4)return;next.overscan_leaks=v;}
    } else if(name==QLatin1String("pair"))next.overscan_pair=value.toBool();
    else return;
    if(next==extra_settings_)return;
    extra_settings_=next;changed(QStringLiteral("overscan:")+name);
}

// --- Date back: RFC-031, the phase 2 port ------------------------------------

QVariantMap PreviewController::dateImprint() const {
    const auto& s=extra_settings_;
    return {{"on",s.date_imprint_active},{"text",text(s.date_imprint_text)},
        {"textB",text(s.date_imprint_text_b)},{"dataText",text(s.date_imprint_data_text)},
        {"placement",text(s.date_imprint_placement)},{"ev",s.date_imprint_exposure_ev},
        {"style",text(s.date_imprint_style)},{"size",s.date_imprint_size},
        {"corner",text(s.date_imprint_corner)},{"insetX",s.date_imprint_inset_x},
        {"insetY",s.date_imprint_inset_y}};
}
void PreviewController::setDateImprint(const QString& name,const QVariant& value) {
    if(!settingsEditable())return;
    auto next=extra_settings_;
    if(name==QLatin1String("on"))next.date_imprint_active=value.toBool();
    else if(name==QLatin1String("text"))next.date_imprint_text=value.toString().toStdString();
    else if(name==QLatin1String("textB"))next.date_imprint_text_b=value.toString().toStdString();
    else if(name==QLatin1String("dataText"))next.date_imprint_data_text=value.toString().toStdString();
    else if(name==QLatin1String("placement")) {
        const QString v=value.toString();
        if(v!=QLatin1String("frame")&&v!=QLatin1String("rebate"))return;
        next.date_imprint_placement=v.toStdString();
    } else if(name==QLatin1String("style")) {
        const QString v=value.toString();
        if(v!=QLatin1String("lcd")&&v!=QLatin1String("dots")&&v!=QLatin1String("data"))return;
        next.date_imprint_style=v.toStdString();
    } else if(name==QLatin1String("corner")) {
        const QString v=value.toString();
        if(v!=QLatin1String("br")&&v!=QLatin1String("bl")&&v!=QLatin1String("tr")&&v!=QLatin1String("tl"))return;
        next.date_imprint_corner=v.toStdString();
    } else if(name==QLatin1String("ev")||name==QLatin1String("size")||name==QLatin1String("insetX")||name==QLatin1String("insetY")) {
        bool ok=false;const double v=value.toDouble(&ok);
        if(!ok||!std::isfinite(v))return;
        // The engine schema's own rows (params.cpp).
        if(name==QLatin1String("ev")){if(v<-2||v>8)return;next.date_imprint_exposure_ev=v;}
        else if(name==QLatin1String("size")){if(v<0.4||v>3.0)return;next.date_imprint_size=v;}
        else if(name==QLatin1String("insetX")){if(v<0||v>30)return;next.date_imprint_inset_x=v;}
        else {if(v<0||v>30)return;next.date_imprint_inset_y=v;}
    } else return;
    if(next==extra_settings_)return;
    extra_settings_=next;changed(QStringLiteral("dateImprint:")+name);
}

// --- Scene Latitude Mapping: the analysis card's engine side -----------------

void PreviewController::setSceneLatitude(const bool value) {
    if(!settingsEditable()||value==extra_settings_.scene_latitude_active)return;
    extra_settings_.scene_latitude_active=value;changed(QStringLiteral("sceneLatitude"));
}
void PreviewController::commitLatitudeFit() {
    // The fit's params_delta is the resolved curve as engine parameters; the
    // host's changed-params channel sends exactly the keys that differ, so
    // writing them into the settings and re-rendering IS the ordinary
    // spk_set_params path. No valid fit, no commit (the reply carries
    // params_delta only when the fit passed). The fit lands on the first
    // curve; a pair's fit commit waits for the per-hole region (§6.3).
    if(!settingsEditable()||activePairItem())return;
    const auto fit=latitude_.value(QLatin1String("fit")).toMap();
    if(!fit.value(QLatin1String("valid")).toBool())return;
    const auto delta=fit.value(QLatin1String("params_delta")).toMap();
    if(delta.isEmpty())return;
    auto next=extra_settings_;
    const auto number=[&](const QString& key,double fallback){
        const auto v=delta.value(key);
        return v.canConvert<double>()?v.toDouble():fallback;
    };
    next.scene_latitude_active=delta.value(QLatin1String("scene_latitude_active")).toBool();
    const auto norm=delta.value(QLatin1String("scene_latitude_norm")).toString();
    if(norm==QLatin1String("power")||norm==QLatin1String("y")||norm==QLatin1String("max"))
        next.scene_latitude_norm=norm.toStdString();
    next.scene_latitude_highlight_knee=number(QLatin1String("scene_latitude_highlight_knee"),next.scene_latitude_highlight_knee);
    next.scene_latitude_highlight_room=number(QLatin1String("scene_latitude_highlight_room"),next.scene_latitude_highlight_room);
    next.scene_latitude_shadow_knee=number(QLatin1String("scene_latitude_shadow_knee"),next.scene_latitude_shadow_knee);
    next.scene_latitude_shadow_room=number(QLatin1String("scene_latitude_shadow_room"),next.scene_latitude_shadow_room);
    next.scene_latitude_rolloff=number(QLatin1String("scene_latitude_rolloff"),next.scene_latitude_rolloff);
    next.scene_latitude_max_lift=number(QLatin1String("scene_latitude_max_lift"),next.scene_latitude_max_lift);
    if(next==extra_settings_)return;
    extra_settings_=next;changed(QStringLiteral("latitudeFit"));
}

// --- Layer 2: the scan-side grade on the controller's fast path ---------------

QImage PreviewController::gradedDisplayImage(const desktop::FramePtr& frame,const l2::Adjustments& adjustments,int previewLongEdge) const {
    // A neutral grade stays byte-identical: applyImage would still run its
    // per-pixel chain, so it is skipped outright and the factory's
    // external-buffer wrapper is returned zero-copy.
    auto image=image_factory_(frame);
    if(previewLongEdge>0) {
        const int longEdge=std::max(image.width(),image.height());
        if(longEdge>previewLongEdge) {
            const double k=double(previewLongEdge)/double(longEdge);
            // The scale returns a fresh deep copy, so the grade below writes
            // into private pixels and the retained frame stays pristine.
            image=image.scaled(int(image.width()*k),int(image.height()*k),
                               Qt::IgnoreAspectRatio,Qt::SmoothTransformation);
        }
    }
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
    if(frame_)status_=L("Updating the preview");
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
        L("Updating the preview"),"layer2");
    full_layer2_timer_.start();
}
void PreviewController::requestLatitude() {
    // The engine's analysis reads the open session and never renders, so it
    // must not bump the generation: a render reply already in flight stays
    // valid, and a render submitted after this call retires this reply
    // through the ordinary generation guard. The worker holds the host, and
    // the analysis needs its live session (an open frame with a render).
    // A pair's reading is the piece's per-hole region (PAIR-WIRING §6.2);
    // until the pair region arrives with the pair UI, the analysis refuses
    // on a pair rather than read both pictures as one scene.
    if(!frame_||busy_||activePairItem())return;
    const auto gen=generation_;
    submit([gen](desktop::PreviewHost& host){Reply r;r.generation=gen;r.operation="latitude";
        try{r.latitude_json=text(host.sceneLatitude());}
        catch(const std::exception& e){r.error=text(e.what());}return r;},
        L("Analysing the latitude…"),"latitude");
}
void PreviewController::refreshBeforeImage() {
    // The before half is the plain decode — the RAW's own pixels, no film,
    // no paper — cut to the photo's geometry, so the two halves align. The
    // comparison therefore shows what the whole pipeline (develop + edit)
    // changed, not only the last drag; the fetch rode the open job.
    if(!frame_||!compare_enabled_||before_plain_.isNull()){before_image_={};return;}
    try {
        before_image_=output::displayImage(before_plain_,geometry_);
    } catch(const std::exception&){before_image_={};}
}
void PreviewController::setCompareEnabled(bool enabled) {
    if(compare_enabled_==enabled)return;
    compare_enabled_=enabled;
    // Toggling the comparison republishes the retained print: the before view
    // only exists while the comparison is on and the grade is non-neutral.
    if(frame_)publishFromFrame(L("Preview updated"));
    emit compareEnabledChanged();
}
void PreviewController::publishFromFrame(const QString& status,bool fullQuality) {
    if(!frame_)return;
    try {
        // The preview tier downscales the print before grading: the grade's
        // coordinates are normalized, so the semantics survive the scale,
        // and a slider drag cannot afford a 22-megapixel CPU grade per
        // move. The full tier (the idle timer's job, every open and render)
        // restores the print's own resolution and updates full_image_.
        constexpr int kLayer2PreviewEdge=2560;
        auto next=gradedDisplayImage(frame_,layer2_,fullQuality?0:kLayer2PreviewEdge);
        auto visible=fullQuality
            ?visibleImage(frame_,next,geometry_,layer2_)
            :output::displayImage(next,geometry_);
        if(fullQuality)full_image_=std::move(next);image_=std::move(visible);
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
        // A pair row carries the two holes' names; it owns no file of its own.
        const QString name=item.is_pair
            ?(QFileInfo(item.pair_sources[0].toLocalFile()).fileName()+QStringLiteral(" + ")+
              QFileInfo(item.pair_sources[1].toLocalFile()).fileName())
            :QFileInfo(item.url.toLocalFile()).fileName();
        result.push_back(QVariantMap{{"name",name},
            {"url",item.url},{"thumbnail",item.thumbnail},{"active",i==active_index_},
            {"selected",item.selected},{"error",item.error},{"pair",item.is_pair}});
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
    // A pair's look syncs to no photograph and no photograph's look syncs to
    // a pair (the pair's wire rows are its own; its holes keep their frames).
    if(!frame_||busy_||dialog_open_||activePairItem())return;
    saveActiveSettings();int count=0;
    for(int i=0;i<int(library_.size());++i)if(i!=active_index_&&library_[i].selected&&!library_[i].is_pair){
        auto& item=library_[i];auto next=currentState();
        if(!includeCrop)next.geometry=item.geometry;
        if(sameState(itemState(item),next))continue;
        // Each recipient owns its own undo history, just like its settings.
        item.undo.push_back(itemState(item));if(item.undo.size()>64)item.undo.erase(item.undo.begin());item.redo.clear();
        item.settings=next.settings;item.mode=next.mode;item.geometry=next.geometry;item.layer2=next.layer2;item.pending_save=true;
        item.thumbnail={};item.error.clear();++count;
    }
    status_=L("Synced the current settings to %@ photos%@",{count,
        includeCrop?L(" (crop included)"):L(" (each keeps its crop)")});
    scheduleSave();emit libraryChanged();emit historyChanged();emit stateChanged();
}
// --- The half-frame pair: item, jobs, per-hole edits (PAIR-WIRING §5) -----

PreviewController::LibraryItem* PreviewController::activePairItem() {
    if(active_index_<0||active_index_>=int(library_.size()))return nullptr;
    auto& item=library_[active_index_];
    return item.is_pair?&item:nullptr;
}
const PreviewController::LibraryItem* PreviewController::activePairItem() const {
    if(active_index_<0||active_index_>=int(library_.size()))return nullptr;
    const auto& item=library_[active_index_];
    return item.is_pair?&item:nullptr;
}
// The pair state's own snapshot for the record and the wire: the pair fields
// with the item's live look carried along (the look lives in the item's
// ordinary fields; the record's look block is this state written out).
editing::PairState PreviewController::pairRecordState(const LibraryItem& item) {
    auto state=item.pair;
    state.look=itemState(item);
    return state;
}
bool PreviewController::createPair(const QUrl& left,const QUrl& right) {
    if(!ready_||busy_||dialog_open_||!left.isLocalFile()||!right.isLocalFile())return false;
    saveActiveSettings();
    const int leftIndex=addFile(left),rightIndex=addFile(right);
    if(leftIndex<0||rightIndex<0)return false;
    // A pair is never a hole of another pair, and the two holes are two
    // photographs (PAIR-WIRING §8).
    if(library_[leftIndex].is_pair||library_[rightIndex].is_pair||leftIndex==rightIndex)return false;
    // An identical pair already sits in the library: choose it, don't twin it.
    for(int i=0;i<int(library_.size());++i) {
        const auto& item=library_[i];
        if(item.is_pair&&item.pair_sources[0]==library_[leftIndex].url&&
            item.pair_sources[1]==library_[rightIndex].url) {
            if(i!=active_index_)selectImage(i);
            return true;
        }
    }
    LibraryItem item;
    item.is_pair=true;
    item.pair_sources[0]=library_[leftIndex].url;
    item.pair_sources[1]=library_[rightIndex].url;
    // The initial look: the first photo's film/paper settings with the
    // pair's own wire rows forced (PAIR-WIRING §3's table): the meter off,
    // exposure 0, the piece's format at the strip's 1 mm, the split at the
    // gap's middle, the second curve at its defaults, film edge and date
    // off -- a bare piece to begin with.
    item.settings=library_[leftIndex].settings;
    item.settings.auto_exposure=false;
    item.settings.film_exposure_ev=0.0;
    item.settings.film_format_mm=pairc::kHoleAlongMM*2.0+1.0;
    item.settings.scene_latitude_split=0.5;
    item.settings.scene_latitude_b_active=false;
    item.settings.scene_latitude_b_highlight_knee=2.0;
    item.settings.scene_latitude_b_highlight_room=0.0;
    item.settings.scene_latitude_b_shadow_knee=-2.0;
    item.settings.scene_latitude_b_shadow_room=0.0;
    item.settings.overscan_active=false;
    item.settings.overscan_pair=false;
    item.settings.date_imprint_active=false;
    item.layer2=library_[leftIndex].layer2;
    item.geometry={};
    item.mode=desktop::DecodeMode::compatible16;
    item.pair.look=itemState(item);
    library_.push_back(std::move(item));
    const int index=int(library_.size())-1;
    // It is kept when it is made (§5): the record follows the first save.
    library_[index].pending_save=true;
    scheduleSave();
    emit libraryChanged();
    submitPairOpen(index);
    return true;
}
QVariantMap PreviewController::pair() const {
    if(active_index_<0||active_index_>=int(library_.size())||!library_[active_index_].is_pair)return {};
    const auto& item=library_[active_index_];
    QVariantList holes;
    for(int side=0;side<2;++side) {
        const auto& hole=item.pair.holes[side];
        // The hole's meter reading, when the item's cache holds its current
        // key. The key is the source frame's own decode (placement excluded,
        // PAIR-WIRING §4.2), computed here only to read the cache.
        QVariant metered;
        const auto source=std::find_if(library_.cbegin(),library_.cend(),
            [&](const LibraryItem& candidate){return !candidate.is_pair&&candidate.url==item.pair_sources[side];});
        if(source!=library_.cend()) {
            const QString key=pairc::meterKey(source->url.toLocalFile(),
                source->settings.wb_temperature_k==0.0?QStringLiteral("as_shot"):QStringLiteral("override"),
                source->settings.wb_temperature_k,source->settings.wb_tint,false,
                QStringLiteral("center_weighted"),source->geometry.crop,source->geometry.quarterTurns);
            const auto cached=item.meter_cache.constFind(key);
            if(cached!=item.meter_cache.cend())metered=QVariant(cached.value());
        }
        holes.push_back(QVariantMap{
            {"scale",hole.placement.scale},{"x",hole.placement.x},{"y",hole.placement.y},
            {"quarterTurns",hole.placement.quarterTurns},{"exposureEv",hole.exposure_ev},
            {"dated",hole.dated},{"dateText",hole.date_text},
            {"trim",QVariantMap{{"printExposure",hole.trim.print_exposure},
                {"yFilterShift",hole.trim.y_filter_shift},{"mFilterShift",hole.trim.m_filter_shift},
                {"preflashExposure",hole.trim.preflash_exposure}}},
            {"meteredEv",metered},
            {"sourceName",item.pair_sources[side].isEmpty()
                ?QString():QFileInfo(item.pair_sources[side].toLocalFile()).fileName()}});
    }
    return {{"spacingMM",item.pair.spacing_mm},{"turned",item.pair.turned},{"holes",holes},
        {"split",item.settings.scene_latitude_split},{"bActive",item.settings.scene_latitude_b_active},
        {"leftUrl",item.pair_sources[0]},{"rightUrl",item.pair_sources[1]},
        {"saved",!item.pending_save}};
}
void PreviewController::setPairHole(int side,const QString& field,const QVariant& value) {
    auto* item=activePairItem();
    if(!item||busy_||dialog_open_)return;
    if(side<0||side>1)return;
    bool ok=false;
    const double number=value.toDouble(&ok);
    editing::PairState next=item->pair;
    auto& hole=next.holes[side];
    // A buffer change re-composes the piece (a new open job); a wire-only
    // row takes the reprint path of the same negative. The meter cache holds
    // either way: none of these fields is part of a meter key. The trim rows
    // are stored and persisted but wired nowhere yet (PAIR-WIRING §12's
    // print-layer cut is a later port), so a trim edit submits nothing: the
    // reprint would re-send provably identical settings. When the trim
    // wiring lands, fold these fields back into the wire path below.
    bool buffer=false,trim=false;
    if(field==QLatin1String("scale")) {
        if(!ok||!std::isfinite(number)||number<1||number>8)return;
        hole.placement.scale=number;buffer=true;
    } else if(field==QLatin1String("x")||field==QLatin1String("y")) {
        if(!ok||!std::isfinite(number)||number<-2||number>2)return;
        (field==QLatin1String("x")?hole.placement.x:hole.placement.y)=number;
        buffer=true;
    } else if(field==QLatin1String("quarterTurns")) {
        bool okInt=false;const int v=value.toInt(&okInt);
        if(!okInt||v<0||v>3)return;
        hole.placement.quarterTurns=v;buffer=true;
    } else if(field==QLatin1String("exposureEv")) {
        if(!ok||!std::isfinite(number)||number<-5||number>5)return;
        hole.exposure_ev=number;buffer=true;
    } else if(field==QLatin1String("dated")) {
        if(!value.canConvert<bool>())return;
        hole.dated=value.toBool();
    } else if(field==QLatin1String("dateText")) {
        hole.date_text=value.toString();
    } else if(field==QLatin1String("trimPrintExposure")) {
        if(!ok||!std::isfinite(number)||number<-4||number>4)return;
        hole.trim.print_exposure=number;trim=true;
    } else if(field==QLatin1String("trimYFilterShift")) {
        if(!ok||!std::isfinite(number)||number<-1||number>1)return;
        hole.trim.y_filter_shift=number;trim=true;
    } else if(field==QLatin1String("trimMFilterShift")) {
        if(!ok||!std::isfinite(number)||number<-1||number>1)return;
        hole.trim.m_filter_shift=number;trim=true;
    } else if(field==QLatin1String("trimPreflashExposure")) {
        if(!ok||!std::isfinite(number)||number<0||number>8)return;
        hole.trim.preflash_exposure=number;trim=true;
    } else return;
    if(samePairState(next,item->pair))return;
    recordPairEdit(*item,item->pair);
    item->pair=next;item->thumbnail={};item->pending_save=true;
    scheduleSave();
    failed_=false;
    emit pairChanged();emit libraryChanged();emit historyChanged();emit stateChanged();
    if(buffer)submitPairOpen(active_index_);
    else if(!trim)submitPairWire(active_index_);
}
void PreviewController::setPairPiece(const QString& name,const QVariant& value) {
    auto* item=activePairItem();
    if(!item||busy_||dialog_open_)return;
    editing::PairState next=item->pair;
    if(name==QLatin1String("spacingMM")) {
        bool ok=false;const double v=value.toDouble(&ok);
        if(!ok||!std::isfinite(v)||v<0||v>4)return;
        next.spacing_mm=v;
    } else if(name==QLatin1String("turned")) {
        if(!value.canConvert<bool>())return;
        next.turned=value.toBool();
    } else return;
    if(samePairState(next,item->pair))return;
    recordPairEdit(*item,item->pair);
    item->pair=next;item->thumbnail={};item->pending_save=true;
    scheduleSave();
    failed_=false;
    emit pairChanged();emit libraryChanged();emit historyChanged();emit stateChanged();
    submitPairOpen(active_index_);
}
void PreviewController::recordPairEdit(LibraryItem& item,const editing::PairState& before) {
    // The same one-step-per-gesture discipline as the look's recordEdit;
    // pair sliders in the pair panel will drive it through beginEditGesture.
    if(gesture_index_!=active_index_||!gesture_recorded_) {
        item.pair_undo.push_back(before);
        if(item.pair_undo.size()>64)item.pair_undo.erase(item.pair_undo.begin());
        item.pair_redo.clear();
        if(gesture_index_==active_index_)gesture_recorded_=true;
    }
}
void PreviewController::submitPairOpen(int index) {
    const auto snapshot=library_.at(index);
    PairSources sources;
    for(int side=0;side<2;++side) {
        sources[side].url=snapshot.pair_sources[side];
        // The decode ref is the source frame's own (PAIR-WIRING §5: the shot
        // stays the frame's; the pair reads it again at every open).
        const auto source=std::find_if(library_.cbegin(),library_.cend(),
            [&](const LibraryItem& candidate){return !candidate.is_pair&&candidate.url==sources[side].url;});
        if(source!=library_.cend()) {
            sources[side].settings=source->settings;
            sources[side].geometry=source->geometry;
            sources[side].mode=source->mode;
        }
    }
    const bool positive=std::any_of(catalog_.films.cbegin(),catalog_.films.cend(),
        [&](const desktop::Stock& stock){return stock.id==snapshot.settings.film_stock&&stock.is_positive;});
    const auto pair=pairRecordState(snapshot);
    const auto cache=snapshot.meter_cache;
    const auto gen=++generation_;
    submit([sources,pair,cache,positive,gen,index](desktop::PreviewHost& host) {
        Reply r;r.generation=gen;r.operation="open";r.library_index=index;
        r.geometry=pair.look.geometry;r.layer2=pair.look.layer2;
        try {r.frame=pairOpenFrame(host,sources,pair,cache,positive,&r.meter_updates);}
        catch(const std::exception& e){r.error=text(e.what());}
        return r;
    },L("Composing the half-frame pair…"),"open");
}
void PreviewController::submitPairWire(int index) {
    const auto snapshot=library_.at(index);
    PairSources sources;
    for(int side=0;side<2;++side) {
        sources[side].url=snapshot.pair_sources[side];
        const auto source=std::find_if(library_.cbegin(),library_.cend(),
            [&](const LibraryItem& candidate){return !candidate.is_pair&&candidate.url==sources[side].url;});
        if(source!=library_.cend()) {
            sources[side].settings=source->settings;
            sources[side].geometry=source->geometry;
            sources[side].mode=source->mode;
        }
    }
    const bool positive=std::any_of(catalog_.films.cbegin(),catalog_.films.cend(),
        [&](const desktop::Stock& stock){return stock.id==snapshot.settings.film_stock&&stock.is_positive;});
    const auto pair=pairRecordState(snapshot);
    const auto wire=pairWireOf(pair);
    const auto held=snapshot.pair_frame;
    const auto cache=snapshot.meter_cache;
    const auto gen=++generation_;
    // The wire reply rides the ordinary render contract: without the
    // settings revision the superseded check in accept() compares 0 against
    // every post-edit revision and silently drops EVERY pair wire reply
    // (found by the first real-RAW gate run — the walk had been
    // compile-only before fixtures existed).
    const auto revision=settings_revision_;
    submit([sources,pair,wire,held,cache,positive,gen,revision,index](desktop::PreviewHost& host) {
        Reply r;r.generation=gen;r.settings_revision=revision;r.operation="render";r.library_index=index;
        try {
            // The date and trim rows live on the wire, not in the pixels: a
            // live pair session reprints its kept negative. Anything else
            // (a failed job discarded the session) falls back to the full
            // compose+open, which lands the same wire.
            const auto active=host.current();
            r.frame=active&&active==held?host.render(wire)
                :pairOpenFrame(host,sources,pair,cache,positive,&r.meter_updates);
        }
        catch(const std::exception& e){r.error=text(e.what());}
        return r;
    },L("Updating the half-frame pair…"),"render");
}
void PreviewController::restorePairHistory(bool redo) {
    if(!ready_||busy_||dialog_open_)return;
    auto* item=activePairItem();
    if(!item)return;
    endEditGesture();
    auto& from=redo?item->pair_redo:item->pair_undo;
    auto& to=redo?item->pair_undo:item->pair_redo;
    if(from.empty())return;
    auto target=from.back();
    to.push_back(item->pair);if(to.size()>64)to.erase(to.begin());
    from.pop_back();
    // A pair edit restores the pair fields; the look keeps its own stack.
    target.look=itemState(*item);
    item->pair=target;
    item->thumbnail={};item->pending_save=true;scheduleSave();
    failed_=false;
    status_=redo?L("Redid the pair's adjustment"):L("Undid the pair's adjustment");
    emit pairChanged();emit libraryChanged();emit historyChanged();emit stateChanged();
    submitPairOpen(active_index_);
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
    if(!settingsEditable()||active_index_<0)return false;
    const auto& item=library_[active_index_];
    return item.is_pair?(!item.pair_undo.empty()||!item.undo.empty()):!item.undo.empty();
}
bool PreviewController::canRedo() const {
    if(!settingsEditable()||active_index_<0)return false;
    const auto& item=library_[active_index_];
    return item.is_pair?(!item.pair_redo.empty()||!item.redo.empty()):!item.redo.empty();
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
void PreviewController::undo(){
    // A pair item walks its own stacks first (the pair fields), then the
    // look's; the two stacks split by concern.
    if(active_index_>=0&&active_index_<int(library_.size())&&library_[active_index_].is_pair&&
        !library_[active_index_].pair_undo.empty()){restorePairHistory(false);return;}
    restoreHistory(false);
}
void PreviewController::redo(){
    if(active_index_>=0&&active_index_<int(library_.size())&&library_[active_index_].is_pair&&
        !library_[active_index_].pair_redo.empty()){restorePairHistory(true);return;}
    restoreHistory(true);
}
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
        failed_=false;status_=redo?L("Redid the photo's adjustment"):L("Undid the photo's last adjustment");
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
        try {
            if(item.is_pair)edit_store_.savePair(item.pair_sources[0],item.pair_sources[1],pairRecordState(item));
            else edit_store_.save(item.url,itemState(item));
            item.pending_save=false;item.has_stored=true;item.save_warning.clear();
        }
        catch(const std::exception& e){item.save_warning=text(e.what());}
    }
    emit stateChanged();
    if(activePairItem())emit pairChanged();
}
QString PreviewController::persistenceWarning() const {
    QStringList warnings;
    for(const auto& item:library_)if(!item.save_warning.isEmpty())
        // A pair item owns no file of its own: the warning names the piece
        // the way the strip does, by its two holes ("left + right"), so the
        // row never renders as ": message" over an empty URL.
        warnings.append((item.is_pair
            ?QFileInfo(item.pair_sources[0].toLocalFile()).fileName()+QStringLiteral(" + ")+
             QFileInfo(item.pair_sources[1].toLocalFile()).fileName()
            :QFileInfo(item.url.toLocalFile()).fileName())+": "+item.save_warning);
    return warnings.join("\n");
}
QString PreviewController::savedStatus() const {
    if(!persistenceWarning().isEmpty())return L("Some settings did not save; see the note below");
    for(const auto& item:library_)if(item.pending_save)return L("Auto-saving the settings…");
    if(active_index_>=0&&library_[active_index_].has_stored)return L("The photo's settings saved automatically");
    return L("Adjustments save automatically and return when the photo reopens");
}
// Flips are stored in output axes. Rotating the displayed image also swaps
// those axes; without this a mirrored photo would turn the opposite way.
void PreviewController::rotateClockwise(){auto next=geometry_;next.quarterTurns=(next.quarterTurns+1)%4;std::swap(next.flipHorizontal,next.flipVertical);setGeometry(next,"quarterTurns");}
void PreviewController::rotateCounterClockwise(){auto next=geometry_;next.quarterTurns=(next.quarterTurns+3)%4;std::swap(next.flipHorizontal,next.flipVertical);setGeometry(next,"quarterTurns");}
void PreviewController::setFlipHorizontal(bool value){auto next=geometry_;next.flipHorizontal=value;setGeometry(next,"flipHorizontal");}
void PreviewController::setFlipVertical(bool value){auto next=geometry_;next.flipVertical=value;setGeometry(next,"flipVertical");}
void PreviewController::setStraightenDegrees(double value){auto next=geometry_;next.straightenDegrees=value;setGeometry(next,"straightenDegrees");}
void PreviewController::resetGeometry(){setGeometry(output::Geometry{});}

void PreviewController::exportBatch(const QUrl& folder,int format,int quality,int maxLongEdge,int colourSpace) {
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
    submit([this,items=std::move(items),held,resources,destination,gen,cancel,format,quality,maxLongEdge,colourSpace](auto& host){
        Reply r;r.generation=gen;r.operation="batch-export";
        int done=0,skipped=0,errors=0,processed=0;QStringList details;
        try {
            auto options=exportOptions(format,quality,maxLongEdge,colourSpace);
            // One fetch per destination for the whole batch (the Mac's
            // `setups[target]` discipline), taken from the shown frame's
            // resolved working space; the batch re-opens every other photo
            // with the same wire, so their working spaces are the same one.
            desktop::OutputTransformData transform;
            if(held) {
                const std::string target=kExportColourNames[std::size_t(colourSpace)];
                if(target!=held->output_color_space) {
                    transform=host.outputTransform(held->output_color_space,target);
                    options.transform=&transform;
                }
            } else if(colourSpace!=0) {
                throw std::runtime_error("Batch export colour needs an open photograph");
            }
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
                    if(item.is_pair) {
                        // The piece's own export path (PAIR-WIRING §6.4) is
                        // the open frame's; the batch driver re-opens RAWs,
                        // which a pair does not have. Refused, not skipped.
                        ++errors;
                        details.append(L("Half-frame pair %@: batch export is not supported yet",{
                            QFileInfo(item.pair_sources[0].toLocalFile()).fileName()}));
                    }
                    else if(std::filesystem::exists(target))++skipped;
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
            r.message=L("%@: exported %@, skipped %@, failed %@",{
                cancel->load()?L("Batch export cancelled"):L("Batch export finished"),
                QString::number(done),QString::number(skipped),QString::number(errors)});
            if(errors)r.error=r.message+"\n"+details.join("\n");
        }catch(const std::exception& e){r.error=text(e.what());}
        return r;
    },L("Batch exporting…"),"batch-export");
}
