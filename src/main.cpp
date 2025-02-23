#include <windows.h>
#include <iostream>
#include <array>
#include <cstring>
#include <thread>
#include <set>
#include <mutex>
#include <atomic>
#include <chrono>

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
#include <QComboBox>

#include "SerialPort.h"
#include "utils.h"

// Define the packed structure.
#pragma pack(push, 1)
struct TraceEvent_t
{
  uint16_t deltaTime : 10; // 10 bits for delta time (0–1023)
  uint16_t eventType : 2;  // 2 bits for event type (0–3)
  uint16_t threadId : 4;   // 4 bits for thread ID (0–15)
};
#pragma pack(pop)

std::string g_portName = "COM6";
int g_baudRate = 9600;

/// Atomic flags for controlling the connection.
std::thread g_receivingThread;
std::atomic<bool> g_stopReceiver{false};
std::atomic<bool> g_connectionRequested{false};
std::atomic<bool> g_restartNeeded{false};

constexpr int PACKET_RECORD_COUNT = 1023;
constexpr int BUFFER_SIZE = PACKET_RECORD_COUNT * sizeof(TraceEvent_t);

/// Global shared events and its mutex.
TraceEvent_t g_sharedEvents[PACKET_RECORD_COUNT];
std::mutex g_sharedEventsMutex;

