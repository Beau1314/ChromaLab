#pragma once
#include "desktop/preview_host.hpp"
#include "FrameOutput.hpp"
#include "EditStore.hpp"
#include <QImage>
#include <QObject>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <atomic>

class PreviewController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool ready READ ready NOTIFY stateChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(bool hasFrame READ hasFrame NOTIFY frameChanged)
    Q_PROPERTY(bool failed READ failed NOTIFY stateChanged)
    Q_PROPERTY(bool dirty READ dirty NOTIFY settingsChanged)
    Q_PROPERTY(bool settingsEditable READ settingsEditable NOTIFY stateChanged)
    Q_PROPERTY(bool filmIsPositive READ filmIsPositive NOTIFY settingsChanged)
    Q_PROPERTY(QString status READ status NOTIFY stateChanged)
    Q_PROPERTY(QString fileName READ fileName NOTIFY frameChanged)
    Q_PROPERTY(QUrl sourceFolder READ sourceFolder NOTIFY frameChanged)
    Q_PROPERTY(QImage previewImage READ previewImage NOTIFY frameChanged)
    // Split compare: the ungraded print ("冲洗后、编辑前" — the current
    // film/paper print WITHOUT Layer 2) that pairs with previewImage. It is
    // only computed while compareEnabled is on and the grade is non-neutral.
    Q_PROPERTY(QImage beforeImage READ beforeImage NOTIFY frameChanged)
    Q_PROPERTY(bool compareEnabled READ compareEnabled WRITE setCompareEnabled NOTIFY compareEnabledChanged)
    // The output histogram: 4 nested lists of 256 ints ([0..3] = R/G/B/luma),
    // counted in parallel over the published preview's pixels (geometry and
    // grade applied). All zeros while no frame is published.
    Q_PROPERTY(QVariantList histogram READ histogram NOTIFY histogramChanged)
    // The engine's scene latitude analysis (RFC-023) of the open session, as
    // the reply of the last requestLatitude(): the raw reply's "medium",
    // "scene", "suggested" and "fit" objects (empty until one runs, and
    // cleared whenever a new print is published -- the analysis describes the
    // session, so an engine re-render invalidates it). Read-only: the fit's
    // mapping is never applied here (Windows refuses `scene_latitude_active`).
    Q_PROPERTY(QVariantMap latitude READ latitude NOTIFY latitudeChanged)
    // True only while a latitude job is in flight (same shape as
    // batchExporting): submit() sets busy_/active_operation_ and emits
    // stateChanged; accept() clears both and emits stateChanged again.
    Q_PROPERTY(bool latitudeBusy READ latitudeBusy NOTIFY stateChanged)
    Q_PROPERTY(QVariantMap metadata READ metadata NOTIFY frameChanged)
    Q_PROPERTY(QVariantList films READ films NOTIFY catalogChanged)
    Q_PROPERTY(QVariantList papers READ papers NOTIFY catalogChanged)
    Q_PROPERTY(int filmIndex READ filmIndex WRITE setFilmIndex NOTIFY settingsChanged)
    Q_PROPERTY(int paperIndex READ paperIndex WRITE setPaperIndex NOTIFY settingsChanged)
    Q_PROPERTY(int decodeMode READ decodeMode WRITE setDecodeMode NOTIFY settingsChanged)
    Q_PROPERTY(double exposureEv READ exposureEv WRITE setExposureEv NOTIFY settingsChanged)
    // White balance (RAW decode side, Adobe DNG Temperature/Tint). 0/0 is
    // As Shot — no override, shown as 自动; the As Shot buttons backfill the
    // decoder-reported as-shot numbers as the edited value. A change re-decodes
    // the photo (transactional re-open), so unlike the render-side grade rows
    // it is not instant.
    Q_PROPERTY(double wbTemperature READ wbTemperature NOTIFY settingsChanged)
    Q_PROPERTY(double wbTint READ wbTint NOTIFY settingsChanged)
    // True while the open photo's decoder reported an as-shot illuminant to
    // backfill (needs a three-colour camera matrix; absent values stay 0).
    Q_PROPERTY(bool wbAsShotAvailable READ wbAsShotAvailable NOTIFY frameChanged)
    // The camera's meter (engine camera.auto_exposure, center-weighted). An
    // engine parameter: a toggle re-renders without re-decoding.
    Q_PROPERTY(bool autoExposure READ autoExposure WRITE setAutoExposure NOTIFY settingsChanged)
    Q_PROPERTY(QVariantMap adjustments READ adjustments NOTIFY settingsChanged)
    Q_PROPERTY(QVariantMap layer2 READ layer2 NOTIFY settingsChanged)
    Q_PROPERTY(QVariantList libraryItems READ libraryItems NOTIFY libraryChanged)
    Q_PROPERTY(int activeIndex READ activeIndex NOTIFY libraryChanged)
    Q_PROPERTY(int selectedCount READ selectedCount NOTIFY libraryChanged)
    Q_PROPERTY(QImage uncroppedImage READ uncroppedImage NOTIFY frameChanged)
    Q_PROPERTY(QRectF cropRect READ cropRect WRITE setCropRect NOTIFY geometryChanged)
    Q_PROPERTY(bool batchExporting READ batchExporting NOTIFY stateChanged)
    Q_PROPERTY(int batchProgress READ batchProgress NOTIFY stateChanged)
    Q_PROPERTY(int batchTotal READ batchTotal NOTIFY stateChanged)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY historyChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY historyChanged)
    Q_PROPERTY(QString persistenceWarning READ persistenceWarning NOTIFY stateChanged)
    Q_PROPERTY(QString savedStatus READ savedStatus NOTIFY stateChanged)
    Q_PROPERTY(int quarterTurns READ quarterTurns NOTIFY geometryChanged)
    Q_PROPERTY(bool flipHorizontal READ flipHorizontal NOTIFY geometryChanged)
    Q_PROPERTY(bool flipVertical READ flipVertical NOTIFY geometryChanged)
    Q_PROPERTY(double straightenDegrees READ straightenDegrees NOTIFY geometryChanged)
