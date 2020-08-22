#include "v4l2-camera.h"

#include <cassert>
#include <cstring>

#include <iomanip>
#include <iostream>
#include <stdexcept>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include <unistd.h>

#include <linux/videodev2.h>


V4L2Camera::~V4L2Camera()
{
	if (_bufferLength != 0)
	{
		munmap(_buffer, _bufferLength);
	}
		
	if (_fd != -1)
	{
		close(_fd);
	}	
}

void V4L2Camera::openDevice(const char* deviceName)
{
	if (_fd != -1)
	{
		close(_fd);
		_fd = -1;
	}
	
	int fd = open(deviceName, O_RDWR);
	if (fd == -1)
	{
		perror("open()");
		throw std::runtime_error("Could not open device.");
	}
	
	_fd = fd;
}

void V4L2Camera::printCapabilities()
{
	struct v4l2_capability caps = { 0 };
	
	if (V4L2Camera::ioctl(VIDIOC_QUERYCAP, &caps) == -1)
	{
		throw std::runtime_error("Could not query device capabilities.");
	}
	
	const std::ios_base::fmtflags fmtFlags = std::cout.flags();
	
	std::cout << "Driver caps: \n"
		<< " driver: " << caps.driver << '\n'
		<< " card: " << caps.card << '\n'
		<< " bus: " << caps.bus_info << '\n'
		<< " version: " << ((caps.version >> 16) & 0xFF) << '.'
						<< ((caps.version >> 24) & 0xFF) << '\n'
		<< " capabilities: " 
		<< std::setw(8) << std::setfill('0') << std::hex << caps.capabilities
		<< std::endl;
		
	std::cout.flags(fmtFlags);
		
		
	struct v4l2_cropcap cropcaps = { 0 };
	cropcaps.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	
	if (V4L2Camera::ioctl(VIDIOC_CROPCAP, &cropcaps) == -1)
	{
		throw std::runtime_error("Could not query cropping capabilities.");
	}
	
	std::cout << "Camera cropping: \n"
		<< " bounds: " << cropcaps.bounds.width << 'x' << cropcaps.bounds.height 
		<< '+' << cropcaps.bounds.left << '+' << cropcaps.bounds.top << '\n'
		<< " default: " << cropcaps.defrect.width << 'x' << cropcaps.defrect.height
		<< '+' << cropcaps.defrect.left << '+' << cropcaps.defrect.top << '\n'
		<< " aspect: " << cropcaps.pixelaspect.numerator << '/' << cropcaps.pixelaspect.denominator
		<< std::endl;
		
	struct v4l2_fmtdesc fmtdesc = { 0 };
	fmtdesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	char fourcc[5] = { 0 };
	char c, e;
	
	std::cout << "  Format | CE | Description\n"
			<< "----------------------------\n";
	while (V4L2Camera::ioctl(VIDIOC_ENUM_FMT, &fmtdesc) == 0)
	{
		strncpy(fourcc, (char*)&fmtdesc.pixelformat, 4);
		c = fmtdesc.flags & 1 ? 'C' : ' ';
		e = fmtdesc.flags & 2 ? 'E' : ' ';
		std::cout << "  " << fourcc << "   | " << c << e << " | " 
			<< fmtdesc.description << std::endl;
		fmtdesc.index++;
	}
	
	std::cout << std::endl;
}

void V4L2Camera::setupCaptureFormat()
{
	struct v4l2_format fmt = { 0 };
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	fmt.fmt.pix.width = 640;
	fmt.fmt.pix.height = 480;
	fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
	fmt.fmt.pix.field = V4L2_FIELD_NONE;
	
	if (V4L2Camera::ioctl(VIDIOC_S_FMT, &fmt) == -1)
	{
		throw std::runtime_error("Could not set capture format.");
	}
	
	char fourcc[5] = { 0 };
	strncpy(fourcc, (char*)&fmt.fmt.pix.pixelformat, 4);
	std::cout << "Selected camera mode: \n"
		<< " width: " << fmt.fmt.pix.width << '\n'
		<< " height: " << fmt.fmt.pix.height << '\n'
		<< " format: " << fourcc << '\n'
		<< " field: " << fmt.fmt.pix.field << std::endl;
}

