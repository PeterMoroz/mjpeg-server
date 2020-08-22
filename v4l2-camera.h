#pragma once

#include <vector>

class V4L2Camera final
{
public:
	V4L2Camera(const V4L2Camera&) = delete;
	V4L2Camera& operator=(const V4L2Camera&) = delete;
	
	V4L2Camera() = default;
	~V4L2Camera();
	
	void openDevice(const char* deviceName);
	void printCapabilities();
	void setupCaptureFormat();
	void setupCaptureBuffer();
	
	void stopCapturing();
	
	std::vector<unsigned char> captureFrame();
	
private:
	int ioctl(int request, void* arg);
	
private:
	int _fd = -1;
	unsigned char* _buffer = NULL;
	size_t _bufferLength = 0;
};
