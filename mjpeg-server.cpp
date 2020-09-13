#include "mjpeg-server.h"
#include "Base64.h"

#include <fcntl.h>
#include <netdb.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#include <unistd.h>

#include <cassert>
#include <cstring>

#include <algorithm>
#include <array>
#include <chrono>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <thread>


const std::size_t MJPEGServer::MAX_CLIENTS_CONNECTIONS = 16;
const std::size_t MJPEGServer::MAX_QUEUED_FRAMES = 8;

namespace
{
	std::string base64Decode(const std::string& str)
	{
		assert(!str.empty());
		const std::size_t numBlocks = str.length() / 4;
		std::string res(numBlocks * 3, ' ');
		
		const char BASE64_ALPHABET[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
		
		std::size_t i = 0, j = 0;
		while (i < str.length())
		{
			std::uint32_t y = 0;
			std::uint8_t sz = 0;
			for (std::size_t k = 0; k < 4; k++)
			{
				char x = str[i + k];
				if (x == '=')
				{					
					continue;
				}
				
				const char* p = std::find(std::cbegin(BASE64_ALPHABET), std::cend(BASE64_ALPHABET), x);
				if (p == std::cend(BASE64_ALPHABET))
				{
					std::cerr << "Could not find '" << x << "' in base64 alphabet!" << std::endl;
					throw std::invalid_argument("The input string contains non base64 character(s).");
				}
				
				std::size_t idx = std::distance(std::cbegin(BASE64_ALPHABET), p);
				y |= (idx & 0x3F);
				if (k < 3)
				{
					y <<= 6;
				}
				sz += 6;
			}

			switch (sz)
			{
			case 24:
			case 18:
				res[j++] = (y >> 16) & 0x7F;
				res[j++] = (y >> 8) & 0x7F;
				res[j++] = y & 0x7F;
				break;
			case 12:
				res[j++] = (y >> 8) & 0x7F;
				res[j++] = y & 0x7F;
				break;
			case 6:
				res[j++] = y & 0x7F;
				break;
			default:
				throw std::logic_error("Wrong size of data chunk when decodind base64 string.");
			}
					
			i += 4;
		}
		
		return res;
	}
}


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
	
