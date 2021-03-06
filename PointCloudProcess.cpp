#include "PointCloudProcess.h"
#include <pcl/kdtree/kdtree_flann.h>
#include <pcl/surface/mls.h>
#include <pcl/io/pcd_io.h>
#include <pcl/point_cloud.h>
#include <pcl/correspondence.h>
#include <pcl/features/normal_3d_omp.h>
#include <pcl/features/shot_omp.h>
#include <pcl/features/board.h>
#include <pcl/filters/uniform_sampling.h>
#include <pcl/recognition/cg/hough_3d.h>
#include <pcl/recognition/cg/geometric_consistency.h>
#include <pcl/visualization/pcl_visualizer.h>
#include <pcl/kdtree/impl/kdtree_flann.hpp>
#include <pcl/common/transforms.h>
#include <pcl/console/parse.h>
#include <pcl/gpu/features/features.hpp>
#include <pcl/surface/gp3.h>
#include <pcl/gpu/utils/safe_call.hpp>
#include "Timer.h"

void PointCloudProcess::mlsFiltering(pcl::PointCloud<pcl::PointXYZRGB>::Ptr cloud)
{
	// 370 ms (for unorganized point cloud)
	pcl::search::KdTree<pcl::PointXYZRGB>::Ptr tree(new pcl::search::KdTree<pcl::PointXYZRGB>);
	pcl::PointCloud<pcl::PointXYZRGBNormal> mlsPoints;
	pcl::MovingLeastSquaresOMP<pcl::PointXYZRGB, pcl::PointXYZRGBNormal> mls;
	mls.setNumberOfThreads(8);
	mls.setComputeNormals(true);
	mls.setInputCloud(cloud);
	mls.setPolynomialFit(true);
	mls.setSearchMethod(tree);
	mls.setSearchRadius(0.01);
	mls.setPolynomialOrder(1);

	Timer timer;
	timer.reset();
	mls.process(mlsPoints);
	std::cout << timer.getTime() * 1e3f << " ms" << std::endl;

	cloud->points.resize(mlsPoints.size());
	for (int i = 0; i < cloud->size(); i++) {
		cloud->points[i].x = mlsPoints.points[i].x;
		cloud->points[i].y = mlsPoints.points[i].y;
		cloud->points[i].z = mlsPoints.points[i].z;
		cloud->points[i].r = mlsPoints.points[i].r;
		cloud->points[i].g = mlsPoints.points[i].g;
		cloud->points[i].b = mlsPoints.points[i].b;
	}
	cloud->width = cloud->size();
	cloud->height = 1;
}

