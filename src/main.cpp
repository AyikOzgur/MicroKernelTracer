#include <windows.h>
#include <iostream>
#include <array>
#include <cstring>
#include <thread>
#include <opencv2/opencv.hpp>

#include "SerialPort.h"

// Define the packed structure.
#pragma pack(push, 1)
struct TraceEvent_t 
{
    uint16_t deltaTime : 10;  // 10 bits for delta time (0–1023)
    uint16_t eventType : 2;   // 2 bits for event type (0–3)
    uint16_t threadId  : 4;   // 4 bits for thread ID (0–15)
};
#pragma pack(pop)

const int PACKET_RECORD_COUNT = 1023;
const int BUFFER_SIZE = PACKET_RECORD_COUNT * sizeof(TraceEvent_t);

std::array<TraceEvent_t, PACKET_RECORD_COUNT> g_sharedEvents;
std::mutex g_sharedEventsMutex;



void receivingTracerDataThreadFunc();

int main() 
{
    // Start a thread to receive data from the tracer.
    std::thread receivingThread(receivingTracerDataThreadFunc);
    receivingThread.detach();

    // Create an OpenCV window.
    const int segmentWidth = 50; // Each event occupies 50 pixels horizontally.
    const int barHeight = 20;    // Height of each thread bar.
    const int leftMargin = 100;  // Left margin reserved for labels.
    const int imageWidth = 1280; // Width of the image.
    const int imageHeight = 720; // Height of the image.
    cv::namedWindow("Scheduling Visualization", cv::WINDOW_AUTOSIZE);
    std::array<TraceEvent_t, PACKET_RECORD_COUNT> events;
    cv::Mat image(imageHeight, imageWidth, CV_8UC3, cv::Scalar(0, 0, 0));

    while (true) 
    {
        g_sharedEventsMutex.lock();
        events = g_sharedEvents;
        g_sharedEventsMutex.unlock();

        // Clear the image.
        image = cv::Scalar(0, 0, 0);

        // Print only 9
        for (size_t i = 0; i < 9; ++i) 
        {
            int tid = events[i].threadId;
            if (tid >= 0 && tid < 3) 
            {  // We visualize only threads 0, 1, and 2.
                int x = static_cast<int>(leftMargin + i * segmentWidth);
                int y = tid * barHeight + 300;  // Y-offset for the thread's bar.
                cv::rectangle(image, cv::Rect(x, y, segmentWidth, barHeight),
                                cv::Scalar(0, 255, 0), cv::FILLED);

                // Put delta time as text inside the bar.
                std::string text = std::to_string(events[i].deltaTime);
                cv::putText(image, text, cv::Point(x + 5, y + 15),
                            cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(0, 0, 0), 1);
            }
        }

        // For each thread (0, 1, 2), draw a label centered in its bar.
        int fontFace = cv::FONT_HERSHEY_SIMPLEX;
        double fontScale = 0.5;
        int thickness = 1;
        const int baseY = 300;       // Y offset where the first bar starts.

        for (int tid = 0; tid < 3; ++tid) 
        {
            std::string label = "Thread " + std::to_string(tid);
            int barY = baseY + tid * barHeight;        // Starting y coordinate for this bar.
            int barCenter = barY + barHeight / 2;      // Vertical center of the bar.
            
            int baseline = 0;
            cv::Size textSize = cv::getTextSize(label, fontFace, fontScale, thickness, &baseline);
            
            // Compute y coordinate for putText:
            // We want the center of the text bounding box (which is y - textSize.height/2)
            // to match the barCenter. So:
            int textY = barCenter + textSize.height / 2 + tid * 2;
            
            cv::putText(image, label, cv::Point(10, textY),
                        fontFace, fontScale, cv::Scalar(255, 255, 255), thickness);
        }
        // Display the visualization.
        cv::imshow("Scheduling Visualization", image);
        // Wait a little bit; exit if the ESC key is pressed.
        if (cv::waitKey(1) == 27)
            break;
    }

    return 0;
}

void receivingTracerDataThreadFunc()
{
    // Open the serial port.
    SerialPort serial;
    if (!serial.open("COM5")) 
    {
        std::cerr << "Error opening COM5." << std::endl;
        exit(1);
    }

    std::cout << "Listening on COM5..." << std::endl;
    
    // We expect one packet to consist of 1023 records, each of size 2 bytes.
    // Buffer to read data (assumed to be delivered at once).
    char buffer[BUFFER_SIZE];
    DWORD bytesRead = 0;

    // Array to store parsed records.
    std::array<TraceEvent_t, PACKET_RECORD_COUNT> events;

    while(true)
    {
        int bytesRead = serial.read(reinterpret_cast<uint8_t*>(buffer), BUFFER_SIZE);
        if (bytesRead >= BUFFER_SIZE) 
        {
            // Parse the first PACKET_SIZE bytes as 1023 complete records.
            size_t recordSize = sizeof(TraceEvent_t);  // Should be 2 bytes.
            for (size_t i = 0; i < PACKET_RECORD_COUNT; ++i) 
            {
                TraceEvent_t record;
                std::memcpy(&record, buffer + i * recordSize, recordSize);
                events[i] = record;
            }

            g_sharedEventsMutex.lock();
            g_sharedEvents = events;
            g_sharedEventsMutex.unlock();
        }
    }
}