	if (fcntl(_sock, F_SETFL, O_NONBLOCK) == -1)
	{
		perror("fcntl()");
		close(_sock);
		_sock = -1;
		throw std::runtime_error("Could not start MJPEG server. "
								"Could not set nonblocking socket mode.");
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

void MJPEGServer::putFrame(const std::vector<unsigned char>& frame)
{
	assert(!frame.empty());
	
	std::lock_guard<std::mutex> lg(_payloadsMutex);
	while (_payloads.size() > MAX_QUEUED_FRAMES)
	{
		_payloads.pop_front();
	}
	
	_payloads.emplace_back(frame);
}

void MJPEGServer::listenWorker()
{
	try
	{
		std::string header200;
		header200 += "HTTP/1.0 200 OK\r\n";
		header200 += "Cache-Control: no-cache\r\n";
		header200 += "Pragma: no-cache\r\n";
		header200 += "Connection: close\r\n";
		header200 += "Content-Type: multipart/x-mixed-replace; boundary=mjpegstream\r\n\r\n";
		
		std::string header400;
		header400 += "HTTP/1.0 400 Bad Request\r\n";	
		header400 += "Connection: close\r\n";
		header400 += "Content-Length: 0\r\n\r\n";		
		
		std::string header401;
		header401 += "HTTP/1.0 401 Unauthorized\r\n";	
		header401 += "WWW-Authenticate: Basic realm=\"Basic Auth Testing\"\r\n";
		header401 += "Connection: close\r\n";
		header401 += "Content-Length: 0\r\n\r\n";
		
		fd_set fds;
		
		while (_isRunning.test_and_set(std::memory_order_relaxed))
		{
			FD_ZERO(&fds);
			FD_SET(_sock, &fds);
			
			struct timeval tv = { 0 };
			tv.tv_sec = 4;
			
			if (select(_sock + 1, &fds, NULL, NULL, &tv) == -1)
			{
				std::lock_guard<std::mutex> lg(_outMutex);
				perror("select()");
				continue;
			}
			
			if (FD_ISSET(_sock, &fds))
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

				const char* cltAddrIP = inet_ntoa(saddr.sin_addr);
				
				{
					std::lock_guard<std::mutex> lg(_outMutex);
					std::cout << "Client connected (sock " << sock << "). IP " << cltAddrIP << std::endl;
					std::cout << "Headers:\n" << buffer << std::endl;
				}
				
				const char* authHeader = strstr(buffer, "Authorization: Basic ");
				if (authHeader == NULL)
				{
					nbytes = send(sock, header401.c_str(), header401.length(), 0);
					if (nbytes < 0)
					{
						std::lock_guard<std::mutex> lg(_outMutex);
						perror("send()");
						std::cerr << "Could not send data to client's socket." << std::endl;				
					}
					
					close(sock);
					continue;
				}
				
				const char* credentialsBegin = authHeader += strlen("Authorization: Basic ");
				const char* credentialsEnd = strstr(credentialsBegin, "\r\n");
				if (credentialsEnd == NULL)
				{
					nbytes = send(sock, header400.c_str(), header400.length(), 0);
					if (nbytes < 0)
					{
						std::lock_guard<std::mutex> lg(_outMutex);
						perror("send()");
						std::cerr << "Could not send data to client's socket." << std::endl;
					}
					
					close(sock);
					continue;
				}
				
				std::string credentialsBase64(credentialsBegin, credentialsEnd);
				while (credentialsBase64.length() % 4 != 0)
				{
					credentialsBase64.push_back('=');
				}

/*
				try
				{					
					credentials = base64Decode(credentials);
				}
				catch (const std::exception& ex)
				{
					{
						std::lock_guard<std::mutex> lg(_outMutex);
						std::cerr << "Exception: " << ex.what() << std::endl;
					}
					
					nbytes = send(sock, header400.c_str(), header400.length(), 0);
					if (nbytes < 0)
					{
						std::lock_guard<std::mutex> lg(_outMutex);
						perror("send()");
						std::cerr << "Could not send data to client's socket." << std::endl;
					}
					
					close(sock);
					continue;					
				}
				* */
				
				std::string credentials;
				const std::string decodeError = macaron::Base64::Decode(credentialsBase64, credentials);
				if (!decodeError.empty())
				{				
					{
						std::lock_guard<std::mutex> lg(_outMutex);
						std::cerr << "An error when decoding credentials: " << decodeError 
							<< ", base64 string is " << credentialsBase64 << std::endl;
					}
					
					nbytes = send(sock, header400.c_str(), header400.length(), 0);
					if (nbytes < 0)
					{
						std::lock_guard<std::mutex> lg(_outMutex);
						perror("send()");
						std::cerr << "Could not send data to client's socket." << std::endl;
					}
					
					close(sock);
					continue;
				}
				
				std::list<std::string>::const_iterator it = 
					std::find(_credentials.cbegin(), _credentials.cend(), credentials);
				if (it == _credentials.cend())
				{
					{
						std::lock_guard<std::mutex> lg(_outMutex);
						std::cout << "The pair user:password is unknown " << credentials << std::endl;
					}
										
					nbytes = send(sock, header401.c_str(), header401.length(), 0);
					if (nbytes < 0)
					{
						std::lock_guard<std::mutex> lg(_outMutex);
						perror("send()");
						std::cerr << "Could not send data to client's socket." << std::endl;				
					}
					
					close(sock);
					continue;					
				}
				
				nbytes = send(sock, header200.c_str(), header200.length(), 0);
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
			}
		
			std::this_thread::sleep_for(std::chrono::microseconds(1000));
		}
		
		_isRunning.clear(std::memory_order_relaxed);
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
		
		_isRunning.clear(std::memory_order_relaxed);
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
