#include <QApplication>
#include <QMainWindow>
#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QPushButton>
#include <QLabel>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGroupBox>
#include <QStyleFactory>
#include <QPalette>
#include <QtCharts/QChartView>
#include <QtCharts/QLineSeries>
#include <QtCharts/QValueAxis>
#include <QThread>
#include <QTimer>
#include <QMutex>
#include <QKeyEvent>

#include <uhd/usrp/multi_usrp.hpp>
#include <uhd/device.hpp>
#include <uhd/stream.hpp>
#include <complex>
#include <cmath>
#include <atomic>

using namespace QtCharts;

// --- 1. THE WORKER (Handles Hardware & Math) ---
class RadioWorker : public QThread {
public:
    std::atomic<bool> running{true};
    std::atomic<bool> hardware_connected{false};
    
    // Settings
    std::atomic<double> frequency{915e6};
    std::atomic<double> gain{40.0};
    std::atomic<double> amplitude{1.0};
    std::atomic<int> waveform_type{0}; // 0=Sine, 1=Square

    QMutex data_mutex;
    std::vector<std::complex<float>> shared_buffer;
    QString device_args = "";

    void run() override {
        uhd::usrp::multi_usrp::sptr usrp;
        uhd::tx_streamer::sptr tx_stream;
        uhd::tx_metadata_t md;

        // --- CONNECTION ATTEMPT ---
        try {
            if (!device_args.isEmpty()) {
                usrp = uhd::usrp::multi_usrp::make(uhd::device_addr_t(device_args.toStdString()));
                usrp->set_tx_rate(1e6);
                usrp->set_tx_freq(frequency.load());
                usrp->set_tx_gain(gain.load());
                
                uhd::stream_args_t stream_args("fc32");
                tx_stream = usrp->get_tx_stream(stream_args);
                
                md.start_of_burst = true;
                md.end_of_burst = false;
                hardware_connected = true;
            }
        } catch (...) {
            hardware_connected = false;
        }

        // --- SIGNAL GENERATION LOOP ---
        const size_t buff_size = 2048; // Larger buffer for better zooming
        std::vector<std::complex<float>> buff(buff_size);
        double phase = 0.0;
        double current_freq = 10e3; 
        double sample_rate = 1e6;

        while (running) {
            double increment = 2.0 * M_PI * current_freq / sample_rate;
            double amp = amplitude.load();
            int type = waveform_type.load();

            for (size_t i = 0; i < buff_size; i++) {
                double val_i, val_q;
                if (type == 0) { // SINE
                    val_i = cos(phase);
                    val_q = sin(phase);
                } else { // SQUARE
                    val_i = (cos(phase) > 0) ? 1.0 : -1.0;
                    val_q = (sin(phase) > 0) ? 1.0 : -1.0;
                }
                buff[i] = std::complex<float>(val_i * amp, val_q * amp);
                phase += increment;
                if (phase > 2 * M_PI) phase -= 2 * M_PI;
            }

            if (hardware_connected) {
                tx_stream->send(buff.data(), buff.size(), md);
                md.start_of_burst = false;
            } else {
                QThread::usleep(2000); // Sleep roughly equivalent to buffer time
            }

            if (data_mutex.tryLock()) {
                shared_buffer = buff;
                data_mutex.unlock();
            }
        }
        
        if (hardware_connected) {
            md.end_of_burst = true;
            tx_stream->send("", 0, md);
        }
    }
};

// --- 2. CUSTOM CHART VIEW (For better Zoom handling) ---
class ZoomableChartView : public QChartView {
public:
    ZoomableChartView(QChart *chart) : QChartView(chart) {
        setRenderHint(QPainter::Antialiasing);
        setRubberBand(QChartView::RectangleRubberBand); // Enable Mouse Drag Zoom
    }
    // Enable zooming with scroll wheel
    void wheelEvent(QWheelEvent *event) override {
        if (event->angleDelta().y() > 0) chart()->zoomIn();
        else chart()->zoomOut();
        event->accept();
    }
};

// --- 3. THE MAIN GUI WINDOW ---
class MainWindow : public QMainWindow {
    RadioWorker *worker;
    QChart *chart;
    QLineSeries *seriesI; 
    QLineSeries *seriesQ;
    QTimer *timer;
    bool isPaused = false;
    
