#pragma once

#include <atomic>
#include <list>
#include <mutex>
#include <thread>
#include <vector>


namespace cv
{
	class Mat;
}


class MJPEGServer final
{
	static const std::size_t MAX_CLIENTS_CONNECTIONS;
	static const std::size_t MAX_QUEUED_FRAMES;
	static const int DEFAULT_JPEG_QUALITY;
	
public:
	MJPEGServer(const MJPEGServer&) = delete;
	MJPEGServer& operator=(const MJPEGServer&) = delete;
	
	explicit MJPEGServer(unsigned short port);
	~MJPEGServer();
	
	void start();
	void stop();
	void putFrame(const cv::Mat& frame);
	
private:
	void listenWorker();
	void streamWorker();
	
private:
	unsigned short _port = 0;
	int _sock = -1;
	
	std::list<int> _clients;
	std::list<std::vector<unsigned char>> _payloads;
	
	
	std::atomic_flag _isRunning;
	std::thread _listenWorker;
	std::thread _streamWorker;
	
	std::mutex _outMutex;
	std::mutex _clientsMutex;
	std::mutex _payloadsMutex;
};
