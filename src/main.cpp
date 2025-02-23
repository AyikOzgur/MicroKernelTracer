#include <windows.h>
#include <iostream>
#include <array>
#include <cstring>
#include <thread>
#include <mutex>
#include "SerialPort.h"
#include "utils.h"

#include <QApplication>
#include <QWidget>
#include <QPainter>
#include <QTimer>
#include <QString>
#include <QFontMetrics>
#include <QVBoxLayout>
#include <QSlider>
#include <QResizeEvent>

std::string g_portName = "COM6";

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
void receivingTracerDataThreadFunc();

// Custom widget that performs the drawing.
class VisualizationWidget : public QWidget
{
  Q_OBJECT
public:
  explicit VisualizationWidget(QWidget *parent = nullptr)
      : QWidget(parent), m_horizontalOffset(0)
  {
    // Optionally, set a minimum height.
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

    TraceEvent_t *events = new TraceEvent_t[PACKET_RECORD_COUNT];
    g_sharedEventsMutex.lock();
    std::memcpy(events, g_sharedEvents, BUFFER_SIZE);
    g_sharedEventsMutex.unlock();

    QPainter painter(this);
    painter.fillRect(rect(), Qt::black);

    const int segmentWidth = 50; // Each event occupies 50 pixels horizontally.
    const int barHeight = 20;    // Height of each thread bar.
    const int leftMargin = 100;  // Margin reserved for thread labels.
    const int baseY = 300;       // Y offset where the bars start.

    // Draw all events in the buffer using the horizontal offset.
    for (size_t i = 0; i < PACKET_RECORD_COUNT; ++i)
    {
      int tid = events[i].threadId;
      if (tid >= 0 && tid < 3) // Only visualize threads 0, 1, and 2.
      {
        // Calculate x-position with the horizontal scrolling offset.
        int x = leftMargin + static_cast<int>(i) * segmentWidth - m_horizontalOffset;
        int y = baseY + tid * barHeight;
        QRect rect(x, y, segmentWidth, barHeight);
        // Only draw if the rectangle is within the visible region.
        if (rect.right() >= leftMargin && rect.left() <= width())
        {
          painter.fillRect(rect, Qt::green);
          QString text = QString::number(events[i].deltaTime);
          painter.setPen(Qt::black);
          painter.drawText(rect.adjusted(5, 0, 0, 0), Qt::AlignVCenter | Qt::AlignLeft, text);
        }
      }
    }

    // Draw fixed thread labels to the left.
    for (int tid = 0; tid < 3; ++tid)
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
  }

private:
  int m_horizontalOffset;
};

// Main window that contains the visualization widget and a horizontal slider.
class MainWindow : public QWidget
{
  Q_OBJECT
public:
  MainWindow(QWidget *parent = nullptr) : QWidget(parent)
  {
    QVBoxLayout *layout = new QVBoxLayout(this);

    m_visualizationWidget = new VisualizationWidget(this);
    layout->addWidget(m_visualizationWidget);

    m_slider = new QSlider(Qt::Horizontal, this);
    layout->addWidget(m_slider);

    // Connect the slider to update the horizontal offset in the visualization widget.
    connect(m_slider, &QSlider::valueChanged, m_visualizationWidget, &VisualizationWidget::setHorizontalOffset);

    // Timer to update the visualization (if needed).
    QTimer *timer = new QTimer(this);
    connect(timer, &QTimer::timeout, m_visualizationWidget, QOverload<>::of(&VisualizationWidget::update));
    timer->start(16);
  }

protected:
  // Recalculate slider range when the window is resized.
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

private:
  VisualizationWidget *m_visualizationWidget;
  QSlider *m_slider;
};

int main(int argc, char *argv[])
{
  QApplication app(argc, argv);

  // Start the thread to receive tracer data.
  std::thread receivingThread(receivingTracerDataThreadFunc);
  receivingThread.detach();

  MainWindow mainWindow;
  mainWindow.setWindowTitle("Scheduling Visualization");
  mainWindow.resize(1280, 720);
  mainWindow.show();

  return app.exec();
}

void receivingTracerDataThreadFunc()
{
  SerialPort serial;
  if (!serial.open(g_portName))
  {
    std::cerr << LOG_LOCATION <<"Error opening serial port : " << g_portName << std::endl;
    exit(1);
  }

  std::cout << "Listening on " << g_portName << std::endl;

  // Buffer to read one packet (1023 records).
  char buffer[BUFFER_SIZE];

  while (true)
  {
    int bytesRead = serial.read(reinterpret_cast<uint8_t *>(buffer), BUFFER_SIZE);
    if (bytesRead >= BUFFER_SIZE)
    {
      g_sharedEventsMutex.lock();
      std::memcpy(g_sharedEvents, buffer, BUFFER_SIZE);
      g_sharedEventsMutex.unlock();
    }
  }
}

#include "main.moc"