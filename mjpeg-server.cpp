#include "mjpeg-server.h"

#include <netdb.h>

#include <sys/types.h>
#include <sys/socket.h>

#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <thread>


#include <opencv2/opencv.hpp>

const std::size_t MJPEGServer::MAX_CLIENTS_CONNECTIONS = 16;
const std::size_t MJPEGServer::MAX_QUEUED_FRAMES = 8;
const int MJPEGServer::DEFAULT_JPEG_QUALITY = 90;

MJPEGServer::MJPEGServer(unsigned short port)
	: _port(port)
	, _isRunning(ATOMIC_FLAG_INIT)
{
}

MJPEGServer::~MJPEGServer()
{
}

void MJPEGServer::start()
{
	if (_sock != -1)
	{
		std::cerr << "Server already started." << std::endl;
		throw std::logic_error("MJPEG server already started.");
	}
	
	if ((_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) == -1)
	{
		perror("socket()");
		throw std::runtime_error("Could not start MJPEG server. Could not open socket.");
	}

	struct sockaddr_in saddr;
	saddr.sin_addr.s_addr = INADDR_ANY;
	saddr.sin_family = AF_INET;
	saddr.sin_port = htons(_port);
	
	if (bind(_sock, (struct sockaddr*)&saddr, sizeof(saddr)) == -1)
	{
		close(_sock);
		_sock = -1;		
		perror("bind()");
		throw std::runtime_error("Could not start MJPEG server. Could not bind socket.");
	}
	
	if (listen(_sock, -1) == -1)
	{
		close(_sock);
		_sock = -1;
		
		perror("listen()");
		throw std::runtime_error("Could not start MJPEG server. Could not start listening the socket.");
	}
	
	_isRunning.test_and_set(std::memory_order_relaxed);
	
	_listenWorker = std::thread(&MJPEGServer::listenWorker, this);
	_streamWorker = std::thread(&MJPEGServer::streamWorker, this);
}

void MJPEGServer::stop()
{
	_isRunning.clear(std::memory_order_relaxed);
	
	_listenWorker.join();
	shutdown(_sock, 2);
	close(_sock);
	_sock = -1;
	
	_streamWorker.join();
	for (int s : _clients)
	{
		shutdown(s, 2);
		close(s);		
	}
	
	_clients.clear();
}

void MJPEGServer::putFrame(const cv::Mat& frame)
{
	std::vector<unsigned char> payload;
	
	std::vector<int> params;
	params.push_back(cv::IMWRITE_JPEG_QUALITY);
	params.push_back(DEFAULT_JPEG_QUALITY);
	cv::imencode(".jpg", frame, payload, params);
	
	if (payload.empty())
	{
		std::lock_guard<std::mutex> lg(_outMutex);
		std::cerr << "JPEG encode failed." << std::endl;
		return;
	}
	
	std::lock_guard<std::mutex> lg(_payloadsMutex);
	while (_payloads.size() > MAX_QUEUED_FRAMES)
	{
		_payloads.pop_front();
	}
	
	_payloads.emplace_back(payload);	
}

