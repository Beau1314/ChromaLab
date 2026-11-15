// Actual RAW/Vulkan controller integration, with owned fixtures and a private
// edit store. This exercises user-visible edit history and reopened documents.
#include "PreviewController.hpp"
#include "EditStore.hpp"
#include "FrameCanvas.hpp"
#include "FrameOutput.hpp"
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include <QTransform>
#include <array>
#include <cmath>
#include <cstring>
#include <functional>
#include <iostream>
#include <stdexcept>

namespace {
namespace output=spk::desktop::output;
namespace editing=spk::desktop::editing;
QJsonArray checks,operations;
void check(bool passed,const QString& name) {
    checks.append(QJsonObject{{"name",name},{"passed",passed}});
    if(!passed)throw std::runtime_error(name.toUtf8().toStdString());
}
bool near(double a,double b){return std::abs(a-b)<1e-10;}
std::filesystem::path nativePath(const QString& value){return std::filesystem::path(value.toStdWString());}
void waitFor(const std::function<bool()>& condition,const QString& description) {
    if(condition())return;
    QEventLoop loop;QTimer poll,deadline;
    poll.setInterval(10);deadline.setSingleShot(true);
    QObject::connect(&poll,&QTimer::timeout,&loop,[&]{if(condition())loop.quit();});
    QObject::connect(&deadline,&QTimer::timeout,&loop,&QEventLoop::quit);
    poll.start();deadline.start(180000);loop.exec();
    check(condition(),"Completed: "+description);
}
void ready(PreviewController& c) {
    waitFor([&]{return c.ready()||c.failed();},"engine initialization");
    check(c.ready(),"Engine initializes: "+c.status());
    QObject::connect(&c,&PreviewController::operationFinished,&c,[&c](const QString& name,bool success){
        operations.append(QJsonObject{{"operation",name},{"success",success},{"status",c.status()}});
    });
}
void idle(PreviewController& c,const QString& name,bool allowFailure=false) {
    waitFor([&]{return !c.busy()&&!c.dirty();},name);
    check(allowFailure||!c.failed(),name+": "+c.status());
}
output::Geometry geometry(const PreviewController& c) {
    return {c.cropRect(),c.quarterTurns(),c.flipHorizontal(),c.flipVertical(),c.straightenDegrees()};
}
struct Desired {
    int film=0,paper=0,decode=0;
    double exposure=0;
    QVariantMap adjustments;
    output::Geometry geometry;
    double wbTemperature=0,wbTint=0;
    bool autoExposure=true;
    bool diActive=false,diBlueCompensation=false;
    bool sceneLatitudeActive=false;
    bool maskActive=false;
    double maskHighlights=0,maskShadows=0,maskCore=1,maskScale=0.03;
    bool scanHdrOn=false;
    double scanHdrHeadroom=0;
    // The date back rides the same saved settings; the checkpoint below
    // leaves a real date on it so reopening must restore the whole group.
    bool dateOn=false;
    QString dateText,dateDataText,dateStyle,dateCorner;
    double dateEv=3.5,dateSize=1.0,dateInsetX=3.0,dateInsetY=2.4;
};
Desired desired(const PreviewController& c) {
    return {c.filmIndex(),c.paperIndex(),c.decodeMode(),c.exposureEv(),c.adjustments(),geometry(c),
        c.wbTemperature(),c.wbTint(),c.autoExposure(),
        c.di()["active"].toBool(),c.di()["blueCompensation"].toBool(),
        c.sceneLatitudeActive(),
        c.mask()["active"].toBool(),
        c.mask()["highlights"].toDouble(),c.mask()["shadows"].toDouble(),
        c.mask()["core"].toDouble(),c.mask()["scale"].toDouble(),
        c.scanHdr()["on"].toBool(),c.scanHdr()["headroom"].toDouble(),
        c.dateImprint()["on"].toBool(),
        c.dateImprint()["text"].toString(),c.dateImprint()["dataText"].toString(),
        c.dateImprint()["style"].toString(),c.dateImprint()["corner"].toString(),
        c.dateImprint()["ev"].toDouble(),c.dateImprint()["size"].toDouble(),
        c.dateImprint()["insetX"].toDouble(),c.dateImprint()["insetY"].toDouble()};
}
bool matches(const PreviewController& c,const Desired& expected) {
    return c.filmIndex()==expected.film&&c.paperIndex()==expected.paper&&c.decodeMode()==expected.decode&&
        near(c.exposureEv(),expected.exposure)&&c.adjustments()==expected.adjustments&&geometry(c)==expected.geometry&&
        near(c.wbTemperature(),expected.wbTemperature)&&near(c.wbTint(),expected.wbTint)&&
        c.autoExposure()==expected.autoExposure&&
        c.di()["active"].toBool()==expected.diActive&&
        c.di()["blueCompensation"].toBool()==expected.diBlueCompensation&&
        c.sceneLatitudeActive()==expected.sceneLatitudeActive&&
        c.mask()["active"].toBool()==expected.maskActive&&
        near(c.mask()["highlights"].toDouble(),expected.maskHighlights)&&
        near(c.mask()["shadows"].toDouble(),expected.maskShadows)&&
        near(c.mask()["core"].toDouble(),expected.maskCore)&&
        near(c.mask()["scale"].toDouble(),expected.maskScale)&&
        c.scanHdr()["on"].toBool()==expected.scanHdrOn&&
        near(c.scanHdr()["headroom"].toDouble(),expected.scanHdrHeadroom)&&
        c.dateImprint()["on"].toBool()==expected.dateOn&&
        c.dateImprint()["text"].toString()==expected.dateText&&
        c.dateImprint()["dataText"].toString()==expected.dateDataText&&
        c.dateImprint()["style"].toString()==expected.dateStyle&&
        c.dateImprint()["corner"].toString()==expected.dateCorner&&
        near(c.dateImprint()["ev"].toDouble(),expected.dateEv)&&
        near(c.dateImprint()["size"].toDouble(),expected.dateSize)&&
        near(c.dateImprint()["insetX"].toDouble(),expected.dateInsetX)&&
        near(c.dateImprint()["insetY"].toDouble(),expected.dateInsetY);
}
bool samePixels(const QImage& left,const QImage& right) {
    if(left.isNull()||right.isNull()||left.size()!=right.size())return false;
    const auto a=left.convertToFormat(QImage::Format_RGBA8888),b=right.convertToFormat(QImage::Format_RGBA8888);
    for(int y=0;y<a.height();++y)
        if(std::memcmp(a.constScanLine(y),b.constScanLine(y),std::size_t(a.width())*4)!=0)return false;
    return true;
}
bool closePixels(const QImage& left,const QImage& right,int tolerance) {
    if(left.isNull()||right.isNull()||left.size()!=right.size())return false;
    const auto a=left.convertToFormat(QImage::Format_RGBA8888),b=right.convertToFormat(QImage::Format_RGBA8888);
    for(int y=0;y<a.height();++y) {
        const auto* pa=a.constScanLine(y);const auto* pb=b.constScanLine(y);
        for(int x=0;x<a.width()*4;++x)
            if(std::abs(int(pa[x])-int(pb[x]))>tolerance)return false;
    }
    return true;
}
// Layer 2 publications also report operationFinished; only engine operations
// prove that the render pipeline ran.
int engineOperations() {
    int count=0;
    for(const auto& entry:operations)
        if(entry.toObject()["operation"].toString()!=QStringLiteral("layer2"))++count;
    return count;
}
// Task 7: the output histogram. The property is 4 nested lists of 256 ints
// ([0..3] = R/G/B/luma) over the published preview's pixels — geometry and
// grade applied — and its bins must equal a manual count of exactly the image
// the canvas shows. The luma weights and rounding are the desktop's
// histogram-core.ts (Math.round == lround for non-negative sums).
using Bins=std::array<std::array<int,256>,4>;
Bins manualBins(const QImage& image) {
    Bins bins{};
    for(int y=0;y<image.height();++y) {
        const auto* line=reinterpret_cast<const QRgb*>(image.constScanLine(y));
        for(int x=0;x<image.width();++x) {
            const QRgb p=line[x];
            const int r=qRed(p),g=qGreen(p),b=qBlue(p);
            ++bins[0][r];++bins[1][g];++bins[2][b];
            ++bins[3][int(std::lround(0.2126*r+0.7152*g+0.0722*b))];
        }
    }
    return bins;
}
QVariantList channelOf(const QVariantList& histogram,int index) {
    return index>=0&&index<histogram.size()?histogram[index].toList():QVariantList{};
}
bool binsMatch(const QVariantList& histogram,const Bins& expected) {
    if(histogram.size()!=4)return false;
    for(int channel=0;channel<4;++channel) {
        const auto row=channelOf(histogram,channel);
        if(row.size()!=256)return false;
        for(int bin=0;bin<256;++bin)
            if(row[bin].toInt()!=expected[channel][bin])return false;
    }
    return true;
}
bool sameHistogram(const QVariantList& left,const QVariantList& right) {
    if(left.size()!=right.size())return false;
    for(int channel=0;channel<left.size();++channel)
        if(channelOf(left,channel)!=channelOf(right,channel))return false;
    return true;
}
qint64 histogramTotal(const QVariantList& histogram) {
    qint64 sum=0;
    for(const auto& bin:channelOf(histogram,0))sum+=bin.toLongLong();
    return sum;
}
QByteArray bytes(const QString& path) {
    QFile file(path);check(file.open(QIODevice::ReadOnly),"Read file: "+QFileInfo(path).fileName());return file.readAll();
}
QImage readImage(const QString& path) {
    QImageReader reader(path);auto image=reader.read();
    check(!image.isNull(),"Read exported image: "+reader.errorString());return image;
}
void writeNew(const QString& path,const QByteArray& data) {
    QFile file(path);
    if(!file.open(QIODevice::WriteOnly|QIODevice::NewOnly)||file.write(data)!=data.size()||!file.flush())
        throw std::runtime_error("Cannot write complete report without replacement");
}
void checkPhoto(const PreviewController& c,int index,const QString& source) {
    check(c.activeIndex()==index&&c.frame()&&
          QFileInfo(QString::fromStdWString(c.frame()->source.wstring())).canonicalFilePath()==QFileInfo(source).canonicalFilePath(),
          "Active index and displayed source agree");
}
}

