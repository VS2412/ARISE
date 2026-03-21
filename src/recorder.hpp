#pragma once
#include <string>
#include <sys/types.h>

class Recorder {
public:
    explicit Recorder(const std::string& outputPath);
    void toggle();
    bool isRecording() const;
    const std::string& getOutputPath() const;
private:
    void start();
    void stop();  // used toggle in cpp file
    std::string outputPath;
    pid_t recordPid{-1};
};