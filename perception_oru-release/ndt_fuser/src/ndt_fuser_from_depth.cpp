// Evaluation using TUM data set.
#include <ndt_fuser/ndt_fuser.h>
#include <pointcloud_vrml/pointcloud_utils.h>

#include <iostream>
#include <boost/thread/thread.hpp>
#include <boost/program_options.hpp>

#include <cv.h>
#include <highgui.h>
#include <pcl/io/io.h>
#include <pcl/io/pcd_io.h>
#include <pcl/common/transforms.h>

using namespace pcl;
using namespace cv;
using namespace std;
using namespace lslgeneric;

namespace po = boost::program_options;

inline std::vector<std::string> split_string(const std::string &str, const std::string &separator)
{
	std::string::size_type it = 0;
	std::vector<std::string> values;
	bool stop = false;
	while (!stop)
	{
		std::string::size_type start = it;
		it = str.find(separator, it + 1); // Gets the next comma.
		if (it == std::string::npos)	  // didn't find any new comma, return the remaining string.
		{
			values.push_back(str.substr(start, std::string::npos));
			stop = true;
		}
		else
		{
			it++; // We want the character after the comma.
			values.push_back(str.substr(start, it - start - 1));
		}
	}
	return values;
}

vector<pair<string, string>> load_associations(const string &association_file, vector<string> &timestamps, int skip_nb)
{
	// Generated by associate.py rgb.txt depth.txt (always this order)
	vector<pair<string, string>> ret;
	std::ifstream file(association_file.c_str());
	std::string line;
	bool skip = false;
	int skip_counter = 0;
	while (std::getline(file, line))
	{
		vector<string> splits = split_string(line, std::string(" "));
		if (splits.size() < 4)
		{
			cerr << "couldn't parse : " << line << endl;
			continue;
		}
		if (!skip)
		{
			ret.push_back(pair<string, string>(splits[1], splits[3])); // rgb, depth
			timestamps.push_back(splits[0]);						   // Timestamp for the rgb.
		}
		/*
        if (skip_counter > skip_nb)
        {
            skip = false;
            skip_counter = 0;
        }
        else
        {
            skip = true;
            skip_counter++;
        }
	*/
	}
	return ret;
}
double getDoubleTime()
{
	struct timeval time;
	gettimeofday(&time, NULL);
	return time.tv_sec + time.tv_usec * 1e-6;
}