void V4L2Camera::setupCaptureBuffer()
{
	/* TO DO: benchmark capturing into memory mapped buffer,
	 * implement capturing into user buffer, benchmark also.
	 * */
	struct v4l2_requestbuffers req = { 0 };
	req.count = 1;
	req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	req.memory = V4L2_MEMORY_MMAP;
	
	if (V4L2Camera::ioctl(VIDIOC_REQBUFS, &req) == -1)
	{
		throw std::runtime_error("Could not request capture buffer.");
	}
	
	
	struct v4l2_buffer buf = { 0 };
	buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	buf.memory = V4L2_MEMORY_MMAP;
	buf.index = 0;
	
	if (V4L2Camera::ioctl(VIDIOC_QUERYBUF, &buf) == -1)
	{
		throw std::runtime_error("Could not query capture buffer.");
	}

	void* buffer = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, 
						MAP_SHARED, _fd, buf.m.offset);
							
	if (buffer == MAP_FAILED)
	{
		perror("mmap()");
		throw std::runtime_error("Could not map device file to memory.");
	}
	
	_buffer = static_cast<unsigned char*>(buffer);
	_bufferLength = buf.length;

	const std::ios_base::fmtflags fmtFlags = std::cout.flags();
	
	std::cout << "Buffer: \n"
		<< " address: " << std::setw(8) << std::setfill('0') << std::hex << _buffer << '\n';
	std::cout.flags(fmtFlags);
	std::cout << " length: " << _bufferLength << std::endl;
	std::cout << "Image size (bytes): " << buf.bytesused << std::endl;	
}

void V4L2Camera::stopCapturing()
{
	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	
	if (V4L2Camera::ioctl(VIDIOC_STREAMOFF, &type) == -1)
	{
		throw std::runtime_error("Could not stop capturing.");
	}
}

std::vector<unsigned char> V4L2Camera::captureFrame()
{
	struct v4l2_buffer buf = { 0 };
	buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	buf.memory = V4L2_MEMORY_MMAP;
	buf.index = 0;
	
	if (V4L2Camera::ioctl(VIDIOC_QBUF, &buf) == -1)
	{
		throw std::runtime_error("Could not query buffer.");
	}

	
	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	
	if (V4L2Camera::ioctl(VIDIOC_STREAMON, &type) == -1)
	{
		throw std::runtime_error("Could not start capturing.");
	}
		
	fd_set fds;
	FD_ZERO(&fds);
	FD_SET(_fd, &fds);
	
	struct timeval tv{ 0 };
	tv.tv_sec = 2;
	int r = select(_fd + 1, &fds, NULL, NULL, &tv);
	if (r == -1)
	{
		std::cout << "Capture timeout expired." << std::endl;
		return {};
	}
		 
	// buf.length = _bufferLength;	
	 	 	
	if (V4L2Camera::ioctl(VIDIOC_DQBUF, &buf) == -1)
	{
		throw std::runtime_error("Could not read frame data from buffer.");
	}
	
	// trace message
	/*
	std::cout << " captured " << buf.bytesused
			<< " bytes, buffer length " << _bufferLength << " bytes "
			<< std::endl; */
	
	return std::vector<unsigned char>(_buffer, _buffer + buf.bytesused);
}


int V4L2Camera::ioctl(int request, void* arg)
{
	assert(_fd != -1);
	
	int r = -1;
	do
	{
		r = ::ioctl(_fd, request, arg);
	}
	while (r == -1 && errno == EINTR);
	
	if (r == -1)
	{
		perror("ioctl()");
	}
	
	return r;
}
