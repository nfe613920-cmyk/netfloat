#include <QApplication>
#include <QCoreApplication>
#include <QContextMenuEvent>
#include <QDir>
#include <QElapsedTimer>
#include <QEnterEvent>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHash>
#include <QIcon>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLibrary>
#include <QLocale>
#include <QMenu>
#include <QMouseEvent>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>
#include <QResizeEvent>
#include <QSettings>
#include <QShowEvent>
#include <QSvgRenderer>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QUrl>

#ifdef NETFLOAT_KDE_BLUR
#include <KWindowEffects>
#endif

#include <X11/Xatom.h>
#include <X11/Xlib.h>

#include <algorithm>
#include <cmath>
#include <limits>

static QIcon loadLogo() {
    const QString path = QCoreApplication::applicationDirPath() + "/netfloat.svg";
    QSvgRenderer svg(path);
    if (!svg.isValid()) return QIcon::fromTheme("network-transmit-receive");
    QIcon icon;
    for (int size : {16, 22, 32, 48, 64, 128}) {
        QPixmap image(size, size);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        svg.render(&painter);
        icon.addPixmap(image);
    }
    return icon;
}

static QIcon alertLogo(const QIcon &base, const QColor &badgeColor) {
    QPixmap image = base.pixmap(48, 48);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(QColor(22, 22, 22), 3));
    painter.setBrush(badgeColor);
    painter.drawEllipse(QRect(29, 29, 17, 17));
    return QIcon(image);
}

static void skipTaskbar(WId windowId) {
    if (QGuiApplication::platformName() != "xcb") return;
    Display *display = XOpenDisplay(nullptr);
    if (!display) return;
    const Atom state = XInternAtom(display, "_NET_WM_STATE", False);
    const Atom taskbar = XInternAtom(display, "_NET_WM_STATE_SKIP_TASKBAR", False);
    const Atom pager = XInternAtom(display, "_NET_WM_STATE_SKIP_PAGER", False);
    XEvent event{};
    event.xclient.type = ClientMessage;
    event.xclient.window = static_cast<Window>(windowId);
    event.xclient.message_type = state;
    event.xclient.format = 32;
    event.xclient.data.l[0] = 1; // _NET_WM_STATE_ADD
    event.xclient.data.l[1] = static_cast<long>(taskbar);
    event.xclient.data.l[2] = static_cast<long>(pager);
    event.xclient.data.l[3] = 1; // source: application
    XSendEvent(display, DefaultRootWindow(display), False,
               SubstructureRedirectMask | SubstructureNotifyMask, &event);
    XFlush(display);
    XCloseDisplay(display);
}

struct GpuStats {
    int busy = -1;
    int temperature = -1;
    double watts = -1;
    quint64 used = 0;
    quint64 total = 0;
    bool memoryValid = false;
};

struct GpuDevice {
    QString id;
    QString label;
    QString sysfsPath;
    QString temperaturePath;
    QString powerPath;
    void *nvmlHandle = nullptr;
};

static bool readUnsigned(const QString &path, quint64 &value) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return false;
    bool ok = false;
    const quint64 number = file.readAll().trimmed().toULongLong(&ok, 0);
    if (ok) value = number;
    return ok;
}

static QString discoverCpuTemperature() {
    const QDir root("/sys/class/hwmon");
    for (const QString &entry : root.entryList({"hwmon*"}, QDir::Dirs | QDir::NoDotAndDotDot)) {
        const QDir chip(root.filePath(entry));
        QFile nameFile(chip.filePath("name"));
        if (!nameFile.open(QIODevice::ReadOnly)) continue;
        const QByteArray name = nameFile.readAll().trimmed();
        if (name == "coretemp" || name == "k10temp" || name == "zenpower" || name == "cpu_thermal") {
            const QString path = chip.filePath("temp1_input");
            if (QFileInfo::exists(path)) return path;
        }
    }
    return {};
}

class GpuMonitor {
public:
    GpuMonitor() : nvml_("libnvidia-ml.so.1") {
        discoverNvml();
        discoverSysfs();
    }

    ~GpuMonitor() {
        if (nvmlReady_ && shutdown_) shutdown_();
    }

    const QList<GpuDevice> &devices() const { return devices_; }

    GpuStats read(const GpuDevice &device) const {
        GpuStats stats;
        if (device.nvmlHandle) {
            NvmlUtilization utilization{};
            NvmlMemory memory{};
            unsigned int temperature = 0;
            unsigned int milliwatts = 0;
            if (getUtilization_(device.nvmlHandle, &utilization) == 0)
                stats.busy = std::min(utilization.gpu, 100u);
            if (getTemperature_ && getTemperature_(device.nvmlHandle, 0, &temperature) == 0 &&
                temperature <= 150)
                stats.temperature = static_cast<int>(temperature);
            if (getPower_ && getPower_(device.nvmlHandle, &milliwatts) == 0 &&
                milliwatts <= 1000000)
                stats.watts = milliwatts / 1000.0;
            if (getMemory_(device.nvmlHandle, &memory) == 0 && memory.total > 0) {
                stats.used = memory.used;
                stats.total = memory.total;
                stats.memoryValid = true;
            }
        } else if (!device.sysfsPath.isEmpty()) {
            quint64 busy = 0, used = 0, total = 0, millidegrees = 0, microwatts = 0;
            if (readUnsigned(device.sysfsPath + "/gpu_busy_percent", busy))
                stats.busy = static_cast<int>(std::min(busy, quint64(100)));
            if (!device.temperaturePath.isEmpty() &&
                readUnsigned(device.temperaturePath, millidegrees) && millidegrees <= 150000)
                stats.temperature = static_cast<int>(std::lround(millidegrees / 1000.0));
            if (!device.powerPath.isEmpty() &&
                readUnsigned(device.powerPath, microwatts) && microwatts <= 1000000000)
                stats.watts = microwatts / 1000000.0;
            if (readUnsigned(device.sysfsPath + "/mem_info_vram_used", used) &&
                readUnsigned(device.sysfsPath + "/mem_info_vram_total", total) && total > 0) {
                stats.used = used;
                stats.total = total;
                stats.memoryValid = true;
            }
        }
        return stats;
    }

private:
    struct NvmlUtilization { unsigned int gpu, memory; };
    struct NvmlMemory { unsigned long long total, free, used; };
    using InitFn = int (*)();
    using ShutdownFn = int (*)();
    using CountFn = int (*)(unsigned int *);
    using HandleFn = int (*)(unsigned int, void **);
    using UtilizationFn = int (*)(void *, NvmlUtilization *);
    using MemoryFn = int (*)(void *, NvmlMemory *);
    using TemperatureFn = int (*)(void *, int, unsigned int *);
    using PowerFn = int (*)(void *, unsigned int *);

    void discoverNvml() {
        if (!nvml_.load()) return;
        auto init = reinterpret_cast<InitFn>(nvml_.resolve("nvmlInit_v2"));
        shutdown_ = reinterpret_cast<ShutdownFn>(nvml_.resolve("nvmlShutdown"));
        auto count = reinterpret_cast<CountFn>(nvml_.resolve("nvmlDeviceGetCount_v2"));
        auto handle = reinterpret_cast<HandleFn>(nvml_.resolve("nvmlDeviceGetHandleByIndex_v2"));
        getUtilization_ = reinterpret_cast<UtilizationFn>(nvml_.resolve("nvmlDeviceGetUtilizationRates"));
        getMemory_ = reinterpret_cast<MemoryFn>(nvml_.resolve("nvmlDeviceGetMemoryInfo"));
        getTemperature_ = reinterpret_cast<TemperatureFn>(nvml_.resolve("nvmlDeviceGetTemperature"));
        getPower_ = reinterpret_cast<PowerFn>(nvml_.resolve("nvmlDeviceGetPowerUsage"));
        if (!init || !shutdown_ || !count || !handle || !getUtilization_ || !getMemory_ || init() != 0)
            return;
        nvmlReady_ = true;
        unsigned int countValue = 0;
        if (count(&countValue) != 0) return;
        for (unsigned int i = 0; i < std::min(countValue, 32u); ++i) {
            void *gpu = nullptr;
            if (handle(i, &gpu) == 0 && gpu)
                devices_.append({"nvidia:" + QString::number(i),
                                 "NVIDIA GPU " + QString::number(i), {}, {}, {}, gpu});
        }
    }

