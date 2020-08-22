#include <csignal>
#include <cstdlib>

#include <chrono>
#include <iostream>


#include "mjpeg-server.h"
#include "v4l2-camera.h"


sig_atomic_t needExit = 0;


int main(int argc, char* argv[])
{
	// TO DO: setup signals: SIGINT, SIGTERM, ignore SIGPIPE
	
	unsigned framesCount = 0;
	
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
			mjpegServer.putFrame(frame);
		}		
		
		mjpegServer.stop();		
	}
	catch (const std::exception& ex)
	{
		std::cerr <<  "Exception: " << ex.what() << std::endl;
		std::exit(EXIT_FAILURE);
	}
	
	return 0;
}