void PointCloudProcess::merge2PointClouds(pcl::PointCloud<pcl::PointXYZRGBNormal>::Ptr cloud, pcl::PointCloud<pcl::PointXYZRGBNormal>::ConstPtr cloud1, pcl::PointCloud<pcl::PointXYZRGBNormal>::ConstPtr cloud2)
{
	const int RADIUS_NEIGHBOR = 20;

	if (cloud1->size() == 0 || cloud2->size() == 0) {
		return;
	}

	pcl::PointCloud<pcl::PointXYZ> points1;
	pcl::PointCloud<pcl::PointXYZ> points2;
	#pragma omp parallel sections
	{
		#pragma omp section
		{
			pcl::copyPointCloud(*cloud1, points1);
		}
		#pragma omp section
		{
			pcl::copyPointCloud(*cloud2, points2);
		}
	}

	std::vector<int> neighbors1;
	std::vector<int> neighbors2;
	#pragma omp parallel sections
	{
		#pragma omp section
		{
			cudaSetDevice(0);
			pcl::gpu::Octree::PointCloud points1_device;
			pcl::gpu::Octree::PointCloud points2_device;
			points1_device.upload(points1.points);
			points2_device.upload(points2.points);

			pcl::gpu::Octree octree;
			octree.setCloud(points2_device);
			octree.build();

			pcl::gpu::NeighborIndices neighbors_device;
			octree.radiusSearch(points1_device, 0.005f, RADIUS_NEIGHBOR, neighbors_device);
			neighbors_device.data.download(neighbors1);
		}
		#pragma omp section
		{
			cudaSetDevice(1);
			pcl::gpu::Octree::PointCloud points1_device;
			pcl::gpu::Octree::PointCloud points2_device;
			points1_device.upload(points1.points);
			points2_device.upload(points2.points);

			pcl::gpu::Octree octree;
			octree.setCloud(points1_device);
			octree.build();

			pcl::gpu::NeighborIndices neighbors_device;
			octree.radiusSearch(points2_device, 0.005f, RADIUS_NEIGHBOR, neighbors_device);
			neighbors_device.data.download(neighbors2);
		}
	}
	cudaSetDevice(1);


	std::vector<int> indices1(points1.size());
	std::vector<int> indices2(points2.size());
#pragma omp parallel for schedule(dynamic, 500)
	for (int i = 0; i < points1.size(); i++) {
		float minDist2 = 1e10;
		for (int k = 0; k < RADIUS_NEIGHBOR; k++) {
			int j = neighbors1[i * RADIUS_NEIGHBOR + k];
			if (j == 0 || j >= points2.size()) {
				break;
			}
			float dist2 = squaredDistance(points1[i], points2[j]);
			if (dist2 < minDist2) {
				minDist2 = dist2;
				indices1[i] = j;
			}
		}
	}

#pragma omp parallel for schedule(dynamic, 500)
	for (int j = 0; j < points2.size(); j++) {
		float minDist2 = 1e10;
		for (int k = 0; k < RADIUS_NEIGHBOR; k++) {
			int i = neighbors2[j * RADIUS_NEIGHBOR + k];
			if (i == 0 || i >= points1.size()) {
				break;
			}
			float dist2 = squaredDistance(points1[i], points2[j]);
			if (dist2 < minDist2) {
				minDist2 = dist2;
				indices2[j] = i;
			}
		}
	}


	cloud->resize(cloud1->size() + cloud2->size());
#pragma omp parallel for
	for (int i = 0; i < cloud1->size(); i++) {
		int j = indices1[i];
		if (indices2[j] == i && checkMerge(cloud1->points[i], cloud2->points[j])) {
			pcl::PointXYZRGBNormal* pt = &cloud->points[i];
			pt->x = (points1[i].x + points2[j].x) / 2;
			pt->y = (points1[i].y + points2[j].y) / 2;
			pt->z = (points1[i].z + points2[j].z) / 2;
			pt->r = ((UINT16)cloud1->points[i].r + cloud2->points[j].r) >> 1;
			pt->g = ((UINT16)cloud1->points[i].g + cloud2->points[j].g) >> 1;
			pt->b = ((UINT16)cloud1->points[i].b + cloud2->points[j].b) >> 1;
			pt->normal_x = (cloud1->points[i].normal_x + cloud2->points[j].normal_x) / 2;
			pt->normal_y = (cloud1->points[i].normal_y + cloud2->points[j].normal_y) / 2;
			pt->normal_z = (cloud1->points[i].normal_z + cloud2->points[j].normal_z) / 2;
		} else {
			cloud->points[i] = cloud1->points[i];
		}
	}
	pcl::PointXYZRGBNormal* pt = &cloud->points[cloud1->size()];
	for (int j = 0; j < cloud2->size(); j++) {
		int i = indices2[j];
		if (indices1[i] == j && checkMerge(cloud1->points[i], cloud2->points[j])) {
			
		} else {
			*pt = cloud2->points[j];
			pt++;
		}
	}
	cloud->resize(pt - &cloud->points[0]);
}

void PointCloudProcess::pointCloud2Mesh(pcl::PolygonMesh::Ptr mesh, pcl::PointCloud<pcl::PointXYZRGB>::Ptr cloud)
{
	pcl::NormalEstimation<pcl::PointXYZRGB, pcl::Normal> normalEstimation;
	pcl::PointCloud<pcl::Normal>::Ptr normals(new pcl::PointCloud<pcl::Normal>);
	pcl::search::KdTree<pcl::PointXYZRGB>::Ptr kdTree(new pcl::search::KdTree<pcl::PointXYZRGB>);
	kdTree->setInputCloud(cloud);
	normalEstimation.setInputCloud(cloud);
	normalEstimation.setSearchMethod(kdTree);
	normalEstimation.setKSearch(10);
	normalEstimation.compute(*normals);

	pcl::PointCloud<pcl::PointXYZRGBNormal>::Ptr cloudWithNormals(new pcl::PointCloud<pcl::PointXYZRGBNormal>);
	pcl::concatenateFields(*cloud, *normals, *cloudWithNormals);

	pcl::search::KdTree<pcl::PointXYZRGBNormal>::Ptr tree2(new pcl::search::KdTree<pcl::PointXYZRGBNormal>);
	tree2->setInputCloud(cloudWithNormals);

	pcl::GreedyProjectionTriangulation<pcl::PointXYZRGBNormal> gp3;

	gp3.setSearchRadius(0.025);

	gp3.setMu(2.5);
	gp3.setMaximumNearestNeighbors(50);
	gp3.setMaximumSurfaceAngle(M_PI / 4);
	gp3.setMinimumAngle(M_PI / 18);
	gp3.setMaximumAngle(M_PI * 2 / 3);
	gp3.setNormalConsistency(false);

	gp3.setInputCloud(cloudWithNormals);
	gp3.setSearchMethod(tree2);
	gp3.reconstruct(*mesh);
}