    void discoverSysfs() {
        const QDir drm("/sys/class/drm");
        for (const QString &entry : drm.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            if (!entry.startsWith("card")) continue;
            bool numeric = false;
            entry.mid(4).toUInt(&numeric);
            if (!numeric) continue;
            const QString path = drm.filePath(entry + "/device");
            quint64 vendor = 0;
            if (!readUnsigned(path + "/vendor", vendor) || vendor != 0x1002) continue;
            if (!QFileInfo::exists(path + "/gpu_busy_percent") &&
                !QFileInfo::exists(path + "/mem_info_vram_used")) continue;
            QString temperaturePath;
            QString powerPath;
            const QDir hwmon(path + "/hwmon");
            for (const QString &sensor : hwmon.entryList({"hwmon*"}, QDir::Dirs | QDir::NoDotAndDotDot)) {
                const QString candidate = hwmon.filePath(sensor + "/temp1_input");
                if (temperaturePath.isEmpty() && QFileInfo::exists(candidate)) temperaturePath = candidate;
                const QString power = hwmon.filePath(sensor + "/power1_average");
                if (powerPath.isEmpty() && QFileInfo::exists(power)) powerPath = power;
            }
            devices_.append({"amd:" + entry, "AMD Radeon · " + entry,
                             path, temperaturePath, powerPath, nullptr});
        }
    }

    QLibrary nvml_;
    QList<GpuDevice> devices_;
    ShutdownFn shutdown_ = nullptr;
    UtilizationFn getUtilization_ = nullptr;
    MemoryFn getMemory_ = nullptr;
    TemperatureFn getTemperature_ = nullptr;
    PowerFn getPower_ = nullptr;
    bool nvmlReady_ = false;
};

struct Counters {
    quint64 rx = 0;
    quint64 tx = 0;
};

static QMap<QString, Counters> readCounters() {
    QMap<QString, Counters> result;
    QFile file("/proc/net/dev");
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return result;
    const auto lines = file.readAll().split('\n');
    for (const QByteArray &line : lines) {
        const int colon = line.indexOf(':');
        if (colon < 0) continue;
        const QString name = QString::fromUtf8(line.left(colon).trimmed());
        const QList<QByteArray> fields = line.mid(colon + 1).simplified().split(' ');
        if (fields.size() < 16 || name.isEmpty()) continue;
        bool rxOk = false, txOk = false;
        const quint64 rx = fields[0].toULongLong(&rxOk);
        const quint64 tx = fields[8].toULongLong(&txOk);
        if (rxOk && txOk) result.insert(name, {rx, tx});
    }
    return result;
}

static bool isPhysicalInterface(const QString &name) {
    return QFileInfo::exists("/sys/class/net/" + name + "/device");
}

static bool isUp(const QString &name) {
    QFile file("/sys/class/net/" + name + "/operstate");
    return file.open(QIODevice::ReadOnly) && file.readAll().trimmed() == "up";
}

static QString defaultInterface(const QMap<QString, Counters> &available) {
    QString routed;
    quint64 bestMetric = std::numeric_limits<quint64>::max();
    QFile routes("/proc/net/route");
    if (routes.open(QIODevice::ReadOnly | QIODevice::Text)) {
        const auto lines = routes.readAll().split('\n');
        for (const QByteArray &line : lines) {
            const auto fields = line.simplified().split(' ');
            if (fields.size() > 7 && fields[1] == "00000000") {
                const QString name = QString::fromUtf8(fields[0]);
                bool ok = false;
                const quint64 metric = fields[6].toULongLong(&ok);
                if (available.contains(name) && isPhysicalInterface(name) && isUp(name) &&
                    ok && metric < bestMetric) {
                    routed = name;
                    bestMetric = metric;
                }
            }
        }
    }
    if (!routed.isEmpty()) return routed;
    for (auto it = available.cbegin(); it != available.cend(); ++it)
        if (isPhysicalInterface(it.key()) && isUp(it.key())) return it.key();
    for (auto it = available.cbegin(); it != available.cend(); ++it)
        if (isPhysicalInterface(it.key())) return it.key();
    for (auto it = available.cbegin(); it != available.cend(); ++it)
        if (it.key() != "lo" && isUp(it.key())) return it.key();
    return {};
}

static QString speedText(double bytesPerSecond) {
    static const char *units[] = {"B/s", "KiB/s", "MiB/s", "GiB/s", "TiB/s"};
    int unit = 0;
    while (bytesPerSecond >= 1024.0 && unit < 4) {
        bytesPerSecond /= 1024.0;
        ++unit;
    }
    const int decimals = unit == 0 ? 0 : (bytesPerSecond < 10 ? 2 : (bytesPerSecond < 100 ? 1 : 0));
    return QString::number(bytesPerSecond, 'f', decimals) + " " + units[unit];
}

static QString contextTokenText(quint64 tokens) {
    if (tokens < 1024) return QString::number(tokens);
    if (tokens < 10240) return QString::number(tokens / 1024.0, 'f', 1) + "K";
    return QString::number(qint64(std::lround(tokens / 1024.0))) + "K";
}

class NetFloat : public QWidget {
public:
    explicit NetFloat(const QIcon &logo) : settings_("NetFloat", "NetFloat"), normalLogo_(logo),
        noticeLogo_(alertLogo(logo, QColor(246, 202, 148))), warningLogo_(alertLogo(logo, QColor(245, 83, 92))) {
        setWindowTitle("NetFloat 网络悬浮窗");
        setWindowIcon(logo);
        setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
        setAttribute(Qt::WA_TranslucentBackground);
        setFixedSize(275, 437);
        setCursor(Qt::OpenHandCursor);
        interface_ = settings_.value("interface", "auto").toString();
        gpuSelection_ = settings_.value("gpu", "auto").toString();
        trendsExpanded_ = settings_.value("trendsExpanded", false).toBool();
        compactMode_ = settings_.value("compactMode", false).toBool();
        autoCollapse_ = settings_.value("autoCollapse", true).toBool();
        glassOpacity_ = settings_.value("glassOpacity", 115).toInt();
        if (glassOpacity_ < 100 || glassOpacity_ > 220) glassOpacity_ = 115;
        updateWindowSize();
        hoverExpandTimer_.setSingleShot(true);
        connect(&hoverExpandTimer_, &QTimer::timeout, this, [this] {
            if (autoCollapse_ && underMouse() && !detailVisible_) showDetails();
        });
        collapseTimer_.setSingleShot(true);
        connect(&collapseTimer_, &QTimer::timeout, this, [this] {
            if (!autoCollapse_ || !detailVisible_ || dragging_ || underMouse()) return;
            if (QApplication::activePopupWidget()) { collapseTimer_.start(800); return; }
            detailVisible_ = false;
            updateWindowSize();
            update();
        });
        llamaPort_ = settings_.value("llamaPort", 8080).toInt();
        if (llamaPort_ < 1 || llamaPort_ > 65535) llamaPort_ = 8080;
        clientContextLimit_ = settings_.value("clientContextLimit", 0).toULongLong();
        if (clientContextLimit_ > 10000000) clientContextLimit_ = 0;
        modelMonitoring_ = settings_.value("modelMonitoring", true).toBool();
        modelStatus_ = modelMonitoring_ ? "连接中" : "已关闭";
        alertGiB_ = settings_.value("vramAlertGiB", 2).toInt();
        if (alertGiB_ != 0 && alertGiB_ != 1 && alertGiB_ != 2 && alertGiB_ != 4 && alertGiB_ != 8)
            alertGiB_ = 2;
        int interval = settings_.value("interval", 750).toInt();
        if (interval != 500 && interval != 750 && interval != 1000 && interval != 2000) interval = 750;
        timer_.setInterval(interval);
        connect(&timer_, &QTimer::timeout, this, [this] { sample(); });
        timer_.start();
        sample();
        gpuTimer_.setInterval(2000);
        connect(&gpuTimer_, &QTimer::timeout, this, [this] { sampleGpu(); });
        gpuTimer_.start();
        sampleGpu();
        systemTimer_.setInterval(2000);
        connect(&systemTimer_, &QTimer::timeout, this, [this] { sampleSystem(); });
        systemTimer_.start();
        sampleSystem();
        modelTimer_.setSingleShot(true);
        connect(&modelTimer_, &QTimer::timeout, this, [this] { sampleModel(); });
        sampleModel();

        const QPoint saved = settings_.value("position").toPoint();
        if (settings_.contains("position")) {
            move(saved);
            if (settings_.contains("snapHorizontal") || settings_.contains("snapVertical")) {
                const int horizontal = settings_.value("snapHorizontal", 0).toInt();
                const int vertical = settings_.value("snapVertical", 0).toInt();
                snapLeft_ = horizontal == -1;
                snapRight_ = horizontal == 1;
                snapTop_ = vertical == -1;
                snapBottom_ = vertical == 1;
            } else {
                // Older versions stored only the top-left position, often for
                // the wider detail panel. Preserve a placement near an edge.
                QScreen *screen = QGuiApplication::screenAt(saved);
                if (!screen) screen = QGuiApplication::primaryScreen();
                if (screen) {
                    const QRect area = screen->availableGeometry();
                    snapLeft_ = std::abs(saved.x() - area.left()) <= 64;
                    snapRight_ = !snapLeft_ &&
                        std::min(std::abs(saved.x() + width() - area.right() - 1),
                                 std::abs(saved.x() + 275 - area.right() - 1)) <= 64;
                    snapTop_ = std::abs(saved.y() - area.top()) <= 32;
                    snapBottom_ = !snapTop_ &&
                        std::abs(saved.y() + height() - area.bottom() - 1) <= 32;
                }
            }
            alignToSnappedEdges();
        } else if (QScreen *screen = QGuiApplication::primaryScreen()) {
            const QRect r = screen->availableGeometry();
            move(r.right() - width() - 24, r.top() + 48);
        }
        if (trendsExpanded_) keepExpandedOnScreen();

        if (QSystemTrayIcon::isSystemTrayAvailable()) {
            tray_ = new QSystemTrayIcon(logo, this);
            tray_->setToolTip("NetFloat · 网络、GPU 和系统监控");
            trayMenu_ = new QMenu(this);
            trayToggle_ = trayMenu_->addAction("隐藏悬浮窗", this, [this] { toggleVisibility(); });
            trayMenu_->addAction("展开／收起趋势图", this, [this] { toggleTrends(); });
            addAutoCollapseMenu(trayMenu_);
            addCompactMenu(trayMenu_);
            addGlassMenu(trayMenu_);
            addAlertMenu(trayMenu_);
            addModelMenu(trayMenu_);
            trayMenu_->addSeparator();
            trayMenu_->addAction("退出 NetFloat", qApp, &QApplication::quit);
            connect(trayMenu_, &QMenu::aboutToShow, this, [this] {
                trayToggle_->setText(isVisible() ? "隐藏悬浮窗" : "显示悬浮窗");
            });
            tray_->setContextMenu(trayMenu_);
            connect(tray_, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
                if (reason == QSystemTrayIcon::Trigger) toggleTrends();
            });
            tray_->show();
            updateAlertIcon();
        }
    }

