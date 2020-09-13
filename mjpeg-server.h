#pragma once

#include <atomic>
#include <list>
#include <mutex>
#include <thread>
#include <vector>


class MJPEGServer final
{
	static const std::size_t MAX_CLIENTS_CONNECTIONS;
	static const std::size_t MAX_QUEUED_FRAMES;
	
public:
	MJPEGServer(const MJPEGServer&) = delete;
	MJPEGServer& operator=(const MJPEGServer&) = delete;
	
	explicit MJPEGServer(unsigned short port);
	~MJPEGServer();
	
	void start();
	void stop();
	void putFrame(const std::vector<unsigned char>& frame);
	
	void setCredentials(const std::list<std::string>& credentials)
	{
		_credentials = credentials;
	}
	
private:
	void listenWorker();
	void streamWorker();
	
private:
	unsigned short _port = 0;
	int _sock = -1;
	
	std::list<int> _clients;
	std::list<std::vector<unsigned char>> _payloads;
	
	std::list<std::string> _credentials;
	
	std::atomic_flag _isRunning;
	std::thread _listenWorker;
	std::thread _streamWorker;
	
	std::mutex _outMutex;
	std::mutex _clientsMutex;
	std::mutex _payloadsMutex;
};