int main(int argc, char **argv)
{
	cout << "-----------------------------------------------------------------" << endl;
	cout << "Evaluations of frame to model tracking with ndt d2d registration" << endl;
	cout << "-----------------------------------------------------------------" << endl;

	string association_name;
	string output_name;
	string times_name;
	double scale, max_inldist_xy, max_inldist_z;
	Eigen::Matrix<double, 6, 1> pose_increment_v;
	int nb_ransac;
	int nb_points;
	size_t support_size;
	size_t max_nb_frames;
	double max_var;
	double current_res;
	int delay_vis;
	double img_scale;
	double detector_thresh;
	int skip_nb = 0;
	int freiburg_camera_nb;
	double max_kp_dist, min_kp_dist;
	double trim_factor;
	double resolution;
	double size_x, size_y, size_z;
	Eigen::Affine3d pose_, T0;
	pose_.setIdentity();
	T0.setIdentity();

	po::options_description desc("Allowed options");
	desc.add_options()("help", "produce help message")("debug", "print debug output")("visualize", "visualize the output")("init-gt", "use the first pose of gt as the start pose of the transform track")("association", po::value<string>(&association_name), "association file name (generated by association.py script)")("output", po::value<string>(&output_name), "file name of the estimation output (to be used with the evaluation python script)")("time_output", po::value<string>(&times_name), "file name for saving the per-frame processing time (to be used with the evaluation python script)")("scale", po::value<double>(&scale)->default_value(0.0002), "depth scale (depth = scale * pixel value)")("freiburg_camera_nb", po::value<int>(&freiburg_camera_nb)->default_value(1), "the camera used for the evaluation 1-> freiburg camera #1, 2-> freiburg camera #2")("resolution", po::value<double>(&resolution)->default_value(0.1), "iresolution of the ndt model")("map_size_x", po::value<double>(&size_x)->default_value(5.), "x size of the map")("map_size_y", po::value<double>(&size_y)->default_value(5.), "y size of the map")("map_size_z", po::value<double>(&size_z)->default_value(5.), "z size of the map");

	po::variables_map vm;
	po::store(po::parse_command_line(argc, argv, desc), vm);
	po::notify(vm);

	if (!vm.count("output") || !vm.count("association"))
	{
		cout << "Missing association / output file names.\n";
		cout << desc << "\n";
		return 1;
	}
	if (vm.count("help"))
	{
		cout << desc << "\n";
		return 1;
	}
	bool debug = vm.count("debug");
	bool visualize = vm.count("visualize");

	if (times_name == "")
	{
		times_name = output_name + ".times";
	}

	if (vm.count("init-gt"))
	{
		cout << "reading gt from groundtrut.txt\n";
		std::ifstream file("groundtruth.txt");
		std::string line;
		double dx, dy, dz, qx, qy, qz, qw;
		while (std::getline(file, line))
		{
			vector<string> splits = split_string(line, std::string(" "));
			if (splits.size() == 0)
				return -1;
			if (splits[0][0] == '#')
				continue;
			if (splits.size() < 8)
				continue;
			dx = atof(splits[1].c_str());
			dy = atof(splits[2].c_str());
			dz = atof(splits[3].c_str());
			qx = atof(splits[4].c_str());
			qy = atof(splits[5].c_str());
			qz = atof(splits[6].c_str());
			qw = atof(splits[7].c_str());
			break;
		}
		cerr << "using init pose of " << dx << " " << dy << " " << dz << " " << qx << " " << qy << " " << qz << " " << qw << endl;
		Eigen::Quaterniond quat(qx, qy, qz, qw);
		pose_ = Eigen::Translation<double, 3>(dx, dy, dz) * quat;
	}

	vector<string> timestamps;
	vector<pair<string, string>> associations = load_associations(association_name, timestamps, skip_nb);
	if (debug)
	{
		for (size_t i = 0; i < associations.size(); i++)
		{
			cout << "associations : " << associations[i].first << " <-> " << associations[i].second << endl;
		}
		cout << "loaded : " << associations.size() << " associations" << endl;
	}

	double fx, fy, cx, cy, ds;
	std::vector<double> dist(5);
	switch (freiburg_camera_nb)
	{
	case 1:
		fx = 517.3;
		fy = 516.5;
		cx = 318.6;
		cy = 255.3;
		dist[0] = 0.2624;
		dist[1] = -0.9531;
		dist[2] = -0.0054;
		dist[3] = 0.0026;
		dist[4] = 1.1633;
		ds = 1.035; // Depth scaling factor.
		break;
	case 2:
		fx = 520.9;
		fy = 521.0;
		cx = 325.1;
		cy = 249.7;
		dist[0] = 0.2312;
		dist[1] = -0.7849;
		dist[2] = -0.0033;
		dist[3] = -0.0001;
		dist[4] = 0.9172;
		ds = 1.031;
		break;
	default:
		cerr << "unknown camera number : " << freiburg_camera_nb << endl;
		return 1;
	}

	double t1, t2;

	lslgeneric::DepthCamera cameraparams(fx, fy, cx, cy, dist, ds * scale, false);

	typedef Eigen::Transform<double, 3, Eigen::Affine, Eigen::ColMajor> EigenTransform;
	std::vector<EigenTransform, Eigen::aligned_allocator<EigenTransform>> transformVector;

	std::ofstream file_times(times_name.c_str(), ios::out);
	if (!file_times)
	{
		cerr << "Failed to open times file : " << times_name << endl;
		return -1;
	}

	file_times << "times = [";
	NDTFuser fuser(resolution, size_x, size_y, size_z, visualize);
	pcl::PointCloud<pcl::PointXYZ> pc;
	cv::Mat depth_img;

	// Loop through all the associations pairs
	for (size_t i = 0; i < associations.size(); i++)
	{
		std::cout << "iter " << i << "(" << associations.size() << ")" << std::endl;
		depth_img = cv::imread(associations[i].second, CV_LOAD_IMAGE_ANYDEPTH); // CV_LOAD_IMAGE_ANYDEPTH is important to load the 16bits image
		t1 = getDoubleTime();
		if (i == 0)
		{
			cameraparams.setupDepthPointCloudLookUpTable(depth_img.size());
			cameraparams.convertDepthImageToPointCloud(depth_img, pc);
			fuser.initialize(pose_, pc);
			transformVector.push_back(pose_);
			t2 = getDoubleTime();
			file_times << t2 - t1 << ", ";

			continue;
		}

		cameraparams.convertDepthImageToPointCloud(depth_img, pc);
		pose_ = fuser.update(T0, pc);
		transformVector.push_back(pose_);

		t2 = getDoubleTime();
		file_times << t2 - t1 << ", ";
	}
	file_times << "];\n";

	// Write out the transformations
	assert(associations.size() == transformVector.size());
	Eigen::Transform<double, 3, Eigen::Affine, Eigen::ColMajor> global_transform;
	global_transform.setIdentity();

	std::ofstream file(output_name.c_str(), ios::out);
	if (!file)
	{
		cerr << "Failed to create file : " << output_name << endl;
		return -1;
	}

	for (size_t i = 0; i < transformVector.size(); i++)
	{
		global_transform = transformVector[i];
		Eigen::Quaternion<double> tmp(global_transform.rotation());
		cout << timestamps[i] << " " << global_transform.translation().transpose() << " " << tmp.x() << " " << tmp.y() << " " << tmp.z() << " " << tmp.w() << endl;
		file << timestamps[i] << " " << global_transform.translation().transpose() << " " << tmp.x() << " " << tmp.y() << " " << tmp.z() << " " << tmp.w() << endl;
	}
	file.close();

	return 0;
}

