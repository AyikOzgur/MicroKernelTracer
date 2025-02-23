#include <windows.h>
#include <iostream>
#include <array>
#include <cstring>
#include <thread>
#include <set>
#include <mutex>
#include <atomic>

#include "SerialPort.h"
#include "utils.h"

#include <QApplication>
#include <QWidget>
#include <QPainter>
#include <QTimer>
#include <QString>
#include <QFontMetrics>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSlider>
#include <QResizeEvent>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>

// Global serial settings.
std::string g_portName = "COM6";
int g_baudRate = 115200;
std::atomic<bool> g_stopReceiver{false};
std::thread* g_receivingThread = nullptr;

// Define the packed structure.
#pragma pack(push, 1)
struct TraceEvent_t
{
  uint16_t deltaTime : 10; // 10 bits for delta time (0–1023)
  uint16_t eventType : 2;  // 2 bits for event type (0–3)
  uint16_t threadId : 4;   // 4 bits for thread ID (0–15)
};
#pragma pack(pop)

const int PACKET_RECORD_COUNT = 1023;
const int BUFFER_SIZE = PACKET_RECORD_COUNT * sizeof(TraceEvent_t);

// Global shared events and mutex.
TraceEvent_t g_sharedEvents[PACKET_RECORD_COUNT];
std::mutex g_sharedEventsMutex;

// Receiver thread function.
void receivingTracerDataThreadFunc()
{
  SerialPort serial;
  // Assume that SerialPort::open can accept a port name and a baud rate.
  if (!serial.open(g_portName, g_baudRate))
  {
    std::cerr << LOG_LOCATION << "Error opening serial port: " << g_portName << std::endl;
    return;
  }

  std::cout << "Connected to " << g_portName << " at " << g_baudRate << std::endl;

  char buffer[BUFFER_SIZE];

  while (!g_stopReceiver)
  {
    int bytesRead = serial.read(reinterpret_cast<uint8_t *>(buffer), BUFFER_SIZE);
    if (bytesRead >= BUFFER_SIZE)
    {
      std::lock_guard<std::mutex> lock(g_sharedEventsMutex);
      std::memcpy(g_sharedEvents, buffer, BUFFER_SIZE);
    }
  }

  serial.close();
}

// Custom widget that performs the drawing.
class VisualizationWidget : public QWidget
{
  Q_OBJECT
public:
  explicit VisualizationWidget(QWidget *parent = nullptr)
      : QWidget(parent), m_horizontalOffset(0)
  {
    setMinimumHeight(720);
  }

public slots:
  void setHorizontalOffset(int offset)
  {
    m_horizontalOffset = offset;
    update();
  }

protected:
  void paintEvent(QPaintEvent *event) override
  {
    Q_UNUSED(event);

    // Copy shared events safely.
    TraceEvent_t *events = new TraceEvent_t[PACKET_RECORD_COUNT];
    {
      std::lock_guard<std::mutex> lock(g_sharedEventsMutex);
      std::memcpy(events, g_sharedEvents, BUFFER_SIZE);
    }

    QPainter painter(this);
    painter.fillRect(rect(), Qt::black);

    const int segmentWidth = 50; // Each event occupies 50 pixels horizontally.
    const int barHeight = 20;    // Height of each thread bar.
    const int leftMargin = 100;  // Margin reserved for thread labels.
    const int baseY = 300;       // Y offset where the bars start.

    std::set<int> usedThreadIds;

    // Draw all events and collect used thread IDs.
    for (size_t i = 0; i < PACKET_RECORD_COUNT; ++i)
    {
      int tid = events[i].threadId;
      usedThreadIds.insert(tid);

      // Calculate x-position using horizontal scrolling.
      int x = leftMargin + static_cast<int>(i) * segmentWidth - m_horizontalOffset;
      int y = baseY + tid * barHeight;
      QRect rect(x, y, segmentWidth, barHeight);

      // Choose red for PENDSV events (eventType == 1), green otherwise.
      QColor rectColor = (events[i].eventType == 1) ? Qt::red : Qt::green;

      if (rect.right() >= leftMargin && rect.left() <= width())
      {
        painter.fillRect(rect, rectColor);
        QString text = QString::number(events[i].deltaTime);
        painter.setPen(Qt::black);
        painter.drawText(rect.adjusted(5, 0, 0, 0), Qt::AlignVCenter | Qt::AlignLeft, text);
      }
    }

    // Draw labels only for threads that were used.
    for (int tid : usedThreadIds)
    {
      QString label = QString("Thread %1").arg(tid);
      int barY = baseY + tid * barHeight;
      int barCenter = barY + barHeight / 2;
      QFontMetrics fm(painter.font());
      int textHeight = fm.height();
      int textY = barCenter + textHeight / 4;
      painter.setPen(Qt::white);
      painter.drawText(10, textY, label);
    }

    delete[] events;
  }

private:
  int m_horizontalOffset;
};