int main(int argc,char** argv) {
    qputenv("QT_QPA_PLATFORM","offscreen");
    QGuiApplication application(argc,argv);
    QString destination;QElapsedTimer elapsed;elapsed.start();QJsonObject report;
    try {
        const auto args=application.arguments();
        if(args.size()!=5)throw std::runtime_error("usage: spk_editing_workflow_test <resources> <RAW> <rejected-RAW> <new-output-directory>");
        const auto resources=nativePath(args[1]);const QFileInfo original(args[2]),rejected(args[3]);
        destination=QFileInfo(args[4]).absoluteFilePath();
        check(original.isFile()&&rejected.isFile(),"Both input fixtures exist");
        check(!QFileInfo::exists(destination),"Output directory is new");
        check(QDir().mkpath(destination+"/fixtures"),"Create owned fixture directory");
        const QString first=destination+"/fixtures/alpha."+original.suffix();
        const QString second=destination+"/fixtures/beta."+original.suffix();
        const QString unsupported=destination+"/fixtures/rejected."+rejected.suffix();
        check(QFile::copy(original.absoluteFilePath(),first)&&QFile::copy(original.absoluteFilePath(),second)&&
              QFile::copy(rejected.absoluteFilePath(),unsupported),"Copy all fixtures before opening");
        const auto firstUrl=QUrl::fromLocalFile(first),secondUrl=QUrl::fromLocalFile(second);
        const QString editDirectory=destination+"/edits";
        Desired savedFirst;const QRectF secondCrop(.2,.1,.1,.12);
        {
            PreviewController c(resources,nullptr,{},editDirectory);ready(c);
            check(binsMatch(c.histogram(),Bins{}),"No frame publishes four all-zero channels");
            c.openFiles({firstUrl,secondUrl});idle(c,"Initial multiple RAW import");checkPhoto(c,0,first);
            check(!c.canUndo()&&!c.canRedo(),"Imported photographs begin with empty history");

            c.beginEditGesture("exposureEv");c.setExposureEv(.2);c.setExposureEv(.4);c.setExposureEv(.6);c.endEditGesture();
            check(c.canUndo()&&near(c.exposureEv(),.6),"Grouped slider retains final value");
            c.undo();check(near(c.exposureEv(),0)&&!c.canUndo()&&c.canRedo(),"One undo restores whole slider gesture");
            c.redo();check(near(c.exposureEv(),.6)&&c.canUndo()&&!c.canRedo(),"One redo restores whole slider gesture");
            c.undo();c.setExposureEv(.3);
            check(near(c.exposureEv(),.3)&&!c.canRedo(),"Diverging edit retires redo branch");
            c.undo();check(c.canRedo()&&near(c.exposureEv(),0),"Prepare redo branch for zero-net drag");
            c.beginEditGesture("exposureEv");c.setExposureEv(.2);c.setExposureEv(0);c.endEditGesture();
            check(!c.canUndo()&&c.canRedo(),"Zero-net slider drag preserves prior redo history");
            c.redo();check(near(c.exposureEv(),.3),"Preserved redo still restores its value");

            c.beginEditGesture("exposureEv");c.setExposureEv(.4);c.setAdjustment("grainActive",false);c.endEditGesture();
            c.undo();check(near(c.exposureEv(),.4)&&c.adjustments()["grainActive"].toBool(),"Toggle is a separate undo step from active slider");
            c.undo();check(near(c.exposureEv(),.3),"Slider still has its own undo step after toggle");
            c.redo();c.redo();
            check(near(c.exposureEv(),.4)&&!c.adjustments()["grainActive"].toBool(),"Separate slider and toggle redo in order");
            c.setAdjustment("glareActive",false);
            const auto latest=desired(c);int publications=0;
            auto publicationConnection=QObject::connect(&c,&PreviewController::frameChanged,&c,[&]{++publications;});
            c.setExposureEv(1.1);c.apply();check(c.busy(),"Explicit apply starts an in-flight render");
            c.undo();check(near(c.exposureEv(),.4),"Undo applies while older render is in flight");
            c.redo();check(near(c.exposureEv(),1.1),"Redo applies while older render is in flight");
            c.undo();idle(c,"Newest history state wins in-flight render");
            check(matches(c,latest)&&near(c.frame()->settings.print_exposure,std::exp2(-.4)),"Displayed engine parameters match newest undo state");
            check(publications==1,"Superseded render never publishes a stale frame");
            QObject::disconnect(publicationConnection);

            {
                const auto retained=c.frame();const int operationsBefore=operations.size();
                c.setCropRect(QRectF(.13,.17,.11,.15));c.setFlipHorizontal(true);
                const auto mirrored=c.previewImage().copy();
                // Geometry republishes pixels without the engine; the histogram
                // follows the published crop, so each channel sums to exactly
                // the new crop's pixel count (and cannot equal the old total).
                check(histogramTotal(c.histogram())==qint64(mirrored.width())*mirrored.height(),
                      "Crop republishes a histogram whose bins sum to the new pixel count");
                c.rotateClockwise();
                check(samePixels(c.previewImage(),mirrored.transformed(QTransform().rotate(90),Qt::FastTransformation))&&
                      !c.flipHorizontal()&&c.flipVertical(),"Clockwise rotation stays visually clockwise after a mirror");
                c.rotateCounterClockwise();
                check(samePixels(c.previewImage(),mirrored)&&c.flipHorizontal()&&!c.flipVertical()&&c.frame()==retained,
                      "Counterclockwise rotation restores mirrored pixels and final-axis flip flags");
                c.setFlipHorizontal(false);c.rotateClockwise();c.setFlipHorizontal(true);c.setFlipVertical(true);c.setStraightenDegrees(6.5);
                const auto transformed=geometry(c);const auto shown=c.previewImage().copy();
                check(c.frame()==retained&&!c.busy()&&operations.size()==operationsBefore,"Crop rotation flips and straighten do not rerender film");
                check(samePixels(shown,output::displayFrame(retained,transformed)),"Preview uses shared full-precision geometry output");
                c.undo();check(near(c.straightenDegrees(),0)&&c.frame()==retained,"Undo geometry changes no engine frame");
                c.redo();check(geometry(c)==transformed&&samePixels(c.previewImage(),shown),"Redo geometry restores exact displayed pixels");
                const QString png=destination+"/transformed.png";
                c.exportImage(QUrl::fromLocalFile(png),1,95,0);idle(c,"Export transformed PNG8");
                check(samePixels(readImage(png),shown),"Actual PNG8 export matches transformed preview pixel for pixel");
                check(c.frame()==retained,"Geometry export retains the editor frame");

                // A parameter's pending debounce must not begin a render and
                // disable the straighten slider while the pointer is held.
                const int beforeHeldGesture=operations.size();
                c.setExposureEv(.55);c.beginEditGesture("straightenDegrees");
                QEventLoop heldGesture;QTimer::singleShot(300,&heldGesture,&QEventLoop::quit);heldGesture.exec();
                check(!c.busy()&&c.dirty()&&operations.size()==beforeHeldGesture,
                      "Holding straighten pauses a pending parameter render");
                c.setStraightenDegrees(7.5);c.endEditGesture();
                idle(c,"Release straighten resumes pending parameter render");
                check(near(c.straightenDegrees(),7.5)&&near(c.frame()->settings.print_exposure,std::exp2(-.55)),
                      "Released straighten and pending exposure both survive rendering");
                c.setStraightenDegrees(6.5);c.setExposureEv(.4);idle(c,"Restore geometry checkpoint parameters");
            }

            // Layer 2 rides the fast path: grades republish the retained print
            // without the engine, share the undo history, and export like any
            // other edit family.
            {
                const auto retained=c.frame();const int engineOps=engineOperations();
                const auto neutral=c.previewImage().copy();
                // The histogram describes the published pixels, so its bins
                // must match a manual count of exactly this image.
                check(binsMatch(c.histogram(),manualBins(neutral)),
                      "Published histogram bins match a manual count of the preview");
                const auto neutralHistogram=c.histogram();
                c.beginEditGesture("exposure");c.setLayer2("exposure",1.0);c.endEditGesture();
                idle(c,"Layer 2 fast path publication");
                check(c.frame()==retained&&engineOperations()==engineOps&&!c.dirty(),
                      "Layer 2 edit republished the print without an engine render");
                check(!samePixels(c.previewImage(),neutral),"Layer 2 exposure visibly grades the canvas");
                const auto graded=c.previewImage().copy();
                check(!sameHistogram(c.histogram(),neutralHistogram),
                      "A layer 2 exposure changes the histogram");
                const auto gradedHistogram=c.histogram();
                c.undo();
                check(!c.busy()&&c.frame()==retained&&samePixels(c.previewImage(),neutral),
                      "Undo of a layer 2 grade restores the neutral canvas without the engine");
                check(sameHistogram(c.histogram(),neutralHistogram),
                      "Undo restores the pre-edit histogram");
                // The export captures the grade at submit time: the neutral
                // file must be written while the grade is still undone.
                const QString neutralPng=destination+"/layer2-neutral.png";
                c.exportImage(QUrl::fromLocalFile(neutralPng),1,95,0);idle(c,"Export the neutral grade");
                check(c.frame()==retained,"Neutral export retains the editor frame");
                c.redo();
                check(!c.busy()&&samePixels(c.previewImage(),graded),"Redo restores the graded canvas");
                check(sameHistogram(c.histogram(),gradedHistogram),
                      "Redo restores the graded histogram");
                // Split compare (Task 6): the before half is the current print
                // without Layer 2 through the same geometry path, and the
                // canvas composite copies whole rows at the split column.
                // These checks run while the exposure grade is on.
                {
                    const auto after=c.previewImage().copy();
                    check(!c.compareEnabled()&&c.beforeImage().isNull(),"Compare defaults to off with no before print");
                    c.setCompareEnabled(true);
                    check(c.compareEnabled()&&!c.beforeImage().isNull(),"Enabling compare publishes the ungraded print");
                    check(c.beforeImage().size()==after.size(),"Before and after prints share the display geometry");
                    check(!samePixels(c.beforeImage(),after),"The published before print differs wherever the grade acts");
                    // The before print must walk the same geometry path as the
                    // after print — only the grade differs — so it has to match
                    // the neutral visible canvas pixel for pixel.
                    check(samePixels(c.beforeImage(),neutral),"compare before matches the neutral visible canvas");
                    const auto before=c.beforeImage().copy();
                    // comparePosition=0.25 → the composite's left quarter is
                    // the before print, its right three quarters the graded one.
                    const auto split=FrameCanvas::composited(after,before,true,.25);
                    const int splitPx=int(std::lround(.25*after.width()));
                    bool leftIsBefore=true,rightIsAfter=true;
                    for(int y=0;y<split.height()&&(leftIsBefore||rightIsAfter);++y) {
                        if(std::memcmp(split.constScanLine(y),before.constScanLine(y),std::size_t(splitPx)*4)!=0)leftIsBefore=false;
                        if(std::memcmp(split.constScanLine(y)+std::size_t(splitPx)*4,
                                      after.constScanLine(y)+std::size_t(splitPx)*4,
                                      std::size_t(split.width()-splitPx)*4)!=0)rightIsAfter=false;
                    }
                    check(leftIsBefore&&rightIsAfter,"Composite splits whole rows: before left of the line, after right of it");
                    check(split!=after,"A differing grade makes the composite differ from the after-only canvas");
                    // A neutral grade clears the before print, so enabling the
                    // comparison can never change displayed pixels.
                    c.undo();
                    check(!c.busy()&&c.beforeImage().isNull(),"A neutral grade publishes no before print");
                    check(samePixels(FrameCanvas::composited(c.previewImage(),c.beforeImage(),c.compareEnabled(),.25),neutral),
                          "Compare with a neutral grade leaves the canvas pixels untouched");
                    c.redo();
                    check(!c.busy()&&!c.beforeImage().isNull()&&samePixels(c.beforeImage(),before),
                          "A layer 2 change republishes the before print");
                    // Closing the comparison must leave exactly the canvas a
                    // never-compared canvas would show, and a size mismatch
                    // degrades to the after print on its own.
                    c.setCompareEnabled(false);
                    check(c.beforeImage().isNull(),"Closing compare clears the before print");
                    check(samePixels(FrameCanvas::composited(after,before,false,.25),after),
                          "Closed compare leaves the after-only canvas");
                    const QImage mismatched(after.width(),after.height()+1,after.format());
                    check(FrameCanvas::composited(after,mismatched,true,.25)==after,
                          "Size-mismatched before print falls back to the after-only canvas");
                }
                const QString gradedPng=destination+"/layer2-graded.png";
                c.exportImage(QUrl::fromLocalFile(gradedPng),1,95,0);idle(c,"Export the graded print");
                check(!samePixels(readImage(neutralPng),readImage(gradedPng)),"The grade changes exported pixels");
                check(closePixels(readImage(gradedPng),graded,2),"Graded export matches the visible canvas within 2 LSB");
                c.selectImage(1);idle(c,"Switch away from the graded photograph");
                check(near(c.layer2()["exposure"].toDouble(),0.0),"The untouched photograph keeps the neutral grade");
                c.selectImage(0);idle(c,"Return to the graded photograph");
                check(near(c.layer2()["exposure"].toDouble(),1.0)&&samePixels(c.previewImage(),graded),
                      "Returning restores the stored grade and its pixels");
            }

            // Scene latitude (Task 8): an on-demand, read-only analysis of
            // the open session. The reply must carry the measured medium and
            // the scene statistic, and the analysis must leave the displayed
            // print byte-identical -- the engine never renders on analysis,
            // and the controller publishes nothing but the parsed reply.
            {
                const auto retained=c.frame();const auto shown=c.previewImage().copy();
                const auto histogramBefore=c.histogram();
                check(c.latitude().isEmpty(),"Latitude analysis begins empty");
                c.requestLatitude();idle(c,"Scene latitude analysis");
                check(!c.failed()&&!c.latitudeBusy(),"Latitude analysis completes without failure");
                const auto reply=c.latitude();
                check(reply.contains("medium")&&reply.contains("scene"),"Latitude reply carries medium and scene");
                const auto medium=reply["medium"].toMap();
                check(!medium.isEmpty()&&medium["ramp_ev"].toList().size()>0&&
                      medium["ramp_ev"].toList().size()==medium["ramp_y"].toList().size(),
                      "Latitude medium carries matched ramp arrays");
                const auto scene=reply["scene"].toMap();
                check(!scene.isEmpty()&&scene["histogram"].toMap().contains("fractions"),
                      "Latitude scene carries its stop histogram");
                check(c.frame()==retained&&samePixels(c.previewImage(),shown),
                      "Latitude analysis leaves the displayed frame unchanged");
                check(sameHistogram(c.histogram(),histogramBefore),
                      "Latitude analysis leaves the published histogram unchanged");
            }

            // White balance (Task 9): a decode-side Temperature/Tint override.
        // Changing it re-opens the photo transactionally (decode with the new
        // multipliers + render in one job, geometry and grade retained); As
        // Shot backfills the decoder-reported as-shot numbers as the edited
        // value and must stay a pixel-level no-op; undo restores across
        // re-decodes, not just re-renders.
        {
            const auto retained=c.frame();const auto shown=c.previewImage().copy();
            const int engineOps=engineOperations();
            check(near(c.wbTemperature(),0)&&near(c.wbTint(),0)&&c.wbAsShotAvailable(),
                  "White balance begins at As Shot with a decoder-reported illuminant");
            c.setWb(5500,10);idle(c,"White balance re-decode");
            check(c.frame()!=retained&&engineOperations()>engineOps,
                  "A white-balance edit re-decodes into a new frame through a decode+render job");
            check(near(c.wbTemperature(),5500)&&near(c.wbTint(),10),
                  "White-balance controls keep the edited numbers");
            check(near(c.frame()->settings.wb_temperature_k,5500)&&near(c.frame()->settings.wb_tint,10),
                  "The frame records the applied white balance");
            check(!samePixels(c.previewImage(),shown),"A daylight-ish override visibly shifts the decode");
            c.applyWbAsShot();idle(c,"As Shot backfill re-decode");
            const double asShotK=c.wbTemperature();
            check(asShotK>0&&near(asShotK,c.frame()->metadata.as_shot_temperature_k)&&
                  near(c.wbTint(),c.frame()->metadata.as_shot_tint),
                  "As Shot backfilled the decoder-reported non-zero illuminant");
            check(closePixels(c.previewImage(),shown,2),
                  "Decoding the backfilled As Shot numbers stays a pixel-level no-op");
            c.applyWbPreset("tungsten");idle(c,"Preset re-decode");
            check(near(c.wbTemperature(),3200)&&near(c.wbTint(),0),"The tungsten preset applies its numbers");
            check(!samePixels(c.previewImage(),shown),"The tungsten preset visibly shifts the decode");
            c.undo();c.undo();c.undo();idle(c,"White-balance undo re-decodes back");
            check(near(c.wbTemperature(),0)&&near(c.wbTint(),0)&&samePixels(c.previewImage(),shown),
                  "Three undos restore As Shot and the original decode pixel for pixel");
            c.applyWbPreset("flash");idle(c,"Flash preset applied for the persistence checkpoint");
            check(near(c.wbTemperature(),5500)&&near(c.wbTint(),10),"The flash preset applies its numbers");

            // 测光 (camera.auto_exposure) is an engine parameter, unlike the WB
            // pair: toggling it re-renders without any decode work.
            {
                const auto metered=c.frame();
                c.setAutoExposure(false);idle(c,"Metering off re-render");
                check(c.frame()!=metered&&c.frame()->settings.auto_exposure==false&&
                      c.frame()->timings.decode_ms==0,
                      "Toggling the meter re-renders without re-decoding");
                c.setAutoExposure(true);idle(c,"Metering restored");
                check(c.frame()->settings.auto_exposure==true,"The meter returns on");
            }

            // 数字中间片 (Cineon DI, RFC-028): also an engine parameter — the
            // toggle re-renders the session (spk_di_encode, ported in phase 2)
            // without decode work, the blue compensation is a second toggle on
            // top, and undo walks back across the re-renders. The whole-image
            // Cineon reference is a Mac-side fixture (pending), so this gate
            // pins the control path and that the encoded output differs from
            // the paper print.
            {
                const auto printed=c.frame();const auto printShown=c.previewImage().copy();
                check(!c.di()["active"].toBool()&&!c.di()["blueCompensation"].toBool(),
                      "The digital intermediate begins off");
                c.setDi("active",true);idle(c,"DI render");
                check(c.frame()!=printed&&c.frame()->settings.di_active&&
                      c.frame()->timings.decode_ms==0,
                      "Enabling the DI re-renders without re-decoding");
                check(!samePixels(c.previewImage(),printShown),
                      "The DI encodes the print as Cineon log instead of the paper");
                c.setDi("blueCompensation",true);idle(c,"DI blue compensation render");
                check(c.frame()->settings.di_active&&c.frame()->settings.di_blue_compensation&&
                      c.frame()->timings.decode_ms==0,
                      "Blue compensation re-renders as an engine parameter");
                c.undo();idle(c,"DI undo re-render");
                check(!c.frame()->settings.di_blue_compensation&&c.frame()->settings.di_active,
                      "Undo removes the blue compensation but keeps the DI");
                c.redo();idle(c,"DI redo re-render");
                check(c.frame()->settings.di_blue_compensation,"Redo restores the blue compensation");
                c.setDi("blueCompensation",false);idle(c,"Blue compensation off render");
                c.setDi("active",false);idle(c,"DI off re-render");
                check(!c.frame()->settings.di_active&&!c.frame()->settings.di_blue_compensation,
                      "The DI returns off");
                check(closePixels(c.previewImage(),printShown,2),
                      "Without the DI the print returns to the paper pixels");
            }

            // 对比遮罩 (RFC-024, phase 2 port): an engine parameter like the
            // DI — the toggle and the four scalars re-render the session
            // (spk_mask_reduce + spk_mask_epilogue on Vulkan) without decode
            // work. With both amounts at zero the mask is not wanted, so the
            // print stays byte-identical; amounts on, the print visibly
            // changes; undo walks back across the re-renders; turning the
            // mask off returns to the paper pixels.
            {
                const auto paper=c.frame();const auto paperShown=c.previewImage().copy();
                check(!c.mask()["active"].toBool(),"The contrast mask begins off");
                c.setMask("active",true);idle(c,"Mask toggle render");
                check(c.frame()!=paper&&c.frame()->settings.contrast_mask_active&&
                      c.frame()->timings.decode_ms==0,
                      "Enabling the mask re-renders without re-decoding");
                check(samePixels(c.previewImage(),paperShown),
                      "The mask with no amounts leaves the print untouched");
                c.setMask("highlights",1.5);idle(c,"Mask highlights render");
                check(near(c.frame()->settings.contrast_mask_highlights,1.5),
                      "The highlight amount lands as an engine parameter");
                check(!samePixels(c.previewImage(),paperShown),
                      "The mask's amounts visibly lift the print highlights");
                c.undo();idle(c,"Mask highlights undo re-render");
                check(near(c.frame()->settings.contrast_mask_highlights,0.0),
                      "Undo removes the highlight amount but keeps the mask on");
                check(samePixels(c.previewImage(),paperShown),
                      "Without the amount the print returns to the paper pixels");
                c.redo();idle(c,"Mask highlights redo re-render");
                check(near(c.frame()->settings.contrast_mask_highlights,1.5),
                      "Redo restores the highlight amount");
                c.setMask("active",false);idle(c,"Mask off re-render");
                check(!c.frame()->settings.contrast_mask_active,"The mask returns off");
                check(closePixels(c.previewImage(),paperShown,2),
                      "Without the mask the print returns to the paper pixels");
            }

            // HDR 扫描 (2026-11-06, phase 2 port): the slide's HDR scan is an
            // engine parameter like the DI and the mask — but only a positive
            // film scanned has it (the engine's own rule), so this block
            // switches to the catalog's first slide, walks the toggle and the
            // headroom through the re-render path (`spk_scan_hdr_expand` on
            // Vulkan, re-developing without decode work), and restores the
            // negative film afterwards. The headroom is a shoot-layer field:
            // every edit drops the cached negative and re-develops the film,
            // which re-draws the stochastic layers, so the walk pins grain,
            // halation and glare off first (PORT_PLAN §0's own rule for
            // whole-image checks) and the off-return is byte for byte — a
            // headroom of 0 builds today's pipeline.
            {
                const int negativeFilm=c.filmIndex();
                const bool grainWas=c.adjustments()["grainActive"].toBool(),
                    halationWas=c.adjustments()["halationActive"].toBool(),
                    glareWas=c.adjustments()["glareActive"].toBool();
                int slideFilm=-1;
                for(int i=0;i<c.films().size();++i)
                    if(c.films()[i].toMap()["positive"].toBool()){slideFilm=i;break;}
                check(slideFilm>=0,"Catalog provides a slide film for the HDR scan");
                c.setAdjustment("grainActive",false);c.setAdjustment("halationActive",false);
                c.setAdjustment("glareActive",false);
                c.setFilmIndex(slideFilm);idle(c,"Slide film render");
                const auto plain=c.frame();const auto plainShown=c.previewImage().copy();
                check(!c.scanHdr()["on"].toBool(),"The HDR scan begins off");
                c.setScanHdr("on",true);idle(c,"HDR scan toggle render");
                check(c.frame()!=plain&&c.frame()->settings.scan_hdr_headroom>1.0&&
                      c.frame()->timings.decode_ms==0,
                      "Enabling the HDR scan re-renders without re-decoding");
                check(near(c.frame()->settings.scan_hdr_headroom,4.0),
                      "The switch backfills the study's working headroom");
                check(!samePixels(c.previewImage(),plainShown),
                      "The HDR scan visibly expands the layers past their knees");
                c.setScanHdr("headroom",8.0);idle(c,"HDR headroom render");
                check(near(c.frame()->settings.scan_hdr_headroom,8.0),
                      "The headroom lands as an engine parameter");
                c.undo();idle(c,"HDR headroom undo re-render");
                check(near(c.frame()->settings.scan_hdr_headroom,4.0),
                      "Undo removes the headroom edit but keeps the scan on");
                c.redo();idle(c,"HDR headroom redo re-render");
                check(near(c.frame()->settings.scan_hdr_headroom,8.0),
                      "Redo restores the headroom edit");
                c.setScanHdr("on",false);idle(c,"HDR scan off render");
                check(!c.frame()->settings.scan_hdr_headroom,"The HDR scan returns off");
                check(samePixels(c.previewImage(),plainShown),
                      "Without the scan the print returns to today's pixels byte for byte");
                c.setFilmIndex(negativeFilm);idle(c,"Restore the negative film");
                c.setAdjustment("grainActive",grainWas);c.setAdjustment("halationActive",halationWas);
                c.setAdjustment("glareActive",glareWas);idle(c,"Stochastic layers restored");
            }

            // 胶片边缘 + 日期背印 (RFC-031/032, phase 2 port): the film edge
            // and the date back are engine parameters like the DI and the
            // mask, through changed() (re-renders, never re-decodes; the
            // overscan fields are shoot-layer, so the film re-develops — the
            // walk pins grain, halation and glare off first for the byte-for-
            // byte off-return). The date alone works on any frame; the film
            // edge needs the photo to match its gate's shape within 5 %, so
            // the format is picked from the fixture's own aspect at run time
            // and the canvas walk is fixture-gated (a fixture that matches no
            // gate exercises the date half only).
            {
                const auto paper=c.frame();const auto paperShown=c.previewImage().copy();
                const bool grainWas=c.adjustments()["grainActive"].toBool(),
                    halationWas=c.adjustments()["halationActive"].toBool(),
                    glareWas=c.adjustments()["glareActive"].toBool();
                c.setAdjustment("grainActive",false);c.setAdjustment("halationActive",false);
                c.setAdjustment("glareActive",false);idle(c,"Stochastic layers off for the overscan walk");
                check(!c.dateImprint()["on"].toBool(),"The date back begins off");
                c.setDateImprint("text","'26 11 15");c.setDateImprint("on",true);idle(c,"Date back render");
                check(c.frame()!=paper&&c.frame()->settings.date_imprint_active&&
                      c.frame()->timings.decode_ms==0,
                      "Enabling the date back re-renders without re-decoding");
                check(!samePixels(c.previewImage(),paperShown),
                      "The date visibly lands on the negative");
                const auto lcdShown=c.previewImage().copy();
                c.setDateImprint("corner","tl");idle(c,"Date corner render");
                check(c.dateImprint()["corner"].toString()==QLatin1String("tl"),
                      "The corner lands as an engine parameter");
                check(!samePixels(c.previewImage(),lcdShown),
                      "Moving the corner moves the print to the new corner");
                const auto cornerShown=c.previewImage().copy();
                c.setDateImprint("dataText","1/125 F2.8 A 50mm");idle(c,"Date and data co-print render");
                check(c.dateImprint()["dataText"].toString()==QLatin1String("1/125 F2.8 A 50mm"),
                      "The shooting data lands as an engine parameter");
                check(!samePixels(c.previewImage(),cornerShown),
                      "The data line joins the date in the frame");
                c.undo();idle(c,"Data line undo re-render");
                check(c.dateImprint()["dataText"].toString().isEmpty()&&
                      samePixels(c.previewImage(),cornerShown),
                      "Undo removes the data line but keeps the date");
                c.redo();idle(c,"Data line redo re-render");
                check(c.dateImprint()["dataText"].toString()==QLatin1String("1/125 F2.8 A 50mm")&&
                      !samePixels(c.previewImage(),cornerShown),
                      "Redo restores the data line");
                c.setDateImprint("style","dots");idle(c,"Date dots face render");
                check(c.dateImprint()["style"].toString()==QLatin1String("dots"),
                      "The style lands as an engine parameter");
                const auto dotsShown=c.previewImage().copy();
                check(!samePixels(c.previewImage(),cornerShown),
                      "The dots face redraws the date as round cells");
                c.setDateImprint("style","data");idle(c,"Date data face render");
                check(c.dateImprint()["style"].toString()==QLatin1String("data"),
                      "The data face lands as an engine parameter");
                check(!samePixels(c.previewImage(),dotsShown),
                      "The data face prints the date as filled cells");
                c.undo();idle(c,"Data face undo re-render");
                check(c.dateImprint()["style"].toString()==QLatin1String("dots")&&
                      samePixels(c.previewImage(),dotsShown),
                      "Undo restores the dots face");
                c.redo();idle(c,"Data face redo re-render");
                check(c.dateImprint()["style"].toString()==QLatin1String("data")&&
                      !samePixels(c.previewImage(),dotsShown),
                      "Redo restores the data face");
                c.setDateImprint("on",false);c.setDateImprint("text","");c.setDateImprint("dataText","");
                c.setDateImprint("corner","br");c.setDateImprint("style","lcd");idle(c,"Date back off render");
                check(samePixels(c.previewImage(),paperShown),
                      "Without the date the print returns to the paper pixels byte for byte");
                const double aspect=double(paper->width())/double(paper->height());
                const std::vector<std::pair<const char*,double>> gates{
                    {"135",36.25/24.3},{"135_half",18.0/24.0},{"120_645",41.5/56.0},{"120_6x6",1.0},
                    {"120_6x7",69.5/56.0},{"120_6x8",76.0/56.0},{"120_6x9",84.0/56.0},
                    {"135_xpan",65.0/24.0},{"120_6x12",2.0},{"120_6x17",3.0}};
                QString match;
                for(const auto& g:gates)
                    if(std::fabs((aspect/g.second)-1.0)<=0.05){match=QLatin1String(g.first);break;}
                if(!match.isEmpty()) {
                    c.setOverscan("format",match);c.setOverscan("on",true);idle(c,"Film edge render");
                    check(c.frame()!=paper&&c.frame()->settings.overscan_active&&
                          c.frame()->timings.decode_ms==0,
                          "Enabling the film edge re-renders without re-decoding");
                    check(c.frame()->width()!=paper->width()||c.frame()->height()!=paper->height(),
                          "The film canvas replaces the frame's size");
                    c.setOverscan("frameNumber",7);idle(c,"Frame number render");
                    check(near(c.overscan()["frameNumber"].toDouble(),7.0),
                          "The frame number lands as an engine parameter");
                    c.undo();idle(c,"Frame number undo re-render");
                    check(near(c.overscan()["frameNumber"].toDouble(),0.0),
                          "Undo removes the frame number edit but keeps the edge on");
                    c.redo();idle(c,"Frame number redo re-render");
                    check(near(c.overscan()["frameNumber"].toDouble(),7.0),
                          "Redo restores the frame number edit");
                    c.setOverscan("on",false);c.setOverscan("format","135");idle(c,"Film edge off render");
                    check(!c.frame()->settings.overscan_active,"The film edge returns off");
                    check(samePixels(c.previewImage(),paperShown),
                          "Without the film edge the print returns to the paper pixels byte for byte");
                } else {
                    check(true,"No gate matches this fixture's shape: the film-edge canvas walk is fixture-gated");
                }
                c.setAdjustment("grainActive",grainWas);c.setAdjustment("halationActive",halationWas);
                c.setAdjustment("glareActive",glareWas);idle(c,"Stochastic layers restored");
            }

            // 宽容度映射 (RFC-023, phase 2 port): the analysis card's
            // engine side. The toggle is an engine parameter through changed()
            // (re-renders, never re-decodes); with nothing committed the
            // resolved curve has rooms of zero and the kernel is a structural
            // bypass, so the print is byte-identical. 提交拟合 writes the
            // analysis reply's fit.params_delta through the same ordinary
            // channel, and undo walks back across both. The committed scalars
            // stay in the settings afterwards so the persistence checkpoint
            // below carries a real fit through the edit store.
            {
                const auto paper=c.frame();const auto paperShown=c.previewImage().copy();
                check(!c.sceneLatitudeActive()&&!c.frame()->settings.scene_latitude_active,
                      "Scene latitude mapping begins off");
                c.requestLatitude();idle(c,"Latitude analysis");
                check(!c.latitude().isEmpty()&&c.latitude().contains("medium"),
                      "The analysis publishes its reply without rendering");
                c.setSceneLatitude(true);idle(c,"Mapping toggle render");
                check(c.frame()!=paper&&c.frame()->settings.scene_latitude_active&&
                      c.frame()->timings.decode_ms==0,
                      "Toggling the mapping re-renders without re-decoding");
                check(samePixels(c.previewImage(),paperShown),
                      "The default curve (no rooms) leaves the print untouched");
                c.commitLatitudeFit();idle(c,"Fit commit render");
                check(c.frame()!=paper&&c.frame()->settings.scene_latitude_active&&
                      c.frame()->settings.scene_latitude_highlight_room>0.0&&
                      c.frame()->settings.scene_latitude_shadow_room>0.0,
                      "The committed fit carries the solved pull-backs");
                check(!samePixels(c.previewImage(),paperShown),
                      "The solved mapping places the scene on the medium");
                c.undo();idle(c,"Fit commit undo re-render");
                check(c.frame()->settings.scene_latitude_active&&
                      c.frame()->settings.scene_latitude_highlight_room==0.0,
                      "Undo removes the committed curve but keeps the mapping");
                check(samePixels(c.previewImage(),paperShown),
                      "Without the committed curve the print returns to the paper pixels");
                c.redo();idle(c,"Fit commit redo re-render");
                check(c.frame()->settings.scene_latitude_highlight_room>0.0,
                      "Redo restores the committed curve");
                c.setSceneLatitude(false);idle(c,"Mapping off render");
                check(!c.frame()->settings.scene_latitude_active&&
                      c.frame()->settings.scene_latitude_highlight_room>0.0,
                      "Turning the mapping off keeps the committed curve for the next toggle");
            }
        }

        c.selectImage(1);idle(c,"Select untouched second photograph");checkPhoto(c,1,second);
            check(!c.canUndo()&&!c.canRedo()&&near(c.exposureEv(),0)&&c.quarterTurns()==0,
                  "Second photograph has independent empty history and geometry");
            c.setCropRect(secondCrop);c.setExposureEv(-.7);
            c.selectImage(0);idle(c,"Restore first photograph with second edit pending");
            check(near(c.exposureEv(),.4)&&c.quarterTurns()==1&&c.canUndo(),"Photo switch restores first independent state and history");
            c.selectAllPhotos(true);c.syncSelectedSettings(false);
            c.selectImage(1);idle(c,"Open synchronized recipient");
            check(near(c.exposureEv(),.4)&&c.cropRect()==secondCrop&&c.quarterTurns()==0,"Synchronization retains recipient geometry");
            c.undo();check(near(c.exposureEv(),-.7)&&c.adjustments()["grainActive"].toBool()&&c.cropRect()==secondCrop&&
                           near(c.wbTemperature(),0),
                           "Recipient independently undoes synchronized parameters");
            check(c.libraryItems()[1].toMap()["thumbnail"].value<QImage>().isNull(),"Pending history render does not republish old thumbnail");
            c.redo();check(near(c.exposureEv(),.4)&&!c.adjustments()["grainActive"].toBool()&&
                           near(c.wbTemperature(),5500)&&near(c.wbTint(),10),"Recipient can redo synchronized parameters");
            c.undo();c.selectImage(0);idle(c,"Return to first before persistence checkpoint");

            // 半幅对 (Task 3.2, PAIR-WIRING §5): two photographs become one
            // pair library item — the first compose and open, one hole's own
            // exposure and placement (the meter cache holds across a drag,
            // its key excluding the placement), the pair's own undo stacks,
            // the record file and the reopen restore, and a wire-only hole
            // edit (the date) taking the reprint path. The gate runs on real
            // RAW fixtures; without them this whole test never runs, so the
            // executable evidence for the record's own discipline stays in
            // spk_qt_edit_store (task-00 §2.5's fixture-gate convention).
            {
                const int pairIndex=c.libraryItems().size();
                check(c.createPair(firstUrl,secondUrl),"createPair accepts two photographs");
                waitFor([&]{return c.activeIndex()==pairIndex&&!c.busy()&&c.frame();},"pair first compose");
                check(!c.failed(),"Pair composes and opens: "+c.status());
                check(!c.pair().isEmpty()&&c.pair()["holes"].toList().size()==2,
                      "The pair property publishes two holes");
                check(near(c.pair()["spacingMM"].toDouble(),1.0)&&!c.pair()["turned"].toBool(),
                      "A new pair begins at the strip's spacing, level");
                check(near(c.pair()["split"].toDouble(),0.5),"The split begins at the gap's middle");
                // One hole's exposure re-composes the piece; the meter reads
                // that hole alone once and caches by its placement-excluding
                // key.
                c.setPairHole(1,"exposureEv",0.4);
                waitFor([&]{return !c.busy();},"hole exposure re-compose");
                check(!c.failed(),"Per-hole exposure re-composes: "+c.status());
                const QVariant metered=c.pair()["holes"].toList()[1].toMap()["meteredEv"];
                check(!metered.isNull(),"The second hole publishes its meter reading");
                // A placement drag re-composes without re-metering
                // (PAIR-WIRING §4.2): the reading does not pump.
                c.setPairHole(1,"x",0.05);
                waitFor([&]{return !c.busy();},"placement drag re-compose");
                check(!c.failed(),"A placement drag re-composes: "+c.status());
                check(c.pair()["holes"].toList()[1].toMap()["meteredEv"]==metered,
                      "A placement drag keeps the cached meter reading");
                c.undo();
                waitFor([&]{return !c.busy();},"pair undo re-compose");
                check(near(c.pair()["holes"].toList()[1].toMap()["exposureEv"].toDouble(),0.0)&&
                      near(c.pair()["holes"].toList()[1].toMap()["x"].toDouble(),0.0),
                      "Pair undo restores both the exposure and the placement");
                c.redo();
                waitFor([&]{return !c.busy();},"pair redo re-compose");
                check(near(c.pair()["holes"].toList()[1].toMap()["exposureEv"].toDouble(),0.4),
                      "Pair redo restores the exposure");
                // The record persists the pair; reopening the pair item
                // restores it (select away, flush, select back).
                c.flushEdits();
                const editing::Store pairStore(editDirectory);
                check(QFileInfo::exists(pairStore.pairFilePath(firstUrl,secondUrl)),
                      "The pair record file exists after a flush");
                c.selectImage(0);idle(c,"Select away from the pair");
                c.selectImage(pairIndex);
                waitFor([&]{return c.activeIndex()==pairIndex&&!c.busy();},"pair reopen");
                check(!c.failed(),"Pair reopens: "+c.status());
                check(near(c.pair()["holes"].toList()[1].toMap()["exposureEv"].toDouble(),0.4)&&
                      near(c.pair()["holes"].toList()[1].toMap()["x"].toDouble(),0.05),
                      "Reopening the pair restores the edited state");
                // A hole's date is a wire row, not pixels: the reprint path
                // of the same negative (no re-compose of the piece). The
                // frame's settings ARE the wire on both the open and the
                // reprint path, so the wire assertions read off them
                // directly (PAIR-WIRING §3): the bare pair sends neither
                // overscan_pair nor 135_half, and the dated bare pair's rows
                // ride the date's -- overscan_pair + 135_half with no film
                // edge (DateBackSettings.pairRows).
                const auto pieceFrame=c.frame();
                check(!c.frame()->settings.overscan_pair&&
                      c.frame()->settings.overscan_format=="135",
                      "A bare pair (no film edge, no date) sends neither overscan_pair nor 135_half");
                c.setPairHole(1,"dated",true);
                waitFor([&]{return !c.busy();},"hole dated switch reprint");
                check(!c.failed(),"The hole's dated switch takes the wire path: "+c.status());
                c.setPairHole(1,"dateText","'26 11 15");
                waitFor([&]{return !c.busy();},"hole date reprint");
                check(!c.failed()&&c.frame()!=nullptr&&c.frame()!=pieceFrame,
                      "The hole's date takes the wire reprint path: "+c.status());
                check(c.frame()->settings.overscan_pair&&
                      c.frame()->settings.overscan_format=="135_half"&&
                      c.frame()->settings.date_imprint_active&&
                      c.frame()->settings.date_imprint_text_b=="'26 11 15"&&
                      c.frame()->settings.date_imprint_text.empty(),
                      "A dated bare pair's wire carries overscan_pair, 135_half and the date rows");
                c.undo();
                waitFor([&]{return !c.busy();},"hole date undo");
                check(!c.failed()&&!c.frame()->settings.overscan_pair,
                      "The date edit undoes through the pair stack, back to the bare pair's wire: "+c.status());
                c.selectImage(0);idle(c,"Leave the pair for the persistence checkpoint");
            }

            // Leave every supported family changed, but deliberately do not
            // yield to the render/save timers. Destruction must flush wanted
            // edits rather than the older engine frame currently on screen.
            int alternativeFilm=-1;
            for(int i=0;i<c.films().size();++i)if(i!=c.filmIndex()&&!c.films()[i].toMap()["positive"].toBool()){alternativeFilm=i;break;}
            check(alternativeFilm>=0&&c.papers().size()>1,"Catalog provides independent persistence choices");
            c.setFilmIndex(alternativeFilm);c.setPaperIndex((c.paperIndex()+1)%c.papers().size());c.setDecodeMode(1);c.setExposureEv(-.35);
            c.setAdjustment("filmExposureEv",.65);c.setAdjustment("filmFormatMm",56.);
            c.setAdjustment("grainAmount",.45);c.setAdjustment("halationActive",false);c.setAdjustment("halationAmount",1.65);
            c.setAdjustment("glareAmount",.35);c.setAdjustment("yFilterShift",.05);c.setAdjustment("mFilterShift",-.07);
            c.setDi("active",true);
            // The latitude card's committed curve rides the same saved
            // settings: reopening must restore the mapping with its solved
            // pull-backs (the gate block above left them committed).
            c.setSceneLatitude(true);
            // The mask's curve rides the same saved settings: reopening must
            // restore the mask with its amounts (all five keys persisted).
            c.setMask("active",true);
            c.setMask("highlights",0.9);
            c.setMask("shadows",0.6);
            c.setMask("core",1.4);
            c.setMask("scale",0.05);
            // The date back rides the same record: reopening must restore the
            // whole eleven-key group (carry-one-carry-all), data line included.
            c.setDateImprint("on",true);
            c.setDateImprint("text","'26 11 20");
            c.setDateImprint("dataText","1/250 F4 ISO 100");
            c.setDateImprint("style","dots");
            c.setDateImprint("corner","bl");
            c.setDateImprint("ev",4.5);
            c.setDateImprint("size",1.5);
            c.setDateImprint("insetX",6.0);
            c.setDateImprint("insetY",5.0);
            c.rotateCounterClockwise();savedFirst=desired(c);
            check(c.dirty()&&!c.busy(),"Persistence checkpoint contains pending unrendered edits");
            check(c.persistenceWarning().isEmpty(),"No persistence warnings before destruction");
        }
        editing::Store store(editDirectory);
        check(QFileInfo::exists(store.filePath(firstUrl))&&QFileInfo::exists(store.filePath(secondUrl)),"Destruction saved both photographs in private store");
        check(QDir(destination+"/fixtures").entryList(QDir::Files|QDir::NoDotAndDotDot).size()==3,"No edit sidecars were written beside RAW fixtures");
        {
            PreviewController c(resources,nullptr,{},editDirectory);ready(c);
            c.openFiles({secondUrl,firstUrl});idle(c,"Recreated controller opens saved second photograph");
            checkPhoto(c,0,second);check(near(c.exposureEv(),-.7)&&c.cropRect()==secondCrop,"Second saved photograph restores independently");
            c.selectImage(1);idle(c,"Recreated controller restores first photograph");
            checkPhoto(c,1,first);
            check(matches(c,savedFirst),"All controls and geometry survive controller destruction and reopening");
            check(!c.canUndo()&&!c.canRedo(),"Restart restores edits without inventing undo history");
            check(c.persistenceWarning().isEmpty(),"Restored records have no persistence warnings");
            c.setExposureEv(.8);c.openRaw(secondUrl);idle(c,"Open known photo while another preset is pending");
            checkPhoto(c,0,second);
            check(near(c.exposureEv(),-.7)&&c.cropRect()==secondCrop,"Opening known photo does not inherit other photo preset");
            c.rotateClockwise();check(c.canUndo(),"Batch lock test begins with an actual undo step");
            c.selectAllPhotos(false);c.setItemSelected(0,true);
            const auto locked=desired(c);const auto retained=c.frame();
            const QString batch=destination+"/batch";check(QDir().mkpath(batch),"Create batch destination");
            c.exportBatch(QUrl::fromLocalFile(batch),1,95,96);
            check(c.batchExporting()&&!c.canUndo()&&!c.canRedo(),"Batch disables edit history");
            c.undo();c.redo();c.rotateClockwise();c.setStraightenDegrees(-3);c.resetGeometry();c.setExposureEv(.2);
            check(matches(c,locked)&&c.frame()==retained,"Batch rejects history geometry and parameter changes");
            idle(c,"Single selected batch export");
            check(c.canUndo()&&QFileInfo::exists(batch+"/beta.png"),"Batch releases history and writes selected image");
            const auto beforeRejected=desired(c);
            c.openRaw(QUrl::fromLocalFile(unsupported));idle(c,"Unsupported RAW rejected",true);
            check(c.failed()&&matches(c,beforeRejected)&&c.frame()==retained&&c.canUndo(),"Failed open preserves frame settings geometry and valid history");
        }

        // A newer application's record must not be silently replaced even
        // when this controller can still render and edit the original RAW.
        const QString record=store.filePath(firstUrl);
        auto newer=QJsonDocument::fromJson(bytes(record)).object();newer["version"]=99;
        const auto futureBytes=QJsonDocument(newer).toJson(QJsonDocument::Indented);
        {QFile file(record);check(file.open(QIODevice::WriteOnly|QIODevice::Truncate),"Modify only owned fixture record");
         check(file.write(futureBytes)==futureBytes.size()&&file.flush(),"Write future-version fixture");}
        {
            PreviewController c(resources,nullptr,{},editDirectory);ready(c);c.openRaw(firstUrl);idle(c,"Open RAW with future-version record");
            check(!c.persistenceWarning().isEmpty(),"Controller exposes blocked saved-edit warning");
            c.setExposureEv(.35);c.flushEdits();
            check(bytes(record)==futureBytes,"Explicit flush preserves unsupported future record");
        }
        check(bytes(record)==futureBytes,"Destructor flush also preserves future record");
        report["passed"]=true;
    } catch(const std::exception& error) {
        report["passed"]=false;report["error"]=QString::fromUtf8(error.what());
        std::cerr<<error.what()<<'\n';
    }
    report["checks"]=checks;report["checkCount"]=checks.size();report["operations"]=operations;
    report["elapsedMs"]=elapsed.elapsed();
    if(!destination.isEmpty()&&QFileInfo(destination).isDir()) {
        try {writeNew(destination+"/report.json",QJsonDocument(report).toJson(QJsonDocument::Indented));}
        catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
    }
    std::cout<<"editing workflow "<<(report["passed"].toBool()?"PASS":"FAIL")<<" checks="<<checks.size()<<'\n';
    return report["passed"].toBool()?0:1;
}