void MJPEGServer::listenWorker()
{
	try
	{
		std::string header;
		header += "HTTP/1.0 200 OK\r\n";
		header += "Cache-Control: no-cache\r\n";
		header += "Pragma: no-cache\r\n";
		header += "Connection: close\r\n";
		header += "Content-Type: multipart/x-mixed-replace; boundary=mjpegstream\r\n\r\n";
		
		while (_isRunning.test_and_set(std::memory_order_relaxed))
		{
			struct sockaddr_in saddr;
			socklen_t slen = sizeof(saddr);
			int sock = accept(_sock, (struct sockaddr*)&saddr, &slen);
			if (sock == -1)
			{
				std::lock_guard<std::mutex> lg(_outMutex);
				perror("accept()");
				std::cerr << "Could not serve client." << std::endl;
				continue;
			}
			
			// TO DO: check the number of currently served clients,
			// respond with error, if threshold is reached

			char buffer[4096];
			int nbytes = recv(sock, buffer, sizeof(buffer), 0);
			if (nbytes < 0)
			{
				std::lock_guard<std::mutex> lg(_outMutex);
				perror("recv()");
				std::cerr << "Could not recv data from client's socket." << std::endl;
				continue;
			}
			
			buffer[nbytes] = '\0';
			
			{
				std::lock_guard<std::mutex> lg(_outMutex);
				std::cout << "Client connected (sock " << sock << "). Headers:\n" << buffer << std::endl;
				// TO DO: print endpoint of connected client
			}
			
			nbytes = send(sock, header.c_str(), header.length(), 0);
			if (nbytes < 0)
			{
				std::lock_guard<std::mutex> lg(_outMutex);
				perror("send()");
				std::cerr << "Could not send data to client's socket." << std::endl;				
				close(sock);
				continue;
			}
			
			{
				std::lock_guard<std::mutex> lg(_clientsMutex);
				_clients.push_back(sock);
			}
			
			std::this_thread::sleep_for(std::chrono::microseconds(1000));
		}		
	}
	catch (const std::exception& ex)
	{
		{
			std::lock_guard<std::mutex> lg(_outMutex);
			std::cerr << "Exception (listen worker): " << ex.what() << std::endl;
		}
	}
	catch (...)
	{
		{
			std::lock_guard<std::mutex> lg(_outMutex);
			std::cerr << "Exception (listen worker): unknown." << std::endl;
		}
	}
}

void MJPEGServer::streamWorker()
{
	try
	{
		std::string header;
		header += "--mjpegstream\r\n";
		header += "Content-Type: image/jpeg\r\n";
		header += "Content-Length: ";
		
		while (_isRunning.test_and_set(std::memory_order_relaxed))
		{
			std::array<int, MAX_CLIENTS_CONNECTIONS> clients;
			clients.fill(-1);
			std::size_t n = 0;
			
			{
				std::lock_guard<std::mutex> lg(_clientsMutex);
				if ((n = std::min(_clients.size(), MAX_CLIENTS_CONNECTIONS)) > 0)
				{
					std::copy_n(_clients.cbegin(), n, clients.begin());
				}
			}
			
			if (n != 0)
			{				
				std::vector<unsigned char> payload;
				
				{
					std::lock_guard<std::mutex> lg(_payloadsMutex);
					if (!_payloads.empty())
					{
						payload = std::move(_payloads.front());
						_payloads.pop_front();
					}
				}
				
				if (!payload.empty())
				{
					std::array<int, MAX_CLIENTS_CONNECTIONS> lostClients;
					lostClients.fill(-1);
					
					std::string hdr(header);
					hdr += std::to_string(payload.size());
					hdr += "\r\n\r\n";
					
					for (std::size_t i = 0; i < n; i++)
					{
						int s = clients[i];
						int nbytes = send(s, hdr.c_str(), hdr.length(), 0);
						if (nbytes < 0)
						{
							lostClients[i] = s;
							std::lock_guard<std::mutex> lg(_outMutex);
							perror("send()");
							std::cerr << "Could not send data (header) to client's socket." << std::endl;
							continue;
						}
						
						nbytes = send(s, payload.data(), payload.size(), 0);
						if (nbytes < 0)
						{
							lostClients[i] = s;
							std::lock_guard<std::mutex> lg(_outMutex);
							perror("send()");
							std::cerr << "Could not send data (payload) to client's socket. send() failed." << std::endl;
							continue;
						}
					}
					
					if (std::find_if(lostClients.cbegin(), lostClients.cend(), 
						[](int x){ return x != -1; }) != lostClients.cend())
					{
						std::lock_guard<std::mutex> lg(_clientsMutex);
						for (std::size_t i = 0; i < n; i++)
						{
							int s = -1;
							if ((s = lostClients[i]) != -1)
							{
								shutdown(s, 2);
								close(s);								
								_clients.remove(s);
							}
						}
					}
				}
			}
			
			std::this_thread::sleep_for(std::chrono::microseconds(1000));
		}
	}
	catch (const std::exception& ex)
	{
		{
			std::lock_guard<std::mutex> lg(_outMutex);
			std::cerr << "Exception (stream worker): " << ex.what() << std::endl;
		}
	}
	catch (...)
	{
		{
			std::lock_guard<std::mutex> lg(_outMutex);
			std::cerr << "Exception (stream worker): unknown." << std::endl;
		}
	}
}