protected:
    void showEvent(QShowEvent *event) override {
        QWidget::showEvent(event);
        QTimer::singleShot(0, this, [this] { skipTaskbar(winId()); });
        QTimer::singleShot(0, this, [this] { applyBlurBehind(); });
    }

    void resizeEvent(QResizeEvent *event) override {
        QWidget::resizeEvent(event);
        applyBlurBehind();
    }

    void enterEvent(QEnterEvent *event) override {
        QWidget::enterEvent(event);
        collapseTimer_.stop();
        if (autoCollapse_ && !detailVisible_) hoverExpandTimer_.start(450);
    }

    void leaveEvent(QEvent *event) override {
        QWidget::leaveEvent(event);
        hoverExpandTimer_.stop();
        if (autoCollapse_ && detailVisible_ && !dragging_) collapseTimer_.start(900);
    }

    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setCompositionMode(QPainter::CompositionMode_Source);
        p.fillRect(rect(), Qt::transparent);
        p.setCompositionMode(QPainter::CompositionMode_SourceOver);
        p.setPen(Qt::NoPen);
        if (!(snapLeft_ || snapRight_ || snapTop_ || snapBottom_)) {
            p.setBrush(QColor(0, 0, 0, 28));
            p.drawRoundedRect(rect().adjusted(1, 2, -1, -1), 20, 20);
        }
        const QPainterPath glass = glassPath();
        p.fillPath(glass, QColor(24, 24, 24, glassOpacity_));
        p.setPen(QPen(QColor(255, 255, 255, 46), 1));
        p.drawPath(glass);
        p.setPen(QPen(QColor(255, 255, 255, 22), 1));
        p.drawLine(22, 3, width() - 22, 3);

        if (autoCollapse_ && !detailVisible_) {
            QFont mini = font();
            mini.setPixelSize(12);
            mini.setBold(true);
            p.setFont(mini);
            p.setPen(QColor(246, 246, 246));
            p.drawText(QRect(14, 7, 110, 23), Qt::AlignVCenter,
                p.fontMetrics().elidedText("↑ " + up_, Qt::ElideRight, 110));
            p.setPen(QColor(210, 210, 210));
            p.drawText(QRect(126, 7, 105, 23), Qt::AlignVCenter,
                p.fontMetrics().elidedText("↓ " + down_, Qt::ElideRight, 105));
            p.setPen(QColor(255, 255, 255, 30));
            p.drawLine(14, 33, 231, 33);
            mini.setPixelSize(11);
            mini.setBold(false);
            p.setFont(mini);
            p.setPen(vramAlertActive_ ? QColor(245, 110, 120) : QColor(246, 246, 246));
            const QString memory = gpuMemory_.startsWith("显存 ") ? gpuMemory_.mid(3) : gpuMemory_;
            p.drawText(QRect(14, 37, 217, 17), Qt::AlignVCenter,
                p.fontMetrics().elidedText(gpuBusy_ + " · 显存 " + memory, Qt::ElideRight, 217));
            QString miniModel = modelText_;
            miniModel.replace("生成 ", "AI ").replace(" token/s", "/s");
            if (modelStatus_ == "未连接" || modelStatus_ == "接口不可用" || modelStatus_ == "已关闭")
                miniModel = "AI " + modelStatus_;
            if (!modelContextPercent_.isEmpty()) miniModel += " · 上下文 " + modelContextPercent_;
            p.setPen(contextAlertLevel_ >= 2 ? QColor(245, 110, 120) : QColor(185, 185, 185));
            p.drawText(QRect(14, 58, 217, 17), Qt::AlignVCenter,
                p.fontMetrics().elidedText(miniModel, Qt::ElideRight, 217));
            return;
        }

        p.setPen(QPen(QColor(255, 255, 255, 15), 1));
        p.setBrush(QColor(255, 255, 255, 7));
        for (const QRect &card : {QRect(8, 7, 259, 82), QRect(8, 99, 259, 87),
                                  QRect(8, 194, 259, 73), QRect(8, 277, 259, 154)})
            p.drawRoundedRect(card, 14, 14);

        QFont small = font();
        small.setPixelSize(11);
        p.setFont(small);
        p.setPen(QColor(185, 185, 185));
        p.drawText(QRect(15, 8, 245, 15), Qt::AlignLeft | Qt::AlignVCenter, displayInterface_);

        QFont main = font();
        main.setPixelSize(16);
        main.setBold(true);
        p.setFont(main);
        p.setPen(QColor(246, 246, 246));
        p.drawText(QRect(15, 29, 245, 22), Qt::AlignLeft | Qt::AlignVCenter, "↑  " + up_);
        p.setPen(QColor(210, 210, 210));
        p.drawText(QRect(15, 57, 245, 22), Qt::AlignLeft | Qt::AlignVCenter, "↓  " + down_);

        p.setFont(small);
        p.setPen(QColor(185, 185, 185));
        p.drawText(QRect(15, 101, 153, 17), Qt::AlignLeft | Qt::AlignVCenter, gpuName_);
        p.setPen(vramAlertActive_ ? QColor(245, 110, 120) : QColor(185, 185, 185));
        p.drawText(QRect(168, 101, 92, 17), Qt::AlignRight | Qt::AlignVCenter, gpuFree_);
        QFont gpuFont = font();
        gpuFont.setPixelSize(15);
        gpuFont.setBold(true);
        p.setFont(gpuFont);
        p.setPen(QColor(246, 246, 246));
        p.drawText(QRect(15, 121, 92, 24), Qt::AlignLeft | Qt::AlignVCenter, gpuBusy_);
        QFont memoryFont = font();
        memoryFont.setPixelSize(13);
        memoryFont.setBold(true);
        p.setFont(memoryFont);
        p.setPen(QColor(246, 246, 246));
        p.drawText(QRect(106, 121, 154, 24), Qt::AlignRight | Qt::AlignVCenter, gpuMemory_);
        p.setFont(memoryFont);
        p.setPen(QColor(210, 210, 210));
        p.drawText(QRect(15, 149, 153, 19), Qt::AlignLeft | Qt::AlignVCenter, gpuTemperatureText_);
        p.setPen(QColor(185, 185, 185));
        p.drawText(QRect(164, 149, 96, 19), Qt::AlignRight | Qt::AlignVCenter, gpuPowerText_);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(255, 255, 255, 28));
        p.drawRoundedRect(QRect(15, 176, 245, 5), 2, 2);
        if (gpuRatio_ > 0) {
            p.setBrush(vramAlertActive_ ? QColor(245, 83, 92) : QColor(225, 225, 225));
            p.drawRoundedRect(QRect(15, 176, std::max(3, int(245 * gpuRatio_)), 5), 2, 2);
        }

        p.setFont(small);
        p.setPen(QColor(185, 185, 185));
        p.drawText(QRect(15, 196, 245, 16), Qt::AlignLeft | Qt::AlignVCenter, "系统");
        p.setFont(gpuFont);
        p.setPen(QColor(222, 222, 222));
        p.drawText(QRect(15, 214, 92, 24), Qt::AlignLeft | Qt::AlignVCenter, cpuText_);
        p.setFont(memoryFont);
        p.setPen(QColor(246, 246, 246));
        p.drawText(QRect(107, 214, 153, 24), Qt::AlignRight | Qt::AlignVCenter, memoryText_);
        p.setFont(memoryFont);
        p.setPen(QColor(185, 185, 185));
        p.drawText(QRect(15, 239, 245, 18), Qt::AlignLeft | Qt::AlignVCenter, cpuTemperatureText_);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(255, 255, 255, 28));
        p.drawRoundedRect(QRect(15, 258, 245, 5), 2, 2);
        if (memoryRatio_ > 0) {
            p.setBrush(QColor(222, 222, 222));
            p.drawRoundedRect(QRect(15, 258, std::max(3, int(245 * memoryRatio_)), 5), 2, 2);
        }
        p.setFont(small);
        p.setPen(QColor(185, 185, 185));
        p.drawText(QRect(15, 279, 245, 17), Qt::AlignLeft | Qt::AlignVCenter, "本地模型 · llama.cpp");
        p.setFont(gpuFont);
        p.setPen(QColor(246, 246, 246));
        p.drawText(QRect(15, 300, 190, 23), Qt::AlignLeft | Qt::AlignVCenter, modelText_);
        p.setFont(small);
        p.setPen(QColor(185, 185, 185));
        p.drawText(QRect(205, 300, 55, 23), Qt::AlignRight | Qt::AlignVCenter, modelStatus_);
        p.setPen(QColor(185, 185, 185));
        p.drawText(QRect(15, 328, 245, 18), Qt::AlignLeft | Qt::AlignVCenter, modelPromptText_);
        p.setPen(contextAlertLevel_ >= 2 ? QColor(245, 110, 120) :
                 contextAlertLevel_ == 1 ? QColor(246, 202, 148) : QColor(185, 185, 185));
        p.drawText(QRect(15, 352, 190, 18), Qt::AlignLeft | Qt::AlignVCenter,
                   p.fontMetrics().elidedText(modelContextText_, Qt::ElideRight, 190));
        p.drawText(QRect(205, 352, 55, 18), Qt::AlignRight | Qt::AlignVCenter, modelContextPercent_);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(255, 255, 255, 28));
        p.drawRoundedRect(QRect(15, 378, 245, 5), 2, 2);
        if (modelContextRatio_ > 0) {
            p.setBrush(contextAlertLevel_ >= 2 ? QColor(245, 83, 92) :
                       contextAlertLevel_ == 1 ? QColor(246, 202, 148) : QColor(246, 246, 246));
            p.drawRoundedRect(QRect(15, 378, std::max(3, int(245 * modelContextRatio_)), 5), 2, 2);
        }
        p.setFont(small);
        p.setPen(QColor(185, 185, 185));
        if (!compactMode_) {
            p.drawText(QRect(15, 392, 245, 17), Qt::AlignLeft | Qt::AlignVCenter, lastTurnLine1_);
            p.drawText(QRect(15, 413, 245, 17), Qt::AlignLeft | Qt::AlignVCenter, lastTurnLine2_);
        }
        if (trendsExpanded_) {
            p.save();
            if (compactMode_) p.translate(0, -46);
            drawTrends(p);
            p.restore();
        }
    }

    void mouseDoubleClickEvent(QMouseEvent *event) override {
        if (event->button() == Qt::LeftButton) {
            dragging_ = false;
            setCursor(Qt::OpenHandCursor);
            toggleTrends();
            event->accept();
        }
    }

    void mousePressEvent(QMouseEvent *event) override {
        if (event->button() == Qt::LeftButton) {
            hoverExpandTimer_.stop();
            collapseTimer_.stop();
            dragOffset_ = event->globalPosition().toPoint() - frameGeometry().topLeft();
            dragging_ = true;
            setCursor(Qt::ClosedHandCursor);
            event->accept();
        }
    }

    void mouseMoveEvent(QMouseEvent *event) override {
        if (dragging_ && (event->buttons() & Qt::LeftButton)) {
            const QPoint desired = event->globalPosition().toPoint() - dragOffset_;
            QScreen *screen = QGuiApplication::screenAt(event->globalPosition().toPoint());
            if (!screen) screen = QGuiApplication::primaryScreen();
            if (!screen) { move(desired); return; }
            const QRect area = screen->availableGeometry();
            const int left = area.left();
            const int right = area.right() - width() + 1;
            const int top = area.top();
            const int bottom = area.bottom() - height() + 1;
            const int x = std::abs(desired.x() - left) <= 28 ? left :
                          std::abs(desired.x() - right) <= 28 ? right :
                          std::clamp(desired.x(), left, std::max(left, right));
            const int y = std::abs(desired.y() - top) <= 28 ? top :
                          std::abs(desired.y() - bottom) <= 28 ? bottom :
                          std::clamp(desired.y(), top, std::max(top, bottom));
            move(x, y);
            setSnapEdges(x == left, x == right, y == top, y == bottom);
            event->accept();
        }
    }

    void mouseReleaseEvent(QMouseEvent *event) override {
        if (event->button() == Qt::LeftButton && dragging_) {
            dragging_ = false;
            setCursor(Qt::OpenHandCursor);
            settings_.setValue("position", pos());
            settings_.setValue("snapHorizontal", snapLeft_ ? -1 : snapRight_ ? 1 : 0);
            settings_.setValue("snapVertical", snapTop_ ? -1 : snapBottom_ ? 1 : 0);
            if (autoCollapse_ && detailVisible_ && !underMouse()) collapseTimer_.start(900);
        }
    }

    void contextMenuEvent(QContextMenuEvent *event) override {
        QMenu menu(this);
        QMenu *interfaces = menu.addMenu("监测网卡");
        const auto current = readCounters();
        auto addInterface = [this, interfaces](const QString &name, const QString &label) {
            QAction *action = interfaces->addAction(label);
            action->setCheckable(true);
            action->setChecked(interface_ == name);
            connect(action, &QAction::triggered, this, [this, name] {
                interface_ = name;
                settings_.setValue("interface", name);
                previousValid_ = false;
                sample();
            });
        };
        addInterface("auto", "自动选择（实际联网网卡）");
        for (auto it = current.cbegin(); it != current.cend(); ++it)
            if (it.key() != "lo")
                addInterface(it.key(), it.key() +
                    (isPhysicalInterface(it.key()) ? "（实体网卡）" : "（虚拟网卡）"));

        QMenu *gpus = menu.addMenu("监测显卡");
        auto addGpu = [this, gpus](const QString &id, const QString &label) {
            QAction *action = gpus->addAction(label);
            action->setCheckable(true);
            action->setChecked(gpuSelection_ == id);
            connect(action, &QAction::triggered, this, [this, id] {
                gpuSelection_ = id;
                settings_.setValue("gpu", id);
                sampleGpu();
            });
        };
        addGpu("auto", "自动选择");
        for (const GpuDevice &gpu : gpuMonitor_.devices()) addGpu(gpu.id, gpu.label);

        menu.addAction(trendsExpanded_ ? "收起趋势图" : "展开趋势图", this, [this] { toggleTrends(); });
        addAutoCollapseMenu(&menu);
        addCompactMenu(&menu);
        addGlassMenu(&menu);
        addAlertMenu(&menu);
        addModelMenu(&menu);

        QMenu *refresh = menu.addMenu("刷新间隔");
        for (int ms : {500, 750, 1000, 2000}) {
            QAction *action = refresh->addAction(QString::number(ms) + " ms");
            action->setCheckable(true);
            action->setChecked(timer_.interval() == ms);
            connect(action, &QAction::triggered, this, [this, ms] {
                timer_.setInterval(ms);
                settings_.setValue("interval", ms);
            });
        }
        menu.addSeparator();
        if (tray_ && tray_->isVisible()) menu.addAction("隐藏悬浮窗", this, [this] { hide(); });
        menu.addAction("退出", qApp, &QApplication::quit);
        menu.exec(event->globalPos());
    }