    // UI Elements
    QComboBox *deviceCombo;
    QLabel *statusLabel;
    QDoubleSpinBox *freqBox;
    QDoubleSpinBox *gainBox;
    QDoubleSpinBox *ampBox;
    QComboBox *waveCombo;
    QPushButton *connectBtn;
    QPushButton *pauseBtn;

public:
    MainWindow() {
        // --- APPLY DARK THEME ---
        applyDarkTheme();

        worker = new RadioWorker();
        
        QWidget *centralWidget = new QWidget;
        QHBoxLayout *mainLayout = new QHBoxLayout(centralWidget);

        // -- LEFT PANEL (Controls) --
        QGroupBox *controlPanel = new QGroupBox("Control Deck");
        controlPanel->setFixedWidth(280);
        QVBoxLayout *panelLayout = new QVBoxLayout(controlPanel);

        // Device Connection Group
        QGroupBox *devGroup = new QGroupBox("Device");
        QVBoxLayout *devLayout = new QVBoxLayout(devGroup);
        deviceCombo = new QComboBox();
        refreshDevices();
        
        connectBtn = new QPushButton("INITIALIZE SYSTEM");
        connectBtn->setCheckable(true);
        connectBtn->setStyleSheet("QPushButton { background-color: #2E7D32; color: white; font-weight: bold; padding: 10px; border-radius: 4px; }"
                                  "QPushButton:checked { background-color: #C62828; text: 'SHUTDOWN SYSTEM'; }");
        
        statusLabel = new QLabel("STATUS: STANDBY");
        statusLabel->setAlignment(Qt::AlignCenter);
        statusLabel->setStyleSheet("color: #757575; font-weight: bold; border: 1px solid #424242; padding: 5px;");

        devLayout->addWidget(new QLabel("Select Hardware Interface:"));
        devLayout->addWidget(deviceCombo);
        devLayout->addWidget(connectBtn);
        devLayout->addWidget(statusLabel);
        panelLayout->addWidget(devGroup);

        // Signal Param Group
        QGroupBox *sigGroup = new QGroupBox("Waveform Generator");
        QFormLayout *sigLayout = new QFormLayout(sigGroup);
        
        freqBox = new QDoubleSpinBox();
        freqBox->setRange(70e6, 6e9);
        freqBox->setValue(915e6);
        freqBox->setSuffix(" Hz");

        gainBox = new QDoubleSpinBox();
        gainBox->setRange(0, 89);
        gainBox->setValue(40);
        gainBox->setSuffix(" dB");
        
        ampBox = new QDoubleSpinBox();
        ampBox->setRange(0, 1.0);
        ampBox->setSingleStep(0.1);
        ampBox->setValue(1.0);

        waveCombo = new QComboBox();
        waveCombo->addItem("Sine Wave");
        waveCombo->addItem("Square Wave");

        sigLayout->addRow("Center Freq:", freqBox);
        sigLayout->addRow("TX Gain:", gainBox);
        sigLayout->addRow("Amplitude:", ampBox);
        sigLayout->addRow("Modulation:", waveCombo);
        panelLayout->addWidget(sigGroup);

        // View Control Group
        QGroupBox *viewGroup = new QGroupBox("Visualizer Controls");
        QVBoxLayout *viewLayout = new QVBoxLayout(viewGroup);
        
        pauseBtn = new QPushButton("PAUSE VIEW");
        pauseBtn->setCheckable(true);
        
        QPushButton *resetZoomBtn = new QPushButton("RESET ZOOM");
        
        viewLayout->addWidget(pauseBtn);
        viewLayout->addWidget(resetZoomBtn);
        panelLayout->addWidget(viewGroup);
        
        panelLayout->addStretch();
        mainLayout->addWidget(controlPanel);

        // -- RIGHT PANEL (Chart) --
        chart = new QChart();
        chart->setTheme(QChart::ChartThemeDark); // Built-in Dark Theme for Graph
        
        seriesI = new QLineSeries();
        seriesQ = new QLineSeries();
        seriesI->setName("In-Phase (I)");
        seriesQ->setName("Quadrature (Q)");
        
        // Style the lines
        QPen penI(QColor(0, 255, 0)); // Neon Green
        penI.setWidth(2);
        seriesI->setPen(penI);

        QPen penQ(QColor(255, 20, 147)); // Neon Pink
        penQ.setWidth(2);
        seriesQ->setPen(penQ);
        
        chart->addSeries(seriesI);
        chart->addSeries(seriesQ);
        chart->createDefaultAxes();
        
        QValueAxis *axisX = qobject_cast<QValueAxis*>(chart->axes(Qt::Horizontal).first());
        axisX->setRange(0, 200);
        axisX->setTitleText("Samples");
        
        QValueAxis *axisY = qobject_cast<QValueAxis*>(chart->axes(Qt::Vertical).first());
        axisY->setRange(-1.5, 1.5);
        axisY->setTitleText("Amplitude");
        chart->setTitle("Real-Time Time Domain Analysis");
        
        // Use custom view for Zooming
        ZoomableChartView *chartView = new ZoomableChartView(chart);
        mainLayout->addWidget(chartView);

        setCentralWidget(centralWidget);
        resize(1200, 700);
        setWindowTitle("USRP Control Interface v2.0");

        // --- CONNECTIONS ---
        connect(connectBtn, &QPushButton::clicked, this, &MainWindow::toggleConnection);
        connect(resetZoomBtn, &QPushButton::clicked, [=](){ chart->zoomReset(); });
        
        connect(pauseBtn, &QPushButton::toggled, [=](bool checked){ 
            isPaused = checked; 
            pauseBtn->setText(checked ? "RESUME VIEW" : "PAUSE VIEW");
            pauseBtn->setStyleSheet(checked ? "background-color: #F57C00;" : "");
        });

        connect(freqBox, QOverload<double>::of(&QDoubleSpinBox::valueChanged), 
                [=](double v){ worker->frequency = v; });
        connect(gainBox, QOverload<double>::of(&QDoubleSpinBox::valueChanged), 
                [=](double v){ worker->gain = v; });
        connect(ampBox, QOverload<double>::of(&QDoubleSpinBox::valueChanged), 
                [=](double v){ worker->amplitude = v; });
        connect(waveCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), 
                [=](int idx){ worker->waveform_type = idx; });

