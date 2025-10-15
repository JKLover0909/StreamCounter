#include <opencv2/opencv.hpp>
#include <iostream>

int main() {
    cv::VideoCapture cap(0);
    if (!cap.isOpened()) {
        std::cerr << "Cannot open camera 0\n";
        return 1;
    }

    cv::Mat frame;
    if (!cap.read(frame) || frame.empty()) {
        std::cerr << "Failed to capture frame\n";
        return 1;
    }

    std::string out = "capture_first.jpg";
    if (!cv::imwrite(out, frame)) {
        std::cerr << "Failed to write " << out << "\n";
        return 1;
    }

    std::cout << "Saved: " << out << "\n";
    return 0;
}