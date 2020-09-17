#pragma once

#include <atomic>
#include <list>
#include <map>
#include <mutex>
#include <string>
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
	
	std::string digestAuthentication();
	bool sendResponse(int sock, int code, const std::map<std::string, std::string>& headers = {});
	
	bool authorization(int sock, const std::string& header, const std::string& httpMethod);
	
	static std::string getHeader(const std::string& request,
								const std::string& headerName);								
	// split request (the first line) into method:url:protocol. return method and url
	static std::pair<std::string, std::string> getMethodAndUrl(const std::string& request);
	
	static std::string generateNonce();
	
	static std::map<std::string, std::string> parseAuthData(const std::string& data);
	
	
private:
	unsigned short _port = 0;
	int _sock = -1;
	
	std::list<int> _clients;
	std::list<std::vector<unsigned char>> _payloads;	
	
	std::list<std::string> _credentials;
	std::string _realm = "mjpeg server";
	std::string _opaque;
	
	std::atomic_flag _isRunning;
	std::thread _listenWorker;
	std::thread _streamWorker;
	
	std::mutex _outMutex;
	std::mutex _clientsMutex;
	std::mutex _payloadsMutex;
};
