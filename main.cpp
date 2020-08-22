#include <csignal>
#include <cstdlib>

#include <chrono>
#include <iostream>
#include <thread>

#include <opencv2/opencv.hpp>


#include "mjpeg-server.h"


sig_atomic_t needExit = 0;


int main(int argc, char* argv[])
{
	// TO DO: setup signals: SIGINT, SIGTERM, ignore SIGPIPE
	
	unsigned framesCount = 0;
	
	try
	{	
		cv::VideoCapture vc;
		unsigned openCount = 0;
		static const unsigned MAX_OPEN_COUNT = 5;
		while (openCount < MAX_OPEN_COUNT)
		{
			if (vc.open(0))
			{
				break;
			}
			std::cout << "Could not open video capture device. "
				" Waiting for 100 ms ..." << std::endl;
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
			openCount += 1;
		}
		
		if (!vc.isOpened())
		{
			std::cerr << "Could not open video capture device. " << std::endl;
			std::exit(EXIT_FAILURE);
		}
		
		MJPEGServer mjpegServer(8090);
		
		mjpegServer.start();

		while (!needExit)
		{
			if (!vc.isOpened())
			{
				std::cerr << "Video capture device is lost." << std::endl;
				break;	// TO DO: reconnect
			}
			
			cv::Mat frame;
			vc >> frame;
			mjpegServer.putFrame(frame);
			frame.release();
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
