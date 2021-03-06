#include "Timer.h"
#include "PointCloudProcess.h"
#include "Kinect2Grabber.h"
#include "SceneRegistration.h"
#include "TsdfVolume.h"
#include "Transmission.h"
#include <pcl/gpu/features/features.hpp>
#include <pcl/visualization/cloud_viewer.h>
#include <pcl/io/pcd_io.h>
#include <pcl/compression/octree_pointcloud_compression.h>
#include <pcl/gpu/utils/safe_call.hpp>
#include <pcl/console/parse.h>
#include <pcl/io/vtk_lib_io.h>
#include <pcl/visualization/pcl_visualizer.h>
#include <pcl/conversions.h>
#include <pcl/surface/vtk_smoothing/vtk_utils.h>
#include <windows.h>

//#define CREATE_EXE

const int BUFFER_SIZE = 100000000;
byte* buffer = NULL;
pcl::Kinect2Grabber* grabber = NULL;
TsdfVolume* volume = NULL;
pcl::PointCloud<pcl::PointXYZRGB>::Ptr cloud;
boost::shared_ptr<pcl::visualization::PCLVisualizer> viewer;

Transmission* transmission = NULL;
UINT16* depthList[2];
RGBQUAD* colorList[2];
Eigen::Matrix4f transformationList[2];

void registration() {
	pcl::PointCloud<pcl::PointXYZRGB>::Ptr local = grabber->getPointCloud(depthList[0], colorList[0]);
	pcl::PointCloud<pcl::PointXYZRGB>::Ptr remote = grabber->getPointCloud(depthList[1], colorList[1]);
	transformationList[1] = SceneRegistration::align(local, remote);
}

void setBackground() {
	grabber->updateBackground();
}

void saveScene() {
	pcl::io::savePCDFileASCII("scene.pcd", *cloud);
}

void keyboardEventOccurred(const pcl::visualization::KeyboardEvent& event) {
	if (event.getKeySym() == "r" && event.keyDown()) {
		registration();
	}
	if (event.getKeySym() == "b" && event.keyDown()) {
		setBackground();
	}
	if (event.getKeySym() == "s" && event.keyDown()) {
		saveScene();
	}
}

void startViewer() {
	viewer = boost::shared_ptr<pcl::visualization::PCLVisualizer>(new pcl::visualization::PCLVisualizer("Point Cloud Viewer"));
	viewer->setCameraPosition(0.0, 0.0, -2.0, 0.0, 0.0, 0.0);
	viewer->registerKeyboardCallback(keyboardEventOccurred);
}

DWORD WINAPI TransmissionRecvThread(LPVOID pM)
{
	while (true) {
		Sleep(1);
		transmission->recvRGBD(depthList[1], colorList[1]);
	}
	return 0;
}

void start() {
	omp_set_num_threads(4);
	omp_set_nested(6);
	
	grabber = new pcl::Kinect2Grabber();
	cloud = pcl::PointCloud<pcl::PointXYZRGB>::Ptr(new pcl::PointCloud<pcl::PointXYZRGB>());
	transformationList[0].setIdentity();
	transformationList[1].setIdentity();
	volume = new TsdfVolume(512, 512, 512, 1, 1, 1, 0, 0, 0.5);
	buffer = new byte[BUFFER_SIZE];

	transmission = new Transmission();
	CreateThread(NULL, 0, TransmissionRecvThread, NULL, 0, NULL);
}

void update() {
	Timer timer;

	grabber->getDepthAndColor(depthList[0], colorList[0]);
	transmission->sendRGBD(depthList[0], colorList[0]);

	volume->integrate(2, depthList, colorList, transformationList);
	volume->calnMesh(buffer);

	timer.outputTime();
}

void stop() {
	if (grabber != NULL) {
		delete grabber;
	}
	if (volume != NULL) {
		delete volume;
	}
	if (buffer != NULL) {
		delete[] buffer;
	}
	if (transmission != NULL) {
		delete transmission;
	}
}

#ifdef CREATE_EXE

int main(int argc, char *argv[]) {
	start();
	startViewer();

	while (!viewer->wasStopped()) {
		viewer->spinOnce();

		update();

		cloud = volume->getPointCloudFromMesh(buffer);
		if (!viewer->updatePointCloud(cloud, "cloud")) {
			viewer->addPointCloud(cloud, "cloud");
		}
	}

	stop();
	return 0;
}

#else
extern "C" {
	__declspec(dllexport) void callStart() {
		start();
	}

	__declspec(dllexport) byte* callUpdate() {
		update();
		return buffer;
	}

	__declspec(dllexport) void callRegistration() {
		registration();
	}

	__declspec(dllexport) void callSetBackground() {
		setBackground();
	}

	__declspec(dllexport) void callSaveScene() {
		saveScene();
	}

	__declspec(dllexport) void callStop() {
		stop();
	}
}
#endif