private:
    struct TrendPoint {
        double up = 0;
        double down = 0;
        int gpu = -1;
        double vram = -1;
    };

    struct ModelTaskUsage {
        qint64 taskId = -1;
        quint64 promptBase = 0;
        quint64 promptProcessed = 0;
        quint64 promptCached = 0;
        quint64 maxDecoded = 0;
        quint64 lastContext = 0;
        bool hasPromptBase = false;
    };

    static QString exactTokens(quint64 count) {
        return QLocale(QLocale::English).toString(static_cast<qulonglong>(count));
    }

    void finishModelTask(int slotId, quint64 finalContext, bool complete) {
        auto it = modelTasks_.find(slotId);
        if (it == modelTasks_.end()) return;
        const ModelTaskUsage task = it.value();
        modelTasks_.erase(it);
        if (task.maxDecoded == 0) return;
        const quint64 input = task.hasPromptBase ? task.promptBase : task.promptProcessed + task.promptCached;
        quint64 output = task.maxDecoded;
        if (complete && task.hasPromptBase && finalContext >= input) {
            // llama-server keeps the final emitted token out of the KV slot.
            output = std::max(output, finalContext - input + 1);
        }
        // /slots is sampled, so the request can finish between polls. Even with
        // a final slot snapshot, the output count is inferred from KV occupancy.
        lastTurnLine1_ = "上轮 输入 约" + exactTokens(input) + " · 输出 约" + exactTokens(output);
        lastTurnLine2_ = "总用 约" + exactTokens(input + output) +
            (complete ? " · 结束上下文 " : " · 末次上下文 ") + exactTokens(finalContext);
    }

    void addCompactMenu(QMenu *parent) {
        QAction *action = parent->addAction("紧凑显示（隐藏上轮明细）");
        action->setCheckable(true);
        action->setChecked(compactMode_);
        connect(action, &QAction::toggled, this, [this](bool checked) {
            compactMode_ = checked;
            settings_.setValue("compactMode", checked);
            updateWindowSize();
            keepExpandedOnScreen();
            update();
        });
        connect(parent, &QMenu::aboutToShow, this, [this, action] {
            action->setChecked(compactMode_);
        });
    }

    void addAutoCollapseMenu(QMenu *parent) {
        QAction *action = parent->addAction("悬停展开（自动收起）");
        action->setCheckable(true);
        action->setChecked(autoCollapse_);
        connect(action, &QAction::toggled, this, [this](bool checked) {
            autoCollapse_ = checked;
            settings_.setValue("autoCollapse", checked);
            hoverExpandTimer_.stop();
            collapseTimer_.stop();
            detailVisible_ = !checked;
            updateWindowSize();
            keepExpandedOnScreen();
            update();
        });
        connect(parent, &QMenu::aboutToShow, this, [this, action] {
            action->setChecked(autoCollapse_);
        });
    }

    void addGlassMenu(QMenu *parent) {
        QMenu *glass = parent->addMenu("玻璃透明度");
        for (int opacity : {115, 145, 180}) {
            QAction *action = glass->addAction(opacity == 115 ? "更通透（45%）" :
                                               opacity == 145 ? "平衡（57%）" : "更易读（71%）");
            action->setCheckable(true);
            action->setChecked(glassOpacity_ == opacity);
            action->setData(opacity);
            connect(action, &QAction::triggered, this, [this, opacity] {
                glassOpacity_ = opacity;
                settings_.setValue("glassOpacity", opacity);
                update();
            });
        }
        connect(glass, &QMenu::aboutToShow, this, [this, glass] {
            for (QAction *action : glass->actions())
                action->setChecked(action->data().toInt() == glassOpacity_);
        });
    }

    void addAlertMenu(QMenu *parent) {
        QMenu *alerts = parent->addMenu("显存余量提醒");
        for (int threshold : {0, 1, 2, 4, 8}) {
            const QString label = threshold == 0 ? "关闭" : "低于 " + QString::number(threshold) + " GiB";
            QAction *action = alerts->addAction(label);
            action->setCheckable(true);
            action->setChecked(alertGiB_ == threshold);
            action->setData(threshold);
            connect(action, &QAction::triggered, this, [this, threshold] {
                alertGiB_ = threshold;
                settings_.setValue("vramAlertGiB", threshold);
                alertNotified_ = false;
                sampleGpu();
            });
        }
        connect(alerts, &QMenu::aboutToShow, this, [this, alerts] {
            for (QAction *action : alerts->actions())
                action->setChecked(action->data().toInt() == alertGiB_);
        });
    }

    void addModelMenu(QMenu *parent) {
        QMenu *model = parent->addMenu("本地模型");
        QAction *enabled = model->addAction("显示生成速度");
        enabled->setCheckable(true);
        enabled->setChecked(modelMonitoring_);
        connect(enabled, &QAction::triggered, this, [this](bool checked) {
            modelMonitoring_ = checked;
            settings_.setValue("modelMonitoring", checked);
            resetModelMonitor();
        });
        QAction *port = model->addAction("接口端口…");
        connect(port, &QAction::triggered, this, [this] {
            bool accepted = false;
            const int value = QInputDialog::getInt(this, "本地模型接口", "llama-server 端口：",
                                                    llamaPort_, 1, 65535, 1, &accepted);
            if (accepted && value != llamaPort_) {
                llamaPort_ = value;
                settings_.setValue("llamaPort", value);
                resetModelMonitor();
            }
        });
        QAction *contextLimit = model->addAction("自定义上下文上限…");
        connect(contextLimit, &QAction::triggered, this, [this] {
            bool accepted = false;
            const int value = QInputDialog::getInt(this, "自定义上下文上限",
                "调用端设置的最大上下文 token 数（0 = 跟随服务端）：",
                static_cast<int>(clientContextLimit_), 0, 10000000, 1, &accepted);
            if (accepted && quint64(value) != clientContextLimit_) {
                clientContextLimit_ = quint64(value);
                settings_.setValue("clientContextLimit", value);
                refreshContextDisplay();
            }
        });
        connect(model, &QMenu::aboutToShow, this, [this, enabled, port, contextLimit] {
            enabled->setChecked(modelMonitoring_);
            port->setText("接口端口…（" + QString::number(llamaPort_) + "）");
            contextLimit->setText(clientContextLimit_ ?
                "自定义上下文上限…（" + contextTokenText(clientContextLimit_) + "）" :
                "自定义上下文上限…（跟随服务端）");
        });
    }

    void updateWindowSize() {
        if (autoCollapse_ && !detailVisible_) setFixedSize(245, 83);
        else setFixedSize(275, (compactMode_ ? 391 : 437) + (trendsExpanded_ ? 208 : 0));
        alignToSnappedEdges();
    }

    void setSnapEdges(bool left, bool right, bool top, bool bottom) {
        if (snapLeft_ == left && snapRight_ == right &&
            snapTop_ == top && snapBottom_ == bottom) return;
        snapLeft_ = left;
        snapRight_ = right;
        snapTop_ = top;
        snapBottom_ = bottom;
        update();
        applyBlurBehind();
    }

    void alignToSnappedEdges() {
        if (!(snapLeft_ || snapRight_ || snapTop_ || snapBottom_)) return;
        QScreen *screen = QGuiApplication::screenAt(frameGeometry().center());
        if (!screen) screen = QGuiApplication::primaryScreen();
        if (!screen) return;
        const QRect area = screen->availableGeometry();
        const int nextX = snapLeft_ ? area.left() :
                          snapRight_ ? area.right() - width() + 1 : x();
        const int nextY = snapTop_ ? area.top() :
                          snapBottom_ ? area.bottom() - height() + 1 : y();
        move(nextX, nextY);
    }

    QPainterPath glassPath() const {
        const qreal left = snapLeft_ ? 0 : 2;
        const qreal top = snapTop_ ? 0 : 2;
        const qreal right = snapRight_ ? width() : width() - 2;
        const qreal bottom = snapBottom_ ? height() : height() - 3;
        const qreal tl = snapLeft_ || snapTop_ ? 0 : 19;
        const qreal tr = snapRight_ || snapTop_ ? 0 : 19;
        const qreal br = snapRight_ || snapBottom_ ? 0 : 19;
        const qreal bl = snapLeft_ || snapBottom_ ? 0 : 19;
        QPainterPath path;
        path.moveTo(left + tl, top);
        path.lineTo(right - tr, top);
        if (tr) path.quadTo(right, top, right, top + tr);
        path.lineTo(right, bottom - br);
        if (br) path.quadTo(right, bottom, right - br, bottom);
        path.lineTo(left + bl, bottom);
        if (bl) path.quadTo(left, bottom, left, bottom - bl);
        path.lineTo(left, top + tl);
        if (tl) path.quadTo(left, top, left + tl, top);
        path.closeSubpath();
        return path;
    }

    void applyBlurBehind() {
#ifdef NETFLOAT_KDE_BLUR
        if (!windowHandle() || !KWindowEffects::isEffectAvailable(KWindowEffects::BlurBehind)) return;
        KWindowEffects::enableBlurBehind(windowHandle(), true,
                                         QRegion(glassPath().toFillPolygon().toPolygon()));
#endif
    }

    void showDetails() {
        detailVisible_ = true;
        updateWindowSize();
        keepExpandedOnScreen();
        update();
        if (autoCollapse_ && !underMouse()) collapseTimer_.start(3500);
    }

    void toggleTrends() {
        if (!isVisible()) show();
        trendsExpanded_ = !trendsExpanded_;
        settings_.setValue("trendsExpanded", trendsExpanded_);
        if (autoCollapse_) showDetails();
        else {
            updateWindowSize();
            if (trendsExpanded_) keepExpandedOnScreen();
        }
        raise();
        update();
    }

    void keepExpandedOnScreen() {
        QScreen *screen = QGuiApplication::screenAt(frameGeometry().topLeft());
        if (!screen) screen = QGuiApplication::primaryScreen();
        if (screen) {
            const QRect area = screen->availableGeometry();
            move(std::clamp(x(), area.left(), std::max(area.left(), area.right() - width() + 1)),
                 std::clamp(y(), area.top(), std::max(area.top(), area.bottom() - height() + 1)));
            setSnapEdges(x() == area.left(), x() + width() == area.right() + 1,
                         y() == area.top(), y() + height() == area.bottom() + 1);
        }
    }

    void updateAlertIcon() {
        if (!tray_) return;
        const int level = vramAlertActive_ ? 2 : contextAlertLevel_;
        if (trayIconLevel_ != level) {
            tray_->setIcon(level >= 2 ? warningLogo_ : level == 1 ? noticeLogo_ : normalLogo_);
            trayIconLevel_ = level;
        }
        tray_->setToolTip("NetFloat · " + gpuFree_ +
            (vramAlertActive_ ? " · 显存余量偏低" : "") +
            (contextAlertLevel_ ? " · " + modelContextText_ : ""));
    }

    void updateContextAlert(double ratio) {
        if (ratio < 0.78) contextNotifiedLevel_ = 0;
        else if (ratio < 0.88 && contextNotifiedLevel_ > 1) contextNotifiedLevel_ = 1;
        contextAlertLevel_ = ratio >= 0.9 ? 2 : ratio >= 0.8 ? 1 : 0;
        if (contextAlertLevel_ > contextNotifiedLevel_) {
            if (tray_)
                tray_->showMessage("NetFloat 上下文提醒",
                    modelContextText_ + "（已达 " + QString::number(contextAlertLevel_ == 2 ? 90 : 80) + "%）",
                    QSystemTrayIcon::Warning, 6000);
            contextNotifiedLevel_ = contextAlertLevel_;
        }
        updateAlertIcon();
    }

    void drawSeries(QPainter &p, const QRect &area, int metric, double scale, const QColor &color) const {
        if (trends_.isEmpty() || scale <= 0) return;
        QPainterPath path;
        bool started = false;
        for (int i = 0; i < trends_.size(); ++i) {
            const TrendPoint &point = trends_[i];
            const double value = metric == 0 ? point.up : metric == 1 ? point.down :
                                 metric == 2 ? point.gpu : point.vram * 100.0;
            if (value < 0) { started = false; continue; }
            const double x = area.right() - (trends_.size() - 1 - i) * area.width() / 149.0;
            const double y = area.bottom() - std::clamp(value / scale, 0.0, 1.0) * area.height();
            if (started) path.lineTo(x, y);
            else { path.moveTo(x, y); started = true; }
        }
        QPen line(color, 1.7);
        if (metric == 1) line.setStyle(Qt::DashLine);
        p.setPen(line);
        p.drawPath(path);
    }

    void drawTrends(QPainter &p) const {
        p.setPen(QColor(255, 255, 255, 30));
        p.drawLine(15, 443, 260, 443);
        QFont caption = font();
        caption.setPixelSize(11);
        p.setFont(caption);
        p.setPen(QColor(185, 185, 185));
        p.drawText(QRect(15, 449, 155, 16), Qt::AlignVCenter, "网络速度 · 近 5 分钟");
        p.setPen(QColor(246, 246, 246));
        p.drawText(QRect(184, 449, 28, 16), Qt::AlignVCenter, "↑");
        p.setPen(QColor(210, 210, 210));
        p.drawText(QRect(215, 449, 28, 16), Qt::AlignVCenter, "↓");
        p.setPen(QColor(185, 185, 185));
        p.drawText(QRect(15, 510, 245, 16), Qt::AlignVCenter, "GPU 占用 · 近 5 分钟");
        p.drawText(QRect(15, 571, 245, 16), Qt::AlignVCenter, "显存占用 · 近 5 分钟");
        const QRect network(15, 469, 245, 34), gpu(15, 530, 245, 34), vram(15, 591, 245, 34);
        p.setPen(QColor(255, 255, 255, 28));
        for (const QRect &area : {network, gpu, vram}) {
            p.drawLine(area.bottomLeft(), area.bottomRight());
            p.drawLine(area.left(), area.center().y(), area.right(), area.center().y());
        }
        double maxSpeed = 1024;
        for (const TrendPoint &point : trends_)
            maxSpeed = std::max({maxSpeed, point.up, point.down});
        drawSeries(p, network, 0, maxSpeed, QColor(246, 246, 246));
        drawSeries(p, network, 1, maxSpeed, QColor(210, 210, 210));
        drawSeries(p, gpu, 2, 100, QColor(232, 232, 232));
        drawSeries(p, vram, 3, 100, QColor(222, 222, 222));
    }

    void resetModelMonitor() {
        modelTimer_.stop();
        modelFailures_ = 0;
        if (modelReply_) {
            QNetworkReply *old = modelReply_;
            modelReply_ = nullptr;
            old->abort();
        }
        modelPreviousCounts_.clear();
        modelClock_ = QElapsedTimer();
        lastModelRateClock_ = QElapsedTimer();
        modelText_ = "生成 —";
        modelStatus_ = modelMonitoring_ ? "连接中" : "已关闭";
        modelPromptText_ = "提示词 —";
        lastPromptValid_ = false;
        modelTasks_.clear();
        lastTurnLine1_ = "上轮：等待一次完整回复";
        lastTurnLine2_.clear();
        contextNotifiedLevel_ = 0;
        clearModelContext();
        update();
        if (modelMonitoring_) sampleModel();
    }

    void clearModelContext() {
        modelContextText_ = "上下文 —";
        modelContextPercent_.clear();
        modelContextRatio_ = 0;
        stableContextUsed_ = 0;
        stableContextTotal_ = 0;
        modelContextPrevious_ = false;
        contextAlertLevel_ = 0;
        updateAlertIcon();
    }

    void showStableContext(bool previous) {
        if (stableContextTotal_ == 0) return;
        modelContextPrevious_ = previous;
        const quint64 effectiveLimit = clientContextLimit_
            ? std::min(clientContextLimit_, stableContextTotal_) : stableContextTotal_;
        const double ratio = double(stableContextUsed_) / effectiveLimit;
        modelContextRatio_ = std::clamp(ratio, 0.0, 1.0);
        modelContextText_ = (previous ? "上次上下文 " : "上下文 ") +
            contextTokenText(stableContextUsed_) + "/" + contextTokenText(effectiveLimit);
        if (effectiveLimit != stableContextTotal_)
            modelContextText_ += " · 服" + contextTokenText(stableContextTotal_);
        modelContextPercent_ = stableContextUsed_ > 0 && ratio < 0.001 ? "<0.1%" :
            QString::number(ratio * 100.0, 'f', ratio < 0.01 ? 1 : 0) + "%";
    }

    void refreshContextDisplay() {
        if (stableContextTotal_ > 0) {
            showStableContext(modelContextPrevious_);
            if (!modelContextPrevious_) updateContextAlert(modelContextRatio_);
            else updateAlertIcon();
        }
        update();
    }

    void scheduleModelPoll(int delayMs) {
        if (modelMonitoring_) modelTimer_.start(delayMs);
    }

    int modelRetryDelay() {
        modelFailures_ = std::min(modelFailures_ + 1, 4);
        return std::min(10000, 1000 << modelFailures_);
    }

    void sampleModel() {
        if (!modelMonitoring_ || modelReply_) return;
        QNetworkRequest request{QUrl("http://127.0.0.1:" + QString::number(llamaPort_) + "/slots")};
        request.setTransferTimeout(1500);
        QNetworkReply *reply = modelNetwork_.get(request);
        modelReply_ = reply;
        connect(reply, &QNetworkReply::finished, this, [this, reply] {
            if (reply != modelReply_) { reply->deleteLater(); return; }
            modelReply_ = nullptr;
            int nextPollMs = 2000;
            if (reply->error() != QNetworkReply::NoError) {
                nextPollMs = modelRetryDelay();
                const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                modelStatus_ = status == 404 || status == 501 ? "接口不可用" : "未连接";
                modelText_ = "生成 —";
                modelPreviousCounts_.clear();
                modelClock_ = QElapsedTimer();
                modelPromptText_ = "提示词 —";
                lastPromptValid_ = false;
                modelTasks_.clear();
                clearModelContext();
            } else {
                const QJsonDocument document = QJsonDocument::fromJson(reply->readAll());
                if (!document.isArray()) {
                    nextPollMs = modelRetryDelay();
                    modelStatus_ = "接口异常";
                    modelText_ = "生成 —";
                    modelPreviousCounts_.clear();
                    modelClock_ = QElapsedTimer();
                    modelPromptText_ = "提示词 —";
                    lastPromptValid_ = false;
                    modelTasks_.clear();
                    clearModelContext();
                } else {
                    modelFailures_ = 0;
                    QHash<QString, quint64> counts;
                    int active = 0;
                    bool anyDecoded = false;
                    bool promptFieldsSeen = false;
                    quint64 promptProcessed = 0, promptCached = 0;
                    int bestPriority = -1;
                    double bestRatio = -1;
                    quint64 bestUsed = 0, bestTotal = 0;
                    for (const QJsonValue &value : document.array()) {
                        if (!value.isObject()) continue;
                        const QJsonObject slot = value.toObject();
                        const bool processing = slot.value("is_processing").toBool();
                        const int slotId = slot.value("id").toInt();
                        const qint64 taskId = qint64(slot.value("id_task").toDouble(-1));
                        const QJsonValue occupied = slot.value("n_prompt_tokens");
                        quint64 count = 0;
                        bool valid = false;
                        const QJsonValue next = slot.value("next_token");
                        auto addCount = [&count, &valid](const QJsonValue &token) {
                            if (!token.isObject()) return;
                            const QJsonValue decoded = token.toObject().value("n_decoded");
                            if (decoded.isDouble() && decoded.toDouble() >= 0) {
                                count += quint64(decoded.toDouble());
                                valid = true;
                            }
                        };
                        if (next.isArray()) {
                            for (const QJsonValue &token : next.toArray()) addCount(token);
                        } else addCount(next);
                        if (processing) {
                            ++active;
                            anyDecoded = anyDecoded || count > 0;
                            const QString key = QString::number(slotId) + ":" + QString::number(taskId);
                            if (valid) counts.insert(key, count);
                            const QJsonValue processed = slot.value("n_prompt_tokens_processed");
                            const QJsonValue cached = slot.value("n_prompt_tokens_cache");
                            auto oldTask = modelTasks_.find(slotId);
                            if (oldTask != modelTasks_.end() && oldTask->taskId != taskId)
                                finishModelTask(slotId, oldTask->lastContext, false);
                            ModelTaskUsage &task = modelTasks_[slotId];
                            task.taskId = taskId;
                            task.maxDecoded = std::max(task.maxDecoded, count);
                            if (occupied.isDouble() && occupied.toDouble() >= 0) {
                                const quint64 used = quint64(occupied.toDouble());
                                task.lastContext = used;
                                if (count > 0 && used >= count && !task.hasPromptBase) {
                                    task.promptBase = used - count;
                                    task.hasPromptBase = true;
                                }
                            }
                            if (processed.isDouble() && processed.toDouble() >= 0) {
                                const quint64 number = quint64(processed.toDouble());
                                promptProcessed += number;
                                task.promptProcessed = std::max(task.promptProcessed, number);
                                promptFieldsSeen = true;
                            }
                            if (cached.isDouble() && cached.toDouble() >= 0) {
                                const quint64 number = quint64(cached.toDouble());
                                promptCached += number;
                                task.promptCached = std::max(task.promptCached, number);
                                promptFieldsSeen = true;
                            }
                        } else {
                            auto oldTask = modelTasks_.find(slotId);
                            if (oldTask != modelTasks_.end()) {
                                const bool complete = oldTask->taskId == taskId && occupied.isDouble() && occupied.toDouble() >= 0;
                                const quint64 finalContext = complete
                                    ? quint64(occupied.toDouble()) : oldTask->lastContext;
                                finishModelTask(slotId, finalContext, complete);
                            }
                        }
                        const QJsonValue capacity = slot.value("n_ctx");
                        // While a new prompt is being evaluated, n_prompt_tokens is only
                        // the processed prefix. It is not the complete context yet.
                        const bool ready = !processing || count > 0;
                        if (capacity.isDouble() && occupied.isDouble() && capacity.toDouble() > 0 &&
                            occupied.toDouble() >= 0 && ready) {
                            const quint64 total = quint64(capacity.toDouble());
                            const quint64 used = quint64(occupied.toDouble());
                            const quint64 effectiveLimit = clientContextLimit_
                                ? std::min(clientContextLimit_, total) : total;
                            const double ratio = double(used) / effectiveLimit;
                            const int priority = processing ? 1 : 0;
                            if (priority > bestPriority || (priority == bestPriority && ratio > bestRatio)) {
                                bestPriority = priority;
                                bestRatio = ratio;
                                bestUsed = used;
                                bestTotal = total;
                            }
                        }
                    }
                    if (active > 0 && bestPriority < 1) {
                        if (stableContextTotal_ == 0 && bestTotal > 0) {
                            stableContextUsed_ = bestUsed;
                            stableContextTotal_ = bestTotal;
                        }
                        if (stableContextTotal_ > 0) {
                            showStableContext(true);
                            updateAlertIcon();
                        }
                        else {
                            modelContextText_ = "上下文读入中…";
                            modelContextPercent_.clear();
                            modelContextRatio_ = 0;
                        }
                    } else if (bestTotal > 0) {
                        stableContextUsed_ = bestUsed;
                        stableContextTotal_ = bestTotal;
                        showStableContext(false);
                        updateContextAlert(modelContextRatio_);
                    } else clearModelContext();
                    quint64 delta = 0;
                    bool comparable = false;
                    for (auto it = counts.cbegin(); it != counts.cend(); ++it) {
                        if (modelPreviousCounts_.contains(it.key()) && it.value() >= modelPreviousCounts_.value(it.key())) {
                            delta += it.value() - modelPreviousCounts_.value(it.key());
                            comparable = true;
                        }
                    }
                    const qint64 elapsed = modelClock_.isValid() ? modelClock_.elapsed() : 0;
                    modelPreviousCounts_ = counts;
                    modelClock_.restart();
                    if (active > 0) {
                        nextPollMs = 1000;
                        if (promptFieldsSeen && (promptProcessed + promptCached > 0 || anyDecoded)) {
                            lastPromptProcessed_ = promptProcessed;
                            lastPromptCached_ = promptCached;
                            lastPromptValid_ = true;
                            modelPromptText_ = "提示词 新算 " + contextTokenText(promptProcessed) +
                                " · 复用 " + contextTokenText(promptCached);
                        } else modelPromptText_ = "提示词读入中…";
                        modelStatus_ = anyDecoded ? (active > 1 ? QString::number(active) + " 路" : "生成中") : "读入中";
                        if (comparable && elapsed > 0 && delta > 0) {
                            const double speed = delta * 1000.0 / elapsed;
                            modelText_ = "生成 " + QString::number(speed, 'f', 1) + " token/s";
                            lastModelRate_ = speed;
                            lastModelRateClock_.restart();
                        } else {
                            modelText_ = "生成 —";
                        }
                    } else {
                        nextPollMs = 2000;
                        modelStatus_ = "空闲";
                        modelText_ = lastModelRateClock_.isValid() && lastModelRateClock_.elapsed() < 10000
                            ? "最近 " + QString::number(lastModelRate_, 'f', 1) + " token/s" : "生成 —";
                        modelPromptText_ = lastPromptValid_
                            ? "上次提示 新算 " + contextTokenText(lastPromptProcessed_) +
                              " · 复用 " + contextTokenText(lastPromptCached_) : "提示词 —";
                    }
                }
            }
            reply->deleteLater();
            update();
            scheduleModelPoll(nextPollMs);
        });
    }

    void sampleSystem() {
        QFile cpuFile("/proc/stat");
        if (cpuFile.open(QIODevice::ReadOnly)) {
            const auto fields = cpuFile.readLine().simplified().split(' ');
            if (fields.size() >= 9 && fields[0] == "cpu") {
                quint64 total = 0, idle = 0;
                bool valid = true;
                for (int i = 1; i <= 8; ++i) {
                    bool ok = false;
                    const quint64 value = fields[i].toULongLong(&ok);
                    valid = valid && ok;
                    total += value;
                    if (i == 4 || i == 5) idle += value;
                }
                if (valid && cpuTotalPrev_ > 0 && total > cpuTotalPrev_ && idle >= cpuIdlePrev_) {
                    const quint64 deltaTotal = total - cpuTotalPrev_;
                    const quint64 deltaIdle = idle - cpuIdlePrev_;
                    const int percent = std::clamp(int(std::lround(100.0 *
                        (deltaTotal - std::min(deltaTotal, deltaIdle)) / deltaTotal)), 0, 100);
                    cpuText_ = "CPU " + QString::number(percent) + "%";
                }
                if (valid) { cpuTotalPrev_ = total; cpuIdlePrev_ = idle; }
            }
        }

        QFile memoryFile("/proc/meminfo");
        if (memoryFile.open(QIODevice::ReadOnly)) {
            quint64 total = 0, available = 0;
            for (const QByteArray &line : memoryFile.readAll().split('\n')) {
                if (line.startsWith("MemTotal:"))
                    total = line.mid(9).simplified().split(' ').value(0).toULongLong();
                else if (line.startsWith("MemAvailable:"))
                    available = line.mid(13).simplified().split(' ').value(0).toULongLong();
            }
            if (total > 0 && available <= total) {
                const double gib = 1024.0 * 1024.0; // /proc/meminfo uses KiB
                memoryText_ = "内存 " + QString::number((total - available) / gib, 'f', 1) + "/" +
                              QString::number(total / gib, 'f', 1) + " GiB";
                memoryRatio_ = double(total - available) / total;
            }
        }

        quint64 millidegrees = 0;
        if (!cpuTempPath_.isEmpty() && readUnsigned(cpuTempPath_, millidegrees))
            cpuTemperatureText_ = "CPU 温度 " + QString::number(int(std::lround(millidegrees / 1000.0))) + "°C";
        else
            cpuTemperatureText_ = "CPU 温度 —";
        update();
    }

    void toggleVisibility() {
        if (isVisible()) hide();
        else {
            show();
            raise();
        }
    }

    void sampleGpu() {
        const GpuDevice *chosen = nullptr;
        GpuStats stats;
        quint64 largest = 0;
        for (const GpuDevice &device : gpuMonitor_.devices()) {
            if (gpuSelection_ != "auto" && gpuSelection_ != device.id) continue;
            const GpuStats current = gpuMonitor_.read(device);
            if (!chosen || (gpuSelection_ == "auto" && current.total > largest)) {
                chosen = &device;
                stats = current;
                largest = current.total;
            }
        }
        gpuName_ = chosen ? chosen->label : "未检测到支持的显卡";
        gpuBusy_ = stats.busy >= 0 ? "GPU " + QString::number(stats.busy) + "%" : "GPU —";
        gpuTemperatureText_ = stats.temperature >= 0
            ? "GPU 温度 " + QString::number(stats.temperature) + "°C" : "GPU 温度 —";
        gpuPowerText_ = stats.watts >= 0
            ? "功耗 " + QString::number(stats.watts, 'f', 0) + " W" : "功耗 —";
        const QString chosenId = chosen ? chosen->id : QString();
        if (chosenId != lastGpuId_) {
            trends_.clear();
            lastGpuId_ = chosenId;
            alertNotified_ = false;
        }
        if (stats.memoryValid) {
            const double gib = 1024.0 * 1024.0 * 1024.0;
            gpuMemory_ = "显存 " + QString::number(stats.used / gib, 'f', 1) + "/" +
                         QString::number(stats.total / gib, 'f', 1) + " GiB";
            const double freeGiB = double(stats.total - std::min(stats.used, stats.total)) / gib;
            gpuFree_ = "剩 " + QString::number(freeGiB, 'f', freeGiB < 1 ? 2 : 1) + " GiB";
            gpuRatio_ = std::clamp(double(stats.used) / stats.total, 0.0, 1.0);
            vramAlertActive_ = alertGiB_ > 0 && freeGiB < alertGiB_;
        } else {
            gpuMemory_ = "显存 —";
            gpuFree_ = "剩 —";
            gpuRatio_ = 0;
            vramAlertActive_ = false;
        }
        if (vramAlertActive_ && !alertNotified_) {
            if (tray_)
                tray_->showMessage("NetFloat 显存提醒",
                    gpuName_ + "，" + gpuFree_ + "（低于 " + QString::number(alertGiB_) + " GiB）",
                    QSystemTrayIcon::Warning, 6000);
            alertNotified_ = true;
        } else if (!vramAlertActive_) {
            alertNotified_ = false;
        }
        updateAlertIcon();
        trends_.append({upBytes_, downBytes_, stats.busy, stats.memoryValid ? gpuRatio_ : -1.0});
        if (trends_.size() > 150) trends_.removeFirst();
        update();
    }

    void sample() {
        const auto current = readCounters();
        const QString selected = interface_ == "auto" ? defaultInterface(current) : interface_;
        if (selected != previousInterface_) trends_.clear();
        displayInterface_ = selected.isEmpty() ? "无可用网卡" : selected;
        if (interface_ == "auto") displayInterface_ += "  ·  自动";
        if (!current.contains(selected)) {
            previousValid_ = false;
            down_ = up_ = "—";
            upBytes_ = downBytes_ = 0;
            update();
            return;
        }
        const Counters now = current.value(selected);
        if (previousValid_ && previousInterface_ == selected) {
            const qint64 elapsed = clock_.elapsed();
            if (elapsed > 0 && now.rx >= previous_.rx && now.tx >= previous_.tx) {
                const double scale = 1000.0 / elapsed;
                downBytes_ = (now.rx - previous_.rx) * scale;
                upBytes_ = (now.tx - previous_.tx) * scale;
                down_ = speedText(downBytes_);
                up_ = speedText(upBytes_);
            } else {
                down_ = up_ = "0 B/s";
                upBytes_ = downBytes_ = 0;
            }
        } else {
            down_ = up_ = "0 B/s";
            upBytes_ = downBytes_ = 0;
        }
        previous_ = now;
        previousInterface_ = selected;
        previousValid_ = true;
        clock_.restart();
        update();
    }

    QSettings settings_;
    QSystemTrayIcon *tray_ = nullptr;
    QMenu *trayMenu_ = nullptr;
    QAction *trayToggle_ = nullptr;
    QIcon normalLogo_;
    QIcon noticeLogo_;
    QIcon warningLogo_;
    GpuMonitor gpuMonitor_;
    QString cpuTempPath_ = discoverCpuTemperature();
    QTimer timer_;
    QTimer gpuTimer_;
    QTimer systemTimer_;
    QTimer modelTimer_;
    QTimer hoverExpandTimer_;
    QTimer collapseTimer_;
    QNetworkAccessManager modelNetwork_;
    QNetworkReply *modelReply_ = nullptr;
    QHash<QString, quint64> modelPreviousCounts_;
    QHash<int, ModelTaskUsage> modelTasks_;
    QElapsedTimer modelClock_;
    QElapsedTimer lastModelRateClock_;
    QElapsedTimer clock_;
    QString interface_;
    QString previousInterface_;
    QString displayInterface_;
    QString gpuSelection_;
    QString lastGpuId_;
    QString gpuName_;
    QString gpuBusy_;
    QString gpuMemory_;
    QString gpuFree_ = "剩 —";
    QString gpuPowerText_ = "功耗 —";
    QString modelText_ = "生成 —";
    QString modelStatus_ = "连接中";
    QString modelPromptText_ = "提示词 —";
    QString modelContextText_ = "上下文 —";
    QString modelContextPercent_;
    QString lastTurnLine1_ = "上轮：等待一次完整回复";
    QString lastTurnLine2_;
    double modelContextRatio_ = 0;
    quint64 stableContextUsed_ = 0;
    quint64 stableContextTotal_ = 0;
    quint64 clientContextLimit_ = 0;
    quint64 lastPromptProcessed_ = 0;
    quint64 lastPromptCached_ = 0;
    bool lastPromptValid_ = false;
    int contextAlertLevel_ = 0;
    int contextNotifiedLevel_ = 0;
    int llamaPort_ = 8080;
    int modelFailures_ = 0;
    bool modelMonitoring_ = true;
    bool modelContextPrevious_ = false;
    double lastModelRate_ = 0;
    double gpuRatio_ = 0;
    int alertGiB_ = 2;
    bool vramAlertActive_ = false;
    bool alertNotified_ = false;
    int trayIconLevel_ = -1;
    bool trendsExpanded_ = false;
    bool compactMode_ = false;
    bool autoCollapse_ = true;
    bool detailVisible_ = false;
    bool snapLeft_ = false;
    bool snapRight_ = false;
    bool snapTop_ = false;
    bool snapBottom_ = false;
    int glassOpacity_ = 115;
    QList<TrendPoint> trends_;
    double upBytes_ = 0;
    double downBytes_ = 0;
    QString cpuText_ = "CPU —";
    QString memoryText_ = "内存 —";
    QString gpuTemperatureText_ = "GPU 温度 —";
    QString cpuTemperatureText_ = "CPU 温度 —";
    double memoryRatio_ = 0;
    quint64 cpuTotalPrev_ = 0;
    quint64 cpuIdlePrev_ = 0;
    QString down_ = "0 B/s";
    QString up_ = "0 B/s";
    Counters previous_;
    QPoint dragOffset_;
    bool previousValid_ = false;
    bool dragging_ = false;
};

int main(int argc, char *argv[]) {
    // XWayland supports ordinary window positioning and the keep-above hint on
    // compositors such as KWin. Respect an explicit platform choice by users.
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM") &&
        !qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY") &&
        !qEnvironmentVariableIsEmpty("DISPLAY"))
        qputenv("QT_QPA_PLATFORM", "xcb");
    QApplication app(argc, argv);
    app.setApplicationName("NetFloat");
    app.setQuitOnLastWindowClosed(false);
    const QIcon logo = loadLogo();
    app.setWindowIcon(logo);
    NetFloat window(logo);
    window.show();
    return app.exec();
}
