#include <csignal>
#include <cstring>
#include <cstdlib>

#include <iostream>


#include "mjpeg-server.h"
#include "v4l2-camera.h"


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
	// TO DO: setup signals: SIGINT, SIGTERM, ignore SIGPIPE
	
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
		V4L2Camera v4l2Camera;
		
		v4l2Camera.openDevice("/dev/video0");
		v4l2Camera.printCapabilities();
		v4l2Camera.setupCaptureFormat();
		v4l2Camera.setupCaptureBuffer();
				
		MJPEGServer mjpegServer(8090);
		
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