// Main window that contains the visualization widget, slider, and serial controls.
class MainWindow : public QWidget
{
  Q_OBJECT
public:
  MainWindow(QWidget *parent = nullptr) : QWidget(parent)
  {
    QVBoxLayout *mainLayout = new QVBoxLayout(this);

    // Create a horizontal layout for serial port controls.
    QHBoxLayout *serialLayout = new QHBoxLayout();

    m_portNameEdit = new QLineEdit(this);
    m_portNameEdit->setPlaceholderText("Serial Port (e.g., COM6)");
    m_portNameEdit->setText(QString::fromStdString(g_portName));

    m_baudRateSpin = new QSpinBox(this);
    m_baudRateSpin->setRange(300, 1000000);
    m_baudRateSpin->setValue(g_baudRate);

    m_connectButton = new QPushButton("Connect", this);
    connect(m_connectButton, &QPushButton::clicked, this, &MainWindow::onConnectButtonClicked);

    serialLayout->addWidget(m_portNameEdit);
    serialLayout->addWidget(m_baudRateSpin);
    serialLayout->addWidget(m_connectButton);

    mainLayout->addLayout(serialLayout);

    // Add the visualization widget.
    m_visualizationWidget = new VisualizationWidget(this);
    mainLayout->addWidget(m_visualizationWidget);

    // Add a horizontal slider.
    m_slider = new QSlider(Qt::Horizontal, this);
    mainLayout->addWidget(m_slider);

    connect(m_slider, &QSlider::valueChanged,
            m_visualizationWidget, &VisualizationWidget::setHorizontalOffset);

    // Timer for periodic visualization updates.
    QTimer *timer = new QTimer(this);
    connect(timer, &QTimer::timeout,
            m_visualizationWidget, QOverload<>::of(&VisualizationWidget::update));
    timer->start(16);
  }

  ~MainWindow()
  {
    // Ensure that the receiver thread is stopped.
    if(g_receivingThread != nullptr)
    {
      g_stopReceiver = true;
      g_receivingThread->join();
      delete g_receivingThread;
      g_receivingThread = nullptr;
    }
  }

protected:
  void resizeEvent(QResizeEvent *event) override
  {
    QWidget::resizeEvent(event);

    const int segmentWidth = 50;
    const int leftMargin = 100;
    int totalWidth = leftMargin + PACKET_RECORD_COUNT * segmentWidth;
    int visibleWidth = m_visualizationWidget->width();
    int maxOffset = totalWidth - visibleWidth;
    if (maxOffset < 0)
      maxOffset = 0;
    m_slider->setMinimum(0);
    m_slider->setMaximum(maxOffset);
  }

private slots:
  void onConnectButtonClicked()
  {
    // If a receiver thread is already running, stop it.
    if(g_receivingThread != nullptr)
    {
      g_stopReceiver = true;
      g_receivingThread->join();
      delete g_receivingThread;
      g_receivingThread = nullptr;
      g_stopReceiver = false;
    }

    // Update the global serial settings from the UI.
    g_portName = m_portNameEdit->text().toStdString();
    g_baudRate = m_baudRateSpin->value();

    // Start a new receiving thread.
    g_receivingThread = new std::thread(receivingTracerDataThreadFunc);
  }

private:
  VisualizationWidget *m_visualizationWidget;
  QSlider *m_slider;
  QLineEdit *m_portNameEdit;
  QSpinBox *m_baudRateSpin;
  QPushButton *m_connectButton;
};

int main(int argc, char *argv[])
{
  QApplication app(argc, argv);

  MainWindow mainWindow;
  mainWindow.setWindowTitle("Scheduling Visualization");
  mainWindow.resize(1280, 720);
  mainWindow.show();

  return app.exec();
}

#include "main.moc"