void PointCloudProcess::pointCloud2PCNormal(pcl::PointCloud<pcl::PointXYZRGBNormal>::Ptr pcNormal, pcl::PointCloud<pcl::PointXYZRGB>::Ptr cloud)
{
	int N = 0;
	int* index = new int[cloud->size()];
	for (int i = 0, id = 0; i < cloud->size(); i++) {
		if (cloud->points[i].x != 0 && pcl_isfinite(cloud->points[i].x)) {
			index[i] = ++N;
		} else {
			index[i] = 0;
		}
	}

	std::vector<int> neighbors(N * 25);
	std::vector<int> sizes(N);
	int H = cloud->height;
	int W = cloud->width;
#pragma omp parallel for schedule(dynamic, 1)
	for (int y = 0; y < H; y++) {
		for (int x = 0; x < W; x++) {
			int id = y * W + x;
			if (index[id] != 0) {
				int cnt = 0;
				for (int dx = -2; dx <= 2; dx++) {
					for (int dy = -2; dy <= 2; dy++) {
						int xSearch = x + dx;
						int ySearch = y + dy;

						if (0 <= xSearch && xSearch < W && 0 <= ySearch && ySearch < H) {
							int searchIndex = ySearch * W + xSearch;
							if (index[searchIndex] != 0) {
								neighbors[(index[id] - 1) * 25 + (cnt++)] = index[searchIndex] - 1;
							}
						}
					}
				}
				sizes[index[id] - 1] = cnt;
			}
		}
	}

	pcNormal->resize(N);
	pcNormal->width = pcNormal->size();
	pcNormal->height = 1;
#pragma omp parallel for schedule(static, 500)
	for (int i = 0; i < cloud->size(); i++) {
		if (index[i] != 0) {
			pcl::PointXYZRGB* pt = &cloud->points[i];
			pcl::PointXYZRGBNormal* pt2 = &pcNormal->points[index[i] - 1];
			pt2->x = pt->x;
			pt2->y = pt->y;
			pt2->z = pt->z;
			pt2->r = pt->r;
			pt2->g = pt->g;
			pt2->b = pt->b;
		}
	}
	delete[] index;

	if (pcNormal->size() == 0) {
		return;
	}

	pcl::PointCloud<pcl::PointXYZ> points;
	pcl::copyPointCloud(*pcNormal, points);
	pcl::gpu::NormalEstimation::PointCloud cloud_device;
	cloud_device.upload(points.points);
	pcl::gpu::NeighborIndices neighbors_device;
	neighbors_device.upload(neighbors, sizes, 25);

	pcl::gpu::NormalEstimation ne_device;
	pcl::gpu::NormalEstimation::Normals normals_device;
	ne_device.computeNormals(cloud_device, neighbors_device, normals_device);

	std::vector<pcl::PointXYZ> downloaded;
	normals_device.download(downloaded);
	
	for (int i = 0; i < downloaded.size(); i++) {
		//Make normals face to the camera (z < 0)
		if (downloaded[i].z < 0) {
			pcNormal->points[i].normal_x = downloaded[i].x;
			pcNormal->points[i].normal_y = downloaded[i].y;
			pcNormal->points[i].normal_z = downloaded[i].z;
		} else {
			pcNormal->points[i].normal_x = -downloaded[i].x;
			pcNormal->points[i].normal_y = -downloaded[i].y;
			pcNormal->points[i].normal_z = -downloaded[i].z;
		}
	}
}

inline float PointCloudProcess::squaredDistance(const pcl::PointXYZ& pt1, const pcl::PointXYZ& pt2)
{
	return (pt1.x - pt2.x) * (pt1.x - pt2.x) + (pt1.y - pt2.y) * (pt1.y - pt2.y) + (pt1.z - pt2.z) * (pt1.z - pt2.z);
}

inline bool PointCloudProcess::checkMerge(const pcl::PointXYZRGBNormal & pt1, const pcl::PointXYZRGBNormal & pt2)
{
	const float MERGE_DIST = 0.005;
	const float MERGE_DOT = 0.8;

	float dot = pt1.normal_x * pt2.normal_x + pt1.normal_y * pt2.normal_y + pt1.normal_z * pt2.normal_z;
	float normal_x;
	float normal_y;
	float normal_z;
	if (dot > MERGE_DOT) {
		normal_x = (pt1.normal_x + pt2.normal_x) / 2;
		normal_y = (pt1.normal_y + pt2.normal_y) / 2;
		normal_z = (pt1.normal_z + pt2.normal_z) / 2;
	} else if (dot < -MERGE_DOT) {
		normal_x = (pt1.normal_x - pt2.normal_x) / 2;
		normal_y = (pt1.normal_y - pt2.normal_y) / 2;
		normal_z = (pt1.normal_z - pt2.normal_z) / 2;
	} else {
		return false;
	}

	float d = (pt1.x - pt2.x) * normal_x + (pt1.y - pt2.y) * normal_y + (pt1.z - pt2.z) * normal_z;

	return -MERGE_DIST <= d && d <= MERGE_DIST;
}
