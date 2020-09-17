#include <csignal>
#include <cstring>
#include <cstdlib>

#include <fstream>
#include <functional>
#include <iostream>
#include <list>
#include <string>


#include "mjpeg-server.h"
#include "v4l2-camera.h"

#include <getopt.h>


sig_atomic_t needExit = 0;

static void sighandler(int signum)
{
	if (signum == SIGINT || signum == SIGTERM)
	{
		needExit = 1;
	}
}


int main(int argc, char* argv[])
{
	const char* short_options = "c::h";
	
	const struct option long_options[] = 
	{
		{ "credentials", required_argument, NULL, 'c' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 }
	};
	
	std::function<void (void)> usage = 
		[argv]()
		{
			std::cout << "usage: " << argv[0] 
				<< " --credentials <path-to-file> " << std::endl;
		};
	
	int rez = -1;
	
	std::string credentialsPath;
	while ((rez = getopt_long_only(argc, argv, short_options, long_options, NULL)) != -1)
	{
		switch (rez)
		{
		case 'c':
			credentialsPath = optarg;
			break;
			
		case 'h':
			usage();
			std::exit(EXIT_SUCCESS);
			break;
			
		default:
			std::cout << "Unknoun option '" 
				<< static_cast<char>(rez) << "'" << std::endl;
			usage();
			std::exit(EXIT_FAILURE);			
		}
	}
	
	if (credentialsPath.empty())
	{
		std::cerr << "The path to credentials is not specified." << std::endl;
		usage();
		std::exit(EXIT_FAILURE);
	}
	
		
	struct sigaction sigact;
	memset(&sigact, 0, sizeof(sigact));
	
	sigact.sa_handler = sighandler;
	sigset_t sigset;
	sigemptyset(&sigset);
	sigaddset(&sigset, SIGINT);
	sigaddset(&sigset, SIGTERM);
	sigact.sa_mask = sigset;
	int rc = -1;
	rc = sigaction(SIGINT, &sigact, NULL);
	rc = sigaction(SIGTERM, &sigact, NULL);
	
	sigemptyset(&sigset);
	sigaddset(&sigset, SIGPIPE);
	rc = sigprocmask(SIG_BLOCK, &sigset, NULL);
	
	try
	{
		// read credentials		
		std::ifstream ifs(credentialsPath);
		if (!ifs)
		{
			std::cerr << "Could not open file: " << credentialsPath << std::endl;
			std::exit(EXIT_FAILURE);
		}
		
		std::list<std::string> credentials;
		std::string line;
		while (std::getline(ifs, line))
		{
			if (line.empty())
			{
				break;
			}
			
			credentials.emplace_back(line);
		}
		
		// setup camera
		V4L2Camera v4l2Camera;
		
		v4l2Camera.openDevice("/dev/video0");
		v4l2Camera.printCapabilities();
		v4l2Camera.setupCaptureFormat();
		v4l2Camera.setupCaptureBuffer();
						
		MJPEGServer mjpegServer(8090);
		mjpegServer.setCredentials(credentials);
		
		// start server
		mjpegServer.start();

		while (!needExit)
		{
			std::vector<unsigned char> frame = v4l2Camera.captureFrame();
			if (!frame.empty())
			{
				mjpegServer.putFrame(frame);
			}
		}
		
		std::cout << "Stopping the server..." << std::endl;
		mjpegServer.stop();
	}
	catch (const std::exception& ex)
	{
		std::cerr <<  "Exception: " << ex.what() << std::endl;
		std::exit(EXIT_FAILURE);
	}
	
	return 0;
}