/// Thread function that receives data from the serial port and updates the shared buffer.
void receivingTracerDataThreadFunc()
{
  SerialPort serialPort;
  uint8_t buffer[BUFFER_SIZE];

  while (!g_stopReceiver.load())
  {
    if (g_restartNeeded.load())
    {
      if (serialPort.isOpen())
        serialPort.close();

      if (g_connectionRequested.load())
      {
        if (!serialPort.open(g_portName, g_baudRate))
        {
          std::cerr << LOG_LOCATION << "Error opening serial port: " << g_portName << std::endl;
          std::this_thread::sleep_for(std::chrono::milliseconds(500));
          continue;
        }
      }
      g_restartNeeded.store(false);
    }

    if (serialPort.isOpen())
    {
      int bytesRead = serialPort.read(buffer, BUFFER_SIZE);
      if (bytesRead >= BUFFER_SIZE)
      {
        std::lock_guard<std::mutex> lock(g_sharedEventsMutex);
        std::memcpy(g_sharedEvents, buffer, BUFFER_SIZE);
      }
    }
    else
    {
      // Not connected—sleep briefly.
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
  }

  serialPort.close();
}


/// Custom widget that performs the drawing.
class VisualizationWidget : public QWidget
{
  Q_OBJECT
public:
  explicit VisualizationWidget(QWidget *parent = nullptr)
      : QWidget(parent), m_horizontalOffset(0)
  {
    setMinimumWidth(1280);
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
    g_sharedEventsMutex.lock();
    std::memcpy(m_events, g_sharedEvents, BUFFER_SIZE);
    g_sharedEventsMutex.unlock();

    QPainter painter(this);
    painter.fillRect(rect(), Qt::black);

    const int segmentWidth = 50; // Each event occupies 50 pixels horizontally.
    const int barHeight = 20;    // Height of each thread bar.
    const int leftMargin = 100;  // Margin reserved for thread labels.
    const int baseY = 300;       // Y offset where the bars start.

    std::set<int> usedThreadIds;

    // Draw events and collect used thread IDs.
    for (size_t i = 0; i < PACKET_RECORD_COUNT; ++i)
    {
      int tid = m_events[i].threadId;
      usedThreadIds.insert(tid);

      int x = leftMargin + static_cast<int>(i) * segmentWidth - m_horizontalOffset;
      int y = baseY + tid * barHeight;
      QRect rect(x, y, segmentWidth, barHeight);

      // Choose red for PENDSV events (eventType == 1), green otherwise.
      QColor rectColor = (m_events[i].eventType == 1) ? Qt::red : Qt::green;
      if (rect.right() >= leftMargin && rect.left() <= width())
      {
        painter.fillRect(rect, rectColor);
        QString text = QString::number(m_events[i].deltaTime);
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

    // Draw legend in the top-right corner.
    int legendWidth = 150;
    int legendHeight = 50;
    int legendX = width() - legendWidth - 10;  // 10px margin from right edge
    int legendY = 10;                          // 10px from the top

    // Draw a border around the legend.
    painter.setPen(Qt::white);
    painter.drawRect(legendX, legendY, legendWidth, legendHeight);

    int boxSize = 15;
    int spacing = 5;
    int textOffset = boxSize + spacing;

    // Legend for PENDSV (red)
    painter.fillRect(legendX + spacing, legendY + spacing, boxSize, boxSize, Qt::red);
    painter.drawText(legendX + textOffset, legendY + spacing + boxSize, "PENDSV scheduled");

    // Legend for Normal (green)
    painter.fillRect(legendX + spacing, legendY + spacing*2 + boxSize, boxSize, boxSize, Qt::green);
    painter.drawText(legendX + textOffset, legendY + spacing*2 + boxSize*2, "Systick scheduled");
  }

private:
  int m_horizontalOffset;
  TraceEvent_t m_events[PACKET_RECORD_COUNT];
};

/// Main window that contains the visualization widget, slider, and serial controls.
class MainWindow : public QWidget
{
  Q_OBJECT
public:
  MainWindow(QWidget *parent = nullptr) : QWidget(parent)
  {
    QVBoxLayout *mainLayout = new QVBoxLayout(this);

    // Serial port controls layout.
    QHBoxLayout *serialLayout = new QHBoxLayout();

    m_portNameEdit = new QLineEdit(this);
    m_portNameEdit->setPlaceholderText("Serial Port (e.g., COM6)");
    m_portNameEdit->setText(QString::fromStdString(g_portName));

    m_baudRateCombo = new QComboBox(this);
    // Populate standard baud rates.
    QList<int> baudRates = {300, 600, 1200, 2400, 4800, 9600, 14400, 19200, 38400, 57600, 115200, 230400};
    for (int rate : baudRates)
      m_baudRateCombo->addItem(QString::number(rate), rate);
    int defaultIndex = m_baudRateCombo->findData(g_baudRate);
    if (defaultIndex >= 0)
      m_baudRateCombo->setCurrentIndex(defaultIndex);

    m_connectButton = new QPushButton("Connect", this);
    // Initially not connected: red background.
    m_connectButton->setStyleSheet("background-color: red");
    connect(m_connectButton, &QPushButton::clicked, this, &MainWindow::onConnectButtonClicked);

    serialLayout->addWidget(m_portNameEdit);
    serialLayout->addWidget(m_baudRateCombo);
    serialLayout->addWidget(m_connectButton);
    mainLayout->addLayout(serialLayout);

    // Visualization widget.
    m_visualizationWidget = new VisualizationWidget(this);
    mainLayout->addWidget(m_visualizationWidget);

    // Horizontal slider.
    m_slider = new QSlider(Qt::Horizontal, this);
    mainLayout->addWidget(m_slider);
    connect(m_slider, &QSlider::valueChanged,
            m_visualizationWidget, &VisualizationWidget::setHorizontalOffset);

    // Timer for updating the visualization.
    QTimer *timer = new QTimer(this);
    connect(timer, &QTimer::timeout,
            m_visualizationWidget, QOverload<>::of(&VisualizationWidget::update));
    timer->start(16);
  }

  void wheelEvent(QWheelEvent *event) override 
  {
    // Use horizontal scrolling delta if available; fallback to vertical.
    int delta = event->angleDelta().x();
    if (delta == 0)
      delta = event->angleDelta().y();
    int steps = delta;
    int newValue = m_slider->value() - steps * m_slider->singleStep();
    m_slider->setValue(newValue);
    event->accept();
  }

  void closeEvent(QCloseEvent *event) override
  {
    g_stopReceiver = true;
    if (g_receivingThread.joinable())
      g_receivingThread.join();

    event->accept();
  }

  ~MainWindow()
  {
    g_stopReceiver = true;
    if (g_receivingThread.joinable())
      g_receivingThread.join();
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
    // Toggle connection state.
    if (!g_connectionRequested.load())
    {
      // User requests connection.
      g_portName = m_portNameEdit->text().toStdString();
      g_baudRate = m_baudRateCombo->currentData().toInt();
      g_connectionRequested = true;
      g_restartNeeded = true;
      m_connectButton->setStyleSheet("background-color: green");
      m_connectButton->setText("Disconnect");
    }
    else
    {
      // User requests disconnection.
      g_connectionRequested = false;
      g_restartNeeded = true;
      m_connectButton->setStyleSheet("background-color: red");
      m_connectButton->setText("Connect");
    }
  }

private:
  VisualizationWidget *m_visualizationWidget;
  QSlider *m_slider;
  QLineEdit *m_portNameEdit;
  QComboBox *m_baudRateCombo;
  QPushButton *m_connectButton;
};

int main(int argc, char *argv[])
{
  QApplication app(argc, argv);

  // Create a single persistent receiving thread.
  g_receivingThread = std::thread(receivingTracerDataThreadFunc);

  MainWindow mainWindow;
  mainWindow.setWindowTitle("MicroKernel Tracer");
  mainWindow.resize(1280, 720);
  mainWindow.show();
  return app.exec();
}

#include "main.moc"