public:
    using ImageFactory=std::function<QImage(const spk::desktop::FramePtr&)>;
    explicit PreviewController(std::filesystem::path resources, QObject* parent=nullptr, ImageFactory imageFactory={}, QString editDirectory={});
    static QImage imageFromFrame(const spk::desktop::FramePtr&);
    ~PreviewController() override;
    bool ready() const { return ready_; }
    bool busy() const { return busy_; }
    bool hasFrame() const { return bool(frame_); }
    bool failed() const { return failed_; }
    bool dirty() const;
    // The "layer2" widening is a bounded transient, required so live slider
    // dragging is not blocked while a layer2 flight is in the air. An
    // engine-parameter edit accepted during that flight is coalesced
    // (layer2_pending_) and rendered after the layer2 reply publishes
    // (~180 ms debounce + render); during that window the displayed pixels
    // may briefly show the layer2 reply with pre-edit engine settings.
    bool settingsEditable() const { return ready_&&!dialog_open_&&(!busy_||active_operation_=="render"||active_operation_=="layer2"); }
    bool filmIsPositive() const;
    QString status() const { return status_; }
    QString fileName() const;
    QUrl sourceFolder() const;
    QImage previewImage() const { return image_; }
    QImage beforeImage() const { return before_image_; }
    bool compareEnabled() const { return compare_enabled_; }
    void setCompareEnabled(bool enabled);
    QVariantList histogram() const { return histogram_; }
    QVariantMap metadata() const;
    QVariantList films() const { return films_; }
    QVariantList papers() const { return papers_; }
    int filmIndex() const { return film_index_; }
    int paperIndex() const { return paper_index_; }
    int decodeMode() const { return decode_mode_; }
    double exposureEv() const { return exposure_ev_; }
    double wbTemperature() const { return extra_settings_.wb_temperature_k; }
    double wbTint() const { return extra_settings_.wb_tint; }
    bool wbAsShotAvailable() const;
    bool autoExposure() const { return extra_settings_.auto_exposure; }
    void setAutoExposure(bool value);
    QVariantMap adjustments() const;
    // Layer 2 — the scan-side grade. Keys: enabled + the ten scalar sliders +
    // curves{channel: point list} + colorBalance{zone:{hue,saturation,luminance}}
    // + vignette{amount,midpoint}. setLayer2 also accepts the flat vignette
    // names "vignetteAmount"/"vignetteMidpoint".
    QVariantMap layer2() const;
    QVariantList libraryItems() const;
    int activeIndex() const { return active_index_; }
    int selectedCount() const;
    QImage uncroppedImage() const { return full_image_; }
    QRectF cropRect() const { return geometry_.crop; }
    int quarterTurns() const { return geometry_.quarterTurns; }
    bool flipHorizontal() const { return geometry_.flipHorizontal; }
    bool flipVertical() const { return geometry_.flipVertical; }
    double straightenDegrees() const { return geometry_.straightenDegrees; }
    bool canUndo() const;
    bool canRedo() const;
    QString persistenceWarning() const;
    QString savedStatus() const;
    bool batchExporting() const { return busy_ && active_operation_=="batch-export"; }
    int batchProgress() const { return batch_progress_; }
    int batchTotal() const { return batch_total_; }
    QVariantMap latitude() const { return latitude_; }
    bool latitudeBusy() const { return busy_ && active_operation_=="latitude"; }
    void setFilmIndex(int value);
    void setPaperIndex(int value);
    void setDecodeMode(int value);
    void setExposureEv(double value);
    spk::desktop::FramePtr frame() const { return frame_; }
    Q_INVOKABLE void openRaw(const QUrl& path);
    Q_INVOKABLE void openFiles(const QVariantList& urls);
    Q_INVOKABLE void selectImage(int index);
    Q_INVOKABLE void setItemSelected(int index, bool selected);
    Q_INVOKABLE void selectAllPhotos(bool selected);
    Q_INVOKABLE void syncSelectedSettings(bool includeCrop=false);
    Q_INVOKABLE void setAdjustment(const QString& name, const QVariant& value);
    // White balance edits (each an ordinary undoable adjustment that re-decodes
    // the photo): setWb applies an explicit illuminant, applyWbPreset one of
    // the camera's named ones (daylight/cloudy/shade/tungsten/fluorescent/
    // flash), applyWbAsShot backfills the decoder-reported as-shot numbers as
    // the edited value, and resetWb returns to 0/0 = As Shot. Refused while
    // the experimental headroom decode is selected — that policy keeps the
    // file's own multipliers — and no-ops without an undo step when the value
    // already matches.
    Q_INVOKABLE void setWb(double kelvin, double tint);
    Q_INVOKABLE void applyWbPreset(const QString& name);
    Q_INVOKABLE void applyWbAsShot();
    Q_INVOKABLE void resetWb();
    // Layer 2 edits run on the fast path: they republish the retained print
    // through the grade and never re-render the engine, so a job in flight
    // stays valid. Range contracts match the desktop's adjustment sliders.
    Q_INVOKABLE void setLayer2(const QString& name, double value);
    Q_INVOKABLE void curveInsertPoint(const QString& channel, double x, double y);
    Q_INVOKABLE void curveMovePoint(const QString& channel, int index, double x, double y);
    Q_INVOKABLE void curveRemovePoint(const QString& channel, int index);
    Q_INVOKABLE void curveReset(const QString& channel);
    Q_INVOKABLE QVariantList curvePoints(const QString& channel) const;
    Q_INVOKABLE double curveEval(const QString& channel, double x) const;
    // zone: 0=master 1=shadows 2=midtones 3=highlights; axis: 0=hue 1=sat 2=lum.
    // Hue spans [0,360]; saturation [0,1]; luminance [-1,1] (the desktop's ranges).
    Q_INVOKABLE void setZone(int zone, int axis, double value);
    Q_INVOKABLE void setCropRect(const QRectF& crop);
    Q_INVOKABLE void resetCrop();
    Q_INVOKABLE void setCropAspect(double ratio);
    Q_INVOKABLE void rotateClockwise();
    Q_INVOKABLE void rotateCounterClockwise();
    Q_INVOKABLE void setFlipHorizontal(bool value);
    Q_INVOKABLE void setFlipVertical(bool value);
    Q_INVOKABLE void setStraightenDegrees(double value);
    Q_INVOKABLE void resetGeometry();
    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();
    Q_INVOKABLE void beginEditGesture(const QString& key);
    Q_INVOKABLE void endEditGesture();
    Q_INVOKABLE void flushEdits();
    Q_INVOKABLE void apply();
    Q_INVOKABLE void exportTiff(const QUrl& path);
    Q_INVOKABLE void exportImage(const QUrl& path, int format, int quality, int maxLongEdge);
    Q_INVOKABLE void exportBatch(const QUrl& folder, int format, int quality, int maxLongEdge);
    Q_INVOKABLE void cancelExport();
    Q_INVOKABLE void resetSettings();
    Q_INVOKABLE void setDialogOpen(bool open);
    // The engine's scene latitude analysis of the open session (a read-only
    // measurement: the reply never renders, and the fit's mapping is refused
    // on Windows). Like the layer2 job it captures the current generation
    // without bumping it, so an in-flight render reply stays valid; the job
    // runs on the worker and the reply publishes the `latitude` property.
    Q_INVOKABLE void requestLatitude();