        // --- TIMER ---
        timer = new QTimer(this);
        connect(timer, &QTimer::timeout, this, &MainWindow::updatePlot);
        timer->start(33);
    }

    void applyDarkTheme() {
        qApp->setStyle(QStyleFactory::create("Fusion"));
        QPalette p;
        p.setColor(QPalette::Window, QColor(53, 53, 53));
        p.setColor(QPalette::WindowText, Qt::white);
        p.setColor(QPalette::Base, QColor(25, 25, 25));
        p.setColor(QPalette::AlternateBase, QColor(53, 53, 53));
        p.setColor(QPalette::ToolTipBase, Qt::white);
        p.setColor(QPalette::ToolTipText, Qt::white);
        p.setColor(QPalette::Text, Qt::white);
        p.setColor(QPalette::Button, QColor(53, 53, 53));
        p.setColor(QPalette::ButtonText, Qt::white);
        p.setColor(QPalette::BrightText, Qt::red);
        p.setColor(QPalette::Link, QColor(42, 130, 218));
        p.setColor(QPalette::Highlight, QColor(42, 130, 218));
        p.setColor(QPalette::HighlightedText, Qt::black);
        qApp->setPalette(p);
    }

    void refreshDevices() {
        deviceCombo->clear();
        deviceCombo->addItem("Simulation Mode");
        uhd::device_addrs_t devices = uhd::device::find(uhd::device_addr_t(""));
        for (const auto &dev : devices) {
            std::string label = dev.get("type") + " (" + dev.get("serial") + ")";
            deviceCombo->addItem(QString::fromStdString(label), QString::fromStdString(dev.to_string()));
        }
    }

    void toggleConnection() {
        if (worker->isRunning()) {
            worker->running = false;
            worker->wait();
            connectBtn->setText("INITIALIZE SYSTEM");
            connectBtn->setChecked(false);
            statusLabel->setText("STATUS: STANDBY");
            statusLabel->setStyleSheet("color: #757575; font-weight: bold; border: 1px solid #424242; padding: 5px;");
        } else {
            QString args = deviceCombo->currentData().toString();
            worker->device_args = args;
            worker->running = true;
            worker->start();
            
            QTimer::singleShot(500, this, [=]() {
                if (worker->hardware_connected) {
                    statusLabel->setText("STATUS: TX ACTIVE");
                    statusLabel->setStyleSheet("color: #00E676; font-weight: bold; border: 2px solid #00E676; padding: 5px;");
                    connectBtn->setText("ABORT TX");
                    connectBtn->setChecked(true);
                } else {
                    statusLabel->setText("STATUS: SIMULATION");
                    statusLabel->setStyleSheet("color: #FFEA00; font-weight: bold; border: 2px solid #FFEA00; padding: 5px;");
                    connectBtn->setText("STOP SIM");
                    connectBtn->setChecked(true);
                }
            });
        }
    }

    void updatePlot() {
        if (isPaused) return; // Don't update if paused

        std::vector<std::complex<float>> local_data;
        worker->data_mutex.lock();
        if(!worker->shared_buffer.empty()) local_data = worker->shared_buffer;
        worker->data_mutex.unlock();

        if(local_data.empty()) return;

        QList<QPointF> pI, pQ;
        // Limit points for performance, but take enough for a good wave
        int limit = std::min((int)local_data.size(), 500); 
        for(int i=0; i<limit; ++i) {
            pI.append(QPointF(i, local_data[i].real()));
            pQ.append(QPointF(i, local_data[i].imag()));
        }
        seriesI->replace(pI);
        seriesQ->replace(pQ);
    }
};

int main(int argc, char *argv[]) {
    QApplication a(argc, argv);
    MainWindow w;
    w.show();
    return a.exec();
}