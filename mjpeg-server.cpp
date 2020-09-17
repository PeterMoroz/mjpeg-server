#include "mjpeg-server.h"

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
#include <functional>
#include <iostream>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <thread>

#include <openssl/md5.h>


const std::size_t MJPEGServer::MAX_CLIENTS_CONNECTIONS = 16;
const std::size_t MJPEGServer::MAX_QUEUED_FRAMES = 8;


MJPEGServer::MJPEGServer(unsigned short port)
	: _port(port)
	, _isRunning(ATOMIC_FLAG_INIT)
{
	// generate opaque value for HTTP Digest authentication
	// just arbitrary string of hex-characters	
	_opaque.resize(32, ' ');
	std::srand(std::time(nullptr));
	std::generate(_opaque.begin(), _opaque.end(), 
		[]()
		{
			int r = std::rand() % 16;
			return (r >= 0 && r <= 9) ? (r + '0') : ((r - 10) + 'a');
		});
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
				
				const std::string authorizationHeader = getHeader(buffer, "Authorization");
				const std::pair<std::string, std::string> methodAndUrl = getMethodAndUrl(buffer);
				
				if (authorizationHeader.empty())
				{
					std::string authenticateHeader = digestAuthentication();
					
					if (!sendResponse(sock, 401, {{"WWW-Authenticate", authenticateHeader}, {"Content-Length", "0"} }))
					{
						std::lock_guard<std::mutex> lg(_outMutex);
						std::cerr << "Could not send response via client's socket." << std::endl;
					}					
					close(sock);
					continue;
				}
				
				if (!authorization(sock, authorizationHeader, methodAndUrl.first))
				{
					close(sock);
					continue;
				}
							
				// authorized, add headers to response				
				const std::map<std::string, std::string> headers
				{
					{ "Cache-Control", "no-cache" },
					{ "Pragma", "no-cache" },
					{ "Content-Type", "multipart/x-mixed-replace; boundary=mjpegstream" }
				};
				
				if (!sendResponse(sock, 200, headers))
				{
					std::lock_guard<std::mutex> lg(_outMutex);
					std::cerr << "Could not send response via client's socket." << std::endl;
					close(sock);
					continue;
				}	
				
				// add socket to the list of served clients				
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

std::string MJPEGServer::digestAuthentication()
{
	const bool STALE_NONCE = false;
	
	std::string authenticateHeader("Digest");
	authenticateHeader += " realm=\"" + _realm + "\"";
	authenticateHeader += ", nonce=\"" + generateNonce() + "\"";
	authenticateHeader += ", stale=" + std::string(STALE_NONCE ? "true" : "false");
	authenticateHeader += ", algorithm=MD5";
	authenticateHeader += ", qop=\"auth\"";
	authenticateHeader += ", opaque=\"" + _opaque + "\"";
	
	// std::cout << "digest auth header " << authenticateHeader << std::endl;

	return authenticateHeader;	
}

bool MJPEGServer::sendResponse(int sock, int code, 
		const std::map<std::string, std::string>& headers/* = {}*/)
{
	assert(sock > 0);
	std::string response;
	

	switch (code)
	{
	case 200:
		response = "HTTP/1.0 200 OK\r\n";
		break;
	case 400:
		response = "HTTP/1.0 400 Bad Request\r\n";
		break;
	case 401:
		response = "HTTP/1.0 401 Unauthorized\r\n";
		break;		
	case 404:
		response = "HTTP/1.0 404 Not Found\r\n";
		break;
	default:
		std::cerr << " The response " << code << " is not implemented yet." << std::endl;
		assert(false);
	}
	
	// add this header for all types of response
	response += "Connection: close\r\n";
	
	// it's caller's responsibility to provide correct header(s)
	for (const auto& kv : headers)
	{
		response += kv.first + ": " + kv.second + "\r\n";
	}
	response += "\r\n";

	// send response
	int nbytes = send(sock, response.c_str(), response.length(), 0);
	if (nbytes < 0)
	{
		std::lock_guard<std::mutex> lg(_outMutex);
		perror("send()");
		return false;
	}
	
	return true;
}

bool MJPEGServer::authorization(int sock, const std::string& header, const std::string& httpMethod)
{
	assert(sock > 0);
	assert(!header.empty());
	
	// kind of authorization
	std::size_t p = header.find_first_of(' ');
	if (p == std::string::npos)
	{
		if (!sendResponse(sock, 400, {{"Content-Length", "0"}}))
		{
			std::lock_guard<std::mutex> lg(_outMutex);
			std::cerr << "Could not send response via client's socket." << std::endl;
		}
		return false;
	}
	
	const std::string authorizationKind = header.substr(0, p);
	const std::string authorizationData = header.substr(p + 1);
	
	if (authorizationKind == "Digest")
	{
		const std::map<std::string, std::string> authData = parseAuthData(authorizationData);
		std::function<std::string (const std::string&)> getValByKey = 
			[&authData](const std::string& k)
			{
				decltype(authData)::const_iterator it = authData.find(k);
				return it != authData.cend() ? it->second : "";
			};
		
		const std::string username(getValByKey("username"));
		std::list<std::string>::const_iterator it = 
			std::find_if(_credentials.cbegin(), _credentials.cend(), 
						[&username](const std::string& credentials)
						{
							return (credentials.compare(0, username.length(), username) == 0);
						});
						
		if (it == _credentials.cend())
		{
			return false;
		}		
		
		// TO DO: no necessary get password as substring
		// 1. copy credentials to s1
		// 2. get position of ':' in s1, and insert another one after
		// 3. insert _realm between two colons
		std::size_t p = it->find_first_of(':');
		if (p == std::string::npos)
		{
			return false;
		}
		
		std::string s1(username);
		s1.push_back(':');
		s1.append(_realm);
		s1.push_back(':');
		s1.append(it->substr(p + 1));	// password
		
		
		// TO DO: check the opaque
		// TO DO: check nonce (decode received nonce, 
		// convert to seconds and compare with current time.
		// the difference should not be greater than 3 min)
		
		std::function<std::string (const std::string&)> md5Hash 
			= [](const std::string& s)
			{
				unsigned char digest[MD5_DIGEST_LENGTH];
				unsigned char* src = reinterpret_cast<unsigned char*>(const_cast<char*>(s.c_str()));
				MD5(src, s.length(), digest);

				std::ostringstream oss;
				for (size_t i = 0; i < MD5_DIGEST_LENGTH; i++)
				{
					unsigned char x = digest[i];
					oss << std::hex << std::setw(2) 
						<< std::setfill('0') << static_cast<unsigned short>(x);
				}
				return oss.str();
			};
		
		std::string h1(md5Hash(s1));	

		std::cout << "s1 = '" << s1 << "'" << std::endl;
		std::cout << "h1 = '" << h1 << "'" << std::endl;
		
		std::string s2(httpMethod);
		s2.push_back(':');
		s2.append(getValByKey("uri"));
			
		std::string h2(md5Hash(s2));
		
		std::cout << "s2 = '" << s2 << "'" << std::endl;
		std::cout << "h2 = '" << h2 << "'" << std::endl;		
		
		std::string s3(h1);
		const std::string qop(getValByKey("qop"));
		s3.push_back(':');
		s3.append(getValByKey("nonce"));
		s3.push_back(':');		
		
		if (!qop.empty())
		{
			s3.append(getValByKey("nc"));
			s3.push_back(':');
			s3.append(getValByKey("cnonce"));
			s3.push_back(':');
			s3.append(qop);
			s3.push_back(':');
		}
			
		s3.append(h2);
		
		std::string h3(md5Hash(s3));

		// std::cout << "s3 = '" << s3 << "'" << std::endl;		
		// std::cout << "h3 = '" << h3 << "'" << std::endl;			
		// std::cout << "h3 = " << h3 << ", response = " << getValByKey("response") << std::endl;
		
		return h3 == getValByKey("response");
	}
	else
	{
		// unsupported authorization
		if (!sendResponse(sock, 400, {{"Content-Length", "0"}}))
		{
			std::lock_guard<std::mutex> lg(_outMutex);
			std::cerr << "Could not send response via client's socket." << std::endl;
		}
		return false;
	}
	
	// unauthorized
	if (!sendResponse(sock, 401, {{"Content-Length", "0"}}))
	{
		std::lock_guard<std::mutex> lg(_outMutex);
		std::cerr << "Could not send response via client's socket." << std::endl;
	}
	return false;
}

std::string MJPEGServer::getHeader(const std::string& request,
									const std::string& headerName)
{
	std::string headerValue;
	
	std::size_t p = request.find(headerName);
	if (p != std::string::npos)
	{
		p += headerName.length() + 2;
		std::size_t p1 = request.find("\r\n", p);
		if (p1 != std::string::npos)
		{
			headerValue = request.substr(p, p1 - p);
		}
	}
	
	return headerValue;
}

std::pair<std::string, std::string> MJPEGServer::getMethodAndUrl(const std::string& request)
{
	std::string method;
	std::string url;
	
	std::size_t p = request.find_first_of(' ');
	if (p != std::string::npos)
	{
		method = request.substr(0, p);
		p += 1;
		std::size_t p1 = request.find_first_of(' ', p);
		if (p1 != std::string::npos)
		{
			url = request.substr(p, p1 - p);
		}
	}

	return {method, url};
}

std::string MJPEGServer::generateNonce()
{
	// the simple method:
	// 1. get current time +3 min
	// 2. convert to string, encode with base64

/* 	
	std::time_t now = std::time(nullptr);
	now += 180;
	struct tm* timeinfo = std::localtime(&now);
	
	std::ostringstream oss;
	oss << timeinfo->tm_mday << '-'
		<< timeinfo->tm_mon + 1 << '-'
		<< timeinfo->tm_year + 1900 << ' '
		<< timeinfo->tm_hour << ':'
		<< timeinfo->tm_min << ':'
		<< timeinfo->tm_sec;
		
	std::string nonce = macaron::Base64::Encode(oss.str());
	while (nonce.back() == '=')
	{
		nonce.pop_back();
	}
	
	return nonce;
	* */
	
	// TO DO: return back to previos method, 
	// when base64 implementation will be ready
	
	// generate nonce value for HTTP Digest authentication
	// just arbitrary string of hex-characters	
	std::string nonce(32, ' ');
	std::srand(std::time(nullptr));
	std::generate(nonce.begin(), nonce.end(), 
		[]()
		{
			int r = std::rand() % 16;
			return (r >= 0 && r <= 9) ? (r + '0') : ((r - 10) + 'a');
		});	
	return nonce;
}

std::map<std::string, std::string> MJPEGServer::parseAuthData(const std::string& data)
{
	std::map<std::string, std::string> kvData;
	
	std::function<std::pair<std::string, std::string>(const std::string&)> getKeyValue = 
		[](const std::string& s)
	{
		std::string k, v;
		std::size_t p = s.find_first_of('=');
		if (p == std::string::npos)
		{
			return std::make_pair(k, v);
		}
		
		k = s.substr(0, p);
		p += 1;
		
		while (p < s.length() && s[p] == '\"')
		{
			p += 1;
		}
		
		v = s.substr(p);		
		while (v.back() == '\"')
		{
			v.pop_back();
		}
		
		return std::make_pair(k, v);
	};
	
	std::size_t p0 = 0, p1 = 0;
	
	p1 = data.find_first_of(',');
	if (p1 == std::string::npos)
	{
		p1 = data.length();
	}	
	
	std::size_t len = p1 - p0;
	while (len != 0)
	{
		std::string tmp(data.substr(p0, len));
		std::pair<std::string, std::string> kv = getKeyValue(tmp);
		if (!kv.first.empty() && !kv.second.empty())
		{
			kvData.emplace(kv.first, kv.second);
		}
		
		if (p1 == data.length())
		{
			break;
		}
		
		p1 += 1;
		while (p1 < data.length() && data[p1] == ' ')
		{
			p1 += 1;
		}
		
		if (p1 == data.length())
		{
			break;
		}
		
		p0 = p1;
		p1 = data.find_first_of(',', p0);
		if (p1 == std::string::npos)
		{
			p1 = data.length();
		}
		
		len = p1 - p0;
	}
	
	return kvData;
}