signals:
    void stateChanged();
    void catalogChanged();
    void settingsChanged();
    void frameChanged();
    void compareEnabledChanged();
    void histogramChanged();
    void latitudeChanged();
    void sourceOpened();
    void libraryChanged();
    void geometryChanged();
    void historyChanged();
    void initialized(bool success);
    void operationFinished(const QString& operation, bool success);
private:
    struct Reply {
        quint64 generation=0;
        quint64 settings_revision=0;
        QString operation, error;
        QString message;
        int library_index=-1;
        spk::desktop::output::Geometry geometry;
        spk::desktop::FramePtr frame;
        spk::desktop::Catalog catalog;
        // Only the open reply carries one: it installs the opened photo's
        // stored grade before the graded publication below.
        spk::desktop::layer2::Adjustments layer2;
        // Only the latitude reply carries one: the engine's raw JSON, parsed
        // into the `latitude` property on accept.
        QString latitude_json;
    };
    using Job = std::function<Reply(spk::desktop::PreviewHost&)>;
    void workerLoop(std::filesystem::path resources);
    void deliver(Reply reply);
    void accept(Reply reply);
    void submit(Job job, QString status, QString operation);
    spk::desktop::RenderSettings settings() const;
    void restoreSettings();
    void loadSettings(const spk::desktop::RenderSettings&, spk::desktop::DecodeMode);
    void saveActiveSettings(bool recordHistory=false);
    void setGeometry(const spk::desktop::output::Geometry&,const QString& editKey={});
    spk::desktop::editing::State currentState() const;
    void restoreHistory(bool redo);
    void scheduleSave();
    void updateThumbnail();
    int addFile(const QUrl&);
    void openItem(int index);
    void changed(const QString& editKey={});
    void schedulePreview();
    // The shared write path behind the four WB invokables: pairs the two
    // decode fields, refuses the headroom decode, and skips no-op values.
    void setWbEdit(double kelvin, double tint, const QString& editKey);
    // Layer 2 fast path: grade the retained print without the engine.
    QImage gradedDisplayImage(const spk::desktop::FramePtr&, const spk::desktop::layer2::Adjustments&) const;
    void changedLayer2(const QString& editKey={});
    void scheduleLayer2();
    void publishFromFrame(const QString& status);
    // The before visible image: the ungraded print through the same geometry
    // path as image_. Recomputed on every publication while the comparison
    // is on and the grade differs from the ungraded print.
    void refreshBeforeImage();
    void computeHistogram();
    bool ready_=false, busy_=true, failed_=false;
    QString status_=QStringLiteral("正在启动渲染引擎…");
    QVariantList films_, papers_;
    spk::desktop::Catalog catalog_;
    int film_index_=0, paper_index_=0, decode_mode_=0;
    double exposure_ev_=0;
    spk::desktop::FramePtr frame_;
    QImage image_;
    QImage full_image_;
    QImage before_image_;
    bool compare_enabled_=false;
    QVariantList histogram_;
    QVariantMap latitude_;
    spk::desktop::output::Geometry geometry_;
    spk::desktop::RenderSettings extra_settings_;
    spk::desktop::layer2::Adjustments layer2_;
    quint64 layer2_revision_=0;
    bool layer2_pending_=false;
    struct LibraryItem {
        QUrl url;
        spk::desktop::RenderSettings settings;
        spk::desktop::DecodeMode mode=spk::desktop::DecodeMode::compatible16;
        spk::desktop::output::Geometry geometry;
        spk::desktop::layer2::Adjustments layer2;
        QImage thumbnail;
        QString error;
        bool selected=true;
        std::vector<spk::desktop::editing::State> undo,redo;
        bool pending_save=false,save_blocked=false,has_stored=false;
        QString save_warning;
    };
    static spk::desktop::editing::State itemState(const LibraryItem&);
    void recordEdit(LibraryItem&,const spk::desktop::editing::State& before);
    std::vector<LibraryItem> library_;
    int active_index_=-1;
    std::filesystem::path resources_;
    spk::desktop::editing::Store edit_store_;
    QTimer save_timer_;
    int gesture_index_=-1;
    QString gesture_key_;
    bool gesture_recorded_=false;
    std::vector<spk::desktop::editing::State> gesture_redo_;
    int batch_progress_=0, batch_total_=0;
    std::shared_ptr<std::atomic_bool> cancel_batch_;
    ImageFactory image_factory_;
    quint64 generation_=0;
    quint64 settings_revision_=0;
    QString active_operation_;
    QTimer auto_apply_;
    bool dialog_open_=false;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Job> jobs_;
    bool stopping_=false;
    std::thread worker_;